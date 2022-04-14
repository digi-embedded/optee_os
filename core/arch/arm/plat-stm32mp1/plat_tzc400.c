// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2019-2021, STMicroelectronics
 */

#include <assert.h>
#include <config.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <drivers/stm32mp_dt_bindings.h>
#include <drivers/tzc400.h>
#include <initcall.h>
#include <io.h>
#include <keep.h>
#include <kernel/dt.h>
#include <kernel/interrupt.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <kernel/spinlock.h>
#include <kernel/tee_misc.h>
#include <libfdt.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <stm32_util.h>
#include <trace.h>
#include <util.h>

struct stm32mp_tzc_region {
	uint32_t cfg;
	uint32_t addr;
	uint32_t len;
};

struct stm32mp_tzc_platdata {
	const char *name;
	uintptr_t base;
	struct clk *clk[2];
	int irq;
	uint32_t mem_base;
	uint32_t mem_size;
	struct firewall_compat *tzc_compat;
	struct stm32mp_tzc_region *regions;
};

struct stm32mp_tzc_driver_data {
	uint32_t nb_filters;
	uint32_t nb_regions;
};

struct tzc_device {
	struct stm32mp_tzc_platdata pdata;
	struct stm32mp_tzc_driver_data *ddata;
	struct itr_handler *itr;
	struct tzc_region_config *reg;
	uint32_t nb_reg_used;
	unsigned int lock;
};

#define IS_PAGE_ALIGNED(addr)		(((addr) & SMALL_PAGE_MASK) == 0)
#define filter_mask(_width)		GENMASK_32(((_width) - 1U), 0U)

struct tzc_region_non_sec {
	struct tzc_region_config region;
	SLIST_ENTRY(tzc_region_non_sec) link;
};

static SLIST_HEAD(nsec_list_head, tzc_region_non_sec) nsec_region_list =
	SLIST_HEAD_INITIALIZER(nsec_list_head);

static enum itr_return tzc_it_handler(struct itr_handler *handler __unused)
{
	EMSG("TZC permission failure");
	tzc_fail_dump();
	stm32mp_dump_core_registers(true);

	if (IS_ENABLED(CFG_STM32MP_PANIC_ON_TZC_PERM_VIOLATION))
		panic();
	else
		tzc_int_clear();

	return ITRR_HANDLED;
}

static TEE_Result tzc_region_check_overlap(struct tzc_device *tzc_dev,
					   const struct tzc_region_config *reg)
{
	unsigned int i = 0;

	/* Check if base address already defined in another region */
	for (i = 0; i < tzc_dev->nb_reg_used; i++)
		if (reg->base <= tzc_dev->reg[i].top &&
		    reg->top >= tzc_dev->reg[i].base)
			return TEE_ERROR_ACCESS_CONFLICT;

	return TEE_SUCCESS;
}

static void tzc_set_driverdata(struct tzc_device *tzc_dev)
{
	struct stm32mp_tzc_driver_data *ddata = tzc_dev->ddata;
	uintptr_t base = tzc_dev->pdata.base;
	uint32_t regval = 0;

	clk_enable(tzc_dev->pdata.clk[0]);

	regval = io_read32(base + BUILD_CONFIG_OFF);
	ddata->nb_filters = ((regval >> BUILD_CONFIG_NF_SHIFT) &
			     BUILD_CONFIG_NF_MASK) + 1;
	ddata->nb_regions = ((regval >>	BUILD_CONFIG_NR_SHIFT) &
			     BUILD_CONFIG_NR_MASK);

	clk_disable(tzc_dev->pdata.clk[0]);

	DMSG("TZC400 Filters %i Regions %i\n", ddata->nb_filters,
	     ddata->nb_regions);
}

static struct tzc_device *tzc_alloc(void)
{
	struct tzc_device *tzc_dev = NULL;
	struct stm32mp_tzc_driver_data *ddata = NULL;

	tzc_dev = calloc(1, sizeof(*tzc_dev));
	ddata = calloc(1, sizeof(*ddata));

	if (tzc_dev && ddata) {
		tzc_dev->ddata = ddata;
		return tzc_dev;
	}

	free(ddata);
	free(tzc_dev);

	return NULL;
}

static void tzc_free(struct tzc_device *tzc_dev)
{
	if (tzc_dev) {
		free(tzc_dev->ddata);
		free(tzc_dev);
	}
}

static void stm32mp_tzc_region0(bool enable)
{
	struct tzc_region_config region_cfg_0 = {
		.base = 0,
		.top = UINT_MAX,
		.sec_attr = TZC_REGION_S_NONE,
		.ns_device_access = 0,
	};

	if (enable)
		region_cfg_0.sec_attr = TZC_REGION_S_RDWR;

	tzc_configure_region(0, &region_cfg_0);
}

static void stm32mp_tzc_reset_region(struct tzc_device *tzc_dev)
{
	unsigned int i = 0;
	const struct tzc_region_config cfg = { .top = 0x00000FFF };

	/* Clean old configuration */
	for (i = 0; i < tzc_dev->ddata->nb_regions; i++)
		tzc_configure_region(i + 1, &cfg);
}

static TEE_Result stm32mp_tzc_region_append(struct tzc_device *tzc_dev,
					    const struct tzc_region_config
					    *region_cfg)
{
	TEE_Result res = TEE_SUCCESS;
	unsigned int index = 0;
	uint32_t exceptions = 0;

	exceptions = may_spin_lock(&tzc_dev->lock);
	index = tzc_dev->nb_reg_used;

	if (index >= tzc_dev->ddata->nb_regions ||
	    region_cfg->base < tzc_dev->pdata.mem_base ||
	    region_cfg->top > tzc_dev->pdata.mem_base +
	    (tzc_dev->pdata.mem_size - 1)) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto end;
	}

	res = tzc_region_check_overlap(tzc_dev, region_cfg);
	if (res)
		goto end;

	memcpy(&tzc_dev->reg[index], region_cfg,
	       sizeof(struct tzc_region_config));

	tzc_configure_region(index + 1, region_cfg);

	tzc_dev->nb_reg_used++;

end:
	may_spin_unlock(&tzc_dev->lock, exceptions);

	return res;
}

static TEE_Result
exclude_region_from_nsec(const struct tzc_region_config *reg_exclude)
{
	struct tzc_region_non_sec *reg = NULL;
	bool found = false;

	SLIST_FOREACH(reg, &nsec_region_list, link) {
		found = core_is_buffer_inside(reg_exclude->base,
					      reg_exclude->top -
					      reg_exclude->base + 1,
					      reg->region.base,
					      reg->region.top -
					      reg->region.base + 1);
		if (found)
			break;
	}

	if (!found)
		panic();

	if (reg_exclude->base == reg->region.base &&
	    reg_exclude->top == reg->region.top) {
		/* Remove this entry */
		SLIST_REMOVE(&nsec_region_list, reg, tzc_region_non_sec, link);
	} else if (reg_exclude->base == reg->region.base) {
		reg->region.base = reg_exclude->top + 1;
	} else if (reg_exclude->top == reg->region.top) {
		reg->region.top = reg_exclude->base - 1;
	} else {
		struct tzc_region_non_sec *new_nsec =
			calloc(1, sizeof(*new_nsec));

		if (!new_nsec || !reg)
			panic();

		memcpy((void *)&new_nsec->region, (void *)&reg->region,
		       sizeof(struct tzc_region_config));
		reg->region.top = reg_exclude->base - 1;
		new_nsec->region.base = reg_exclude->top + 1;
		SLIST_INSERT_AFTER(reg, new_nsec, link);
	}

	return TEE_SUCCESS;
}

static void stm32mp_tzc_cfg_boot_region(struct tzc_device *tzc_dev)
{
	unsigned int idx = 0;
	static struct tzc_region_config boot_region[] = {
		{
			.base = CFG_TZDRAM_START,
			.top = CFG_TZDRAM_START + CFG_TZDRAM_SIZE - 1,
			.sec_attr = TZC_REGION_S_RDWR,
			.ns_device_access = 0,
		},
#ifdef CFG_CORE_RESERVED_SHM
		{
			.base = CFG_SHMEM_START,
			.top = CFG_SHMEM_START + CFG_SHMEM_SIZE - 1,
			.sec_attr = TZC_REGION_S_NONE,
			.ns_device_access =
				TZC_REGION_ACCESS_RDWR(STM32MP1_TZC_A7_ID),
		}
#endif
	};

	COMPILE_TIME_ASSERT(IS_PAGE_ALIGNED(CFG_TZDRAM_START));
	COMPILE_TIME_ASSERT(IS_PAGE_ALIGNED(CFG_TZDRAM_SIZE));
#ifdef CFG_CORE_RESERVED_SHM
	COMPILE_TIME_ASSERT(IS_PAGE_ALIGNED(CFG_SHMEM_START));
	COMPILE_TIME_ASSERT(IS_PAGE_ALIGNED(CFG_SHMEM_SIZE));
#endif

	stm32mp_tzc_region0(true);

	stm32mp_tzc_reset_region(tzc_dev);

	for (idx = 0; idx < ARRAY_SIZE(boot_region); idx++) {
		TEE_Result res = TEE_ERROR_GENERIC;

		boot_region[idx].filters =
			filter_mask(tzc_dev->ddata->nb_filters);

		res = stm32mp_tzc_region_append(tzc_dev, &boot_region[idx]);
		if (res)
			panic("Enable to configure core regions");

		res = exclude_region_from_nsec(&boot_region[idx]);
	}

	/* Remove region0 access */
	stm32mp_tzc_region0(false);
}

static TEE_Result fdt_stm32mp_tzc_add_regions(struct tzc_device *tzc_dev,
					      const void *fdt, int node)
{
	const fdt32_t *conf_list = NULL;
	TEE_Result res = TEE_SUCCESS;
	unsigned int nregions = 0;
	unsigned int i = 0;
	int len = 0;

	conf_list = fdt_getprop(fdt, node, "memory-region", &len);
	if (!conf_list)
		return TEE_SUCCESS;

	nregions = len / sizeof(uint32_t);
	if (nregions > tzc_dev->ddata->nb_regions)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < nregions; i++) {
		struct tzc_region_config region_cfg = { };
		int pnode = 0;
		const fdt32_t *prop = NULL;
		uint32_t phandle = fdt32_to_cpu(*(conf_list + i));

		pnode = fdt_node_offset_by_phandle(fdt, phandle);
		if (pnode < 0)
			return TEE_ERROR_BAD_PARAMETERS;

		prop = fdt_getprop(fdt, pnode, "reg", NULL);
		if (!prop)
			return TEE_ERROR_BAD_PARAMETERS;

		if (!IS_PAGE_ALIGNED(fdt32_to_cpu(prop[0])) ||
		    !IS_PAGE_ALIGNED(fdt32_to_cpu(prop[1])))
			return TEE_ERROR_BAD_PARAMETERS;

		region_cfg.base = fdt32_to_cpu(prop[0]);
		region_cfg.top = region_cfg.base + (fdt32_to_cpu(prop[1]) - 1);
		region_cfg.filters = filter_mask(tzc_dev->ddata->nb_filters);

		prop = fdt_getprop(fdt, pnode, "st,protreg", &len);
		if (!prop || len != (2 * sizeof(uint32_t)))
			return TEE_ERROR_BAD_PARAMETERS;

		region_cfg.sec_attr = fdt32_to_cpu(prop[0]);
		region_cfg.ns_device_access = fdt32_to_cpu(prop[1]);

		DMSG("0x%#08"PRIxVA" - 0x%#08"PRIxVA" : Sec access %i NS access %#"PRIx32,
		     region_cfg.base, region_cfg.top, region_cfg.sec_attr,
		     region_cfg.ns_device_access);

		res = stm32mp_tzc_region_append(tzc_dev, &region_cfg);
		if (res)
			panic("Error adding region");

		res = exclude_region_from_nsec(&region_cfg);
		if (res)
			panic("Not able to exclude region");
	}

	return 0;
}

static TEE_Result stm32mp_tzc_parse_fdt(struct tzc_device *tzc_dev,
					const void *fdt, int node)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct dt_node_info dt_info = { };
	struct io_pa_va base = { };
	const fdt32_t *cuint = NULL;
	int offs = 0;

	_fdt_fill_device_info(fdt, &dt_info, node);
	if (dt_info.reg == DT_INFO_INVALID_REG ||
	    dt_info.reg_size == DT_INFO_INVALID_REG_SIZE ||
	    dt_info.clock == DT_INFO_INVALID_CLOCK ||
	    dt_info.interrupt == DT_INFO_INVALID_INTERRUPT)
		panic("Missing properties in TZC DT node");

	base.pa = dt_info.reg;
	tzc_dev->pdata.name = fdt_get_name(fdt, node, NULL);
	tzc_dev->pdata.base = io_pa_or_va_secure(&base, dt_info.reg_size);
	tzc_dev->pdata.irq = dt_info.interrupt;

	res = clk_dt_get_by_index(fdt, node, 0, tzc_dev->pdata.clk);
	if (res)
		return res;

	res = clk_dt_get_by_index(fdt, node, 1, tzc_dev->pdata.clk + 1);
	if (res || !tzc_dev->pdata.clk[1])
		DMSG("No secondary clock for %s",
		     fdt_get_name(fdt, node, NULL));

	/* Use memory node instead of that new one */
	offs = fdt_node_offset_by_prop_value(fdt, offs, "device_type",
					     "memory", sizeof("memory"));
	if (offs < 0)
		panic("No memory reference for TZC DT node");

	cuint = fdt_getprop(fdt, offs, "reg", NULL);
	if (!cuint)
		panic();

	tzc_dev->pdata.mem_base = fdt32_to_cpu(*cuint);
	tzc_dev->pdata.mem_size = fdt32_to_cpu(*(cuint + 1));

	return TEE_SUCCESS;
}

static TEE_Result stm32mp1_tzc_pm(enum pm_op op,
				  unsigned int pm_hint __unused,
				  const struct pm_callback_handle *hdl)
{
	unsigned int i = 0;
	struct tzc_device *tzc_dev =
		(struct tzc_device *)PM_CALLBACK_GET_HANDLE(hdl);

	if (op == PM_OP_RESUME) {
		stm32mp_tzc_region0(true);

		stm32mp_tzc_reset_region(tzc_dev);

		for (i = 0; i < tzc_dev->nb_reg_used; i++)
			tzc_configure_region(i + 1, &tzc_dev->reg[i]);
	}

	return TEE_SUCCESS;
}
DECLARE_KEEP_PAGER(stm32mp1_tzc_pm);

static TEE_Result stm32mp1_tzc_probe(const void *fdt, int node,
				     const void *compt_data __unused)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct tzc_device *tzc_dev = NULL;
	struct tzc_region_non_sec *nsec_region = NULL;
	struct tzc_region_non_sec *region_safe = NULL;

	assert(fdt && node >= 0);

	tzc_dev = tzc_alloc();
	if (!tzc_dev)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = stm32mp_tzc_parse_fdt(tzc_dev, fdt, node);
	if (res)
		goto err;

	if (tzc_dev->ddata) {
		tzc_set_driverdata(tzc_dev);
		tzc_dev->reg = calloc(tzc_dev->ddata->nb_regions,
			      sizeof(*tzc_dev->reg));
		if (!tzc_dev->reg) {
			res = TEE_ERROR_OUT_OF_MEMORY;
			goto err;
		}
	}

	clk_enable(tzc_dev->pdata.clk[0]);
	if (tzc_dev->pdata.clk[1])
		clk_enable(tzc_dev->pdata.clk[1]);

	tzc_init((vaddr_t)tzc_dev->pdata.base);

	nsec_region = calloc(1, sizeof(*nsec_region));
	if (!nsec_region) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto err;
	}

	nsec_region->region.base = tzc_dev->pdata.mem_base;
	nsec_region->region.top = tzc_dev->pdata.mem_base +
		tzc_dev->pdata.mem_size - 1;
	nsec_region->region.sec_attr = TZC_REGION_S_NONE;
	nsec_region->region.ns_device_access = TZC_REGION_NSEC_ALL_ACCESS_RDWR;
	nsec_region->region.filters = filter_mask(tzc_dev->ddata->nb_filters);

	SLIST_INSERT_HEAD(&nsec_region_list, nsec_region, link);

	stm32mp_tzc_cfg_boot_region(tzc_dev);

	res = fdt_stm32mp_tzc_add_regions(tzc_dev, fdt, node);
	if (res)
		goto err;

	SLIST_FOREACH_SAFE(nsec_region, &nsec_region_list, link, region_safe) {
		DMSG("0x%#08"PRIxVA" - 0x%#08"PRIxVA" : Sec access %i NS access %#"PRIx32,
		     nsec_region->region.base, nsec_region->region.top,
		     nsec_region->region.sec_attr,
		     nsec_region->region.ns_device_access);

		res = stm32mp_tzc_region_append(tzc_dev, &nsec_region->region);
		if (res)
			panic("Error adding region");

		SLIST_REMOVE(&nsec_region_list, nsec_region,
			     tzc_region_non_sec, link);
		free(nsec_region);
	};

	tzc_dump_state();

	tzc_dev->itr = itr_alloc_add(tzc_dev->pdata.irq, tzc_it_handler,
				     ITRF_TRIGGER_LEVEL, tzc_dev);
	if (!tzc_dev->itr)
		panic();

	itr_enable(tzc_dev->pdata.irq);
	tzc_set_action(TZC_ACTION_INT);

	register_pm_core_service_cb(stm32mp1_tzc_pm, tzc_dev,
				    "stm32mp1-tzc400");

	return TEE_SUCCESS;

err:
	if (res)
		tzc_free(tzc_dev);

	SLIST_FOREACH_SAFE(nsec_region, &nsec_region_list, link, region_safe) {
		SLIST_REMOVE(&nsec_region_list, nsec_region,
			     tzc_region_non_sec, link);
		free(nsec_region);
	};

	return res;
}

static const struct dt_device_match tzc_secu_match_table[] = {
	{ .compatible = "st,stm32mp1-tzc" },
	{ }
};

DEFINE_DT_DRIVER(tzc_stm32mp1_dt_driver) = {
	.name = "stm32mp1-tzc400",
	.type = DT_DRIVER_NOTYPE,
	.match_table = tzc_secu_match_table,
	.probe = stm32mp1_tzc_probe,
};
