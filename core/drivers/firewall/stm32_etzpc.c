// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2015-2017, ARM Limited and Contributors. All rights reserved.
 * Copyright (c) 2017-2024, STMicroelectronics
 */

/*
 * STM32 ETPZC acts as a firewall on stm32mp SoC peripheral interfaces and
 * internal memories. The driver expects a single instance of the controller
 * in the platform.
 */

#include <assert.h>
#include <drivers/clk_dt.h>
#include <drivers/firewall.h>
#include <drivers/firewall_device.h>
#include <drivers/stm32mp_dt_bindings.h>
#ifdef CFG_STM32MP15
#include <drivers/stm32mp1_rcc.h>
#endif
#include <initcall.h>
#include <io.h>
#include <keep.h>
#include <kernel/boot.h>
#include <kernel/dt.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <kernel/spinlock.h>
#include <kernel/tee_misc.h>
#include <libfdt.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <stm32_util.h>
#include <util.h>

/* ID Registers */
#define ETZPC_TZMA0_SIZE		U(0x000)
#define ETZPC_DECPROT0			U(0x010)
#define ETZPC_DECPROT_LOCK0		U(0x030)
#define ETZPC_HWCFGR			U(0x3F0)
#define ETZPC_VERR			U(0x3F4)

/* ID Registers fields */
#define ETZPC_TZMA0_SIZE_LOCK		BIT(31)
#define ETZPC_DECPROT0_MASK		GENMASK_32(1, 0)
#define ETZPC_HWCFGR_NUM_TZMA_MASK	GENMASK_32(7, 0)
#define ETZPC_HWCFGR_NUM_TZMA_SHIFT	0
#define ETZPC_HWCFGR_NUM_PER_SEC_MASK	GENMASK_32(15, 8)
#define ETZPC_HWCFGR_NUM_PER_SEC_SHIFT	8
#define ETZPC_HWCFGR_NUM_AHB_SEC_MASK	GENMASK_32(23, 16)
#define ETZPC_HWCFGR_NUM_AHB_SEC_SHIFT	16
#define ETZPC_HWCFGR_CHUNKS1N4_MASK	GENMASK_32(31, 24)
#define ETZPC_HWCFGR_CHUNKS1N4_SHIFT	24

#define DECPROT_SHIFT			1
#define IDS_PER_DECPROT_REGS		U(16)
#define IDS_PER_DECPROT_LOCK_REGS	U(32)

/*
 * Implementation uses uint8_t to store each securable DECPROT configuration
 * and uint16_t to store each securable TZMA configuration. When resuming
 * from deep suspend, the DECPROT configurations are restored.
 */
#define PERIPH_PM_LOCK_BIT		BIT(7)
#define PERIPH_PM_ATTR_MASK		GENMASK_32(2, 0)
#define TZMA_PM_LOCK_BIT		BIT(15)
#define TZMA_PM_VALUE_MASK		GENMASK_32(9, 0)

/* ETZPC DECPROT bit field values */
enum etzpc_decprot_attributes {
	ETZPC_DECPROT_S_RW = 0,
	ETZPC_DECPROT_NS_R_S_W = 1,
	ETZPC_DECPROT_MCU_ISOLATION = 2,
	ETZPC_DECPROT_NS_RW = 3,
	ETZPC_DECPROT_MAX = 4,
};

/*
 * struct stm32_etzpc_platdata - Driver data set at initialization
 *
 * @name:	Name of the peripheral
 * @clk:	ETZPC clock
 * @periph_cfg:	Peripheral DECPROT configuration
 * @tzma_cfg:	TZMA configuration
 * @base:	ETZPC IOMEM base address
 */
struct stm32_etzpc_platdata {
	const char *name;
	struct clk *clk;
	uint8_t *periph_cfg;
	uint16_t *tzma_cfg;
	struct io_pa_va base;
};

/*
 * struct stm32_etzpc_driver_data - configuration data from the hardware
 *
 * @num_tzma:	 Number of TZMA zones, read from the hardware
 * @num_per_sec: Number of securable AHB & APB periphs, read from the hardware
 * @num_ahb_sec: Number of securable AHB master zones, read from the hardware
 */
struct stm32_etzpc_driver_data {
	unsigned int num_tzma;
	unsigned int num_per_sec;
	unsigned int num_ahb_sec;
};

/*
 * struct etzpc_device - ETZPC device driver instance
 * @pdata:	Platform data set during initialization
 * @ddata:	Device configuration data from the hardware
 * @lock:	Access contention
 */
struct etzpc_device {
	struct stm32_etzpc_platdata pdata;
	struct stm32_etzpc_driver_data *ddata;
	unsigned int lock;
};

static const char *const etzpc_decprot_strings[] __maybe_unused = {
	[ETZPC_DECPROT_S_RW] = "ETZPC_DECPROT_S_RW",
	[ETZPC_DECPROT_NS_R_S_W] = "ETZPC_DECPROT_NS_R_S_W",
	[ETZPC_DECPROT_MCU_ISOLATION] = "ETZPC_DECPROT_MCU_ISOLATION",
	[ETZPC_DECPROT_NS_RW] = "ETZPC_DECPROT_NS_RW",
};

static uint32_t etzpc_lock(struct etzpc_device *dev)
{
	return may_spin_lock(&dev->lock);
}

static void etzpc_unlock(struct etzpc_device *dev, uint32_t exceptions)
{
	may_spin_unlock(&dev->lock, exceptions);
}

static bool valid_decprot_id(struct etzpc_device *etzpc_dev, unsigned int id)
{
	return id < etzpc_dev->ddata->num_per_sec;
}

static bool __maybe_unused valid_tzma_id(struct etzpc_device *etzpc_dev,
					 unsigned int id)
{
	return id < etzpc_dev->ddata->num_tzma;
}

static enum etzpc_decprot_attributes etzpc_binding2decprot(uint32_t mode)
{
	switch (mode) {
	case DECPROT_S_RW:
		return ETZPC_DECPROT_S_RW;
	case DECPROT_NS_R_S_W:
		return ETZPC_DECPROT_NS_R_S_W;
#ifdef CFG_STM32MP15
	case DECPROT_MCU_ISOLATION:
		return ETZPC_DECPROT_MCU_ISOLATION;
#endif
	case DECPROT_NS_RW:
		return ETZPC_DECPROT_NS_RW;
	default:
		panic();
	}
}

static void
sanitize_decprot_config(uint32_t decprot_id __maybe_unused,
			enum etzpc_decprot_attributes attr __maybe_unused)
{
#ifdef CFG_STM32MP15
	/*
	 * STM32MP15: check dependency on RCC TZEN/MCKPROT configuration
	 * when a ETZPC resource is secured or isolated for Cortex-M
	 * coprocessor.
	 */
	switch (attr) {
	case ETZPC_DECPROT_S_RW:
	case ETZPC_DECPROT_NS_R_S_W:
		if (!stm32_rcc_is_secure()) {
			IMSG("WARNING: RCC tzen:0, insecure ETZPC hardening %"PRIu32":%s",
			     decprot_id, etzpc_decprot_strings[attr]);
			if (!IS_ENABLED(CFG_INSECURE))
				panic();
		}
		break;
	case ETZPC_DECPROT_MCU_ISOLATION:
		if (!stm32_rcc_is_secure() || !stm32_rcc_is_mckprot()) {
			IMSG("WARNING: RCC tzen:%u mckprot:%u, insecure ETZPC hardening %"PRIu32":%s",
			     stm32_rcc_is_secure(), stm32_rcc_is_mckprot(),
			     decprot_id, etzpc_decprot_strings[attr]);
			if (!IS_ENABLED(CFG_INSECURE))
				panic();
		}
		break;
	case ETZPC_DECPROT_NS_RW:
		break;
	default:
		assert(0);
		break;
	}
#endif
}

static void etzpc_do_configure_decprot(struct etzpc_device *etzpc_dev,
				       uint32_t decprot_id,
				       enum etzpc_decprot_attributes attr)
{
	size_t offset = U(4) * (decprot_id / IDS_PER_DECPROT_REGS);
	uint32_t shift = (decprot_id % IDS_PER_DECPROT_REGS) << DECPROT_SHIFT;
	uint32_t masked_decprot = (uint32_t)attr & ETZPC_DECPROT0_MASK;
	vaddr_t base = etzpc_dev->pdata.base.va;
	unsigned int exceptions = 0;

	assert(valid_decprot_id(etzpc_dev, decprot_id));

	FMSG("ID : %"PRIu32", CONF %s", decprot_id,
	     etzpc_decprot_strings[attr]);

	sanitize_decprot_config(decprot_id, attr);

	exceptions = etzpc_lock(etzpc_dev);

	io_clrsetbits32(base + ETZPC_DECPROT0 + offset,
			ETZPC_DECPROT0_MASK << shift,
			masked_decprot << shift);

	etzpc_unlock(etzpc_dev, exceptions);
}

static enum etzpc_decprot_attributes
etzpc_do_get_decprot(struct etzpc_device *etzpc_dev, uint32_t decprot_id)
{
	size_t offset = U(4) * (decprot_id / IDS_PER_DECPROT_REGS);
	uint32_t shift = (decprot_id % IDS_PER_DECPROT_REGS) << DECPROT_SHIFT;
	vaddr_t base = etzpc_dev->pdata.base.va;
	uint32_t value = 0;

	assert(valid_decprot_id(etzpc_dev, decprot_id));

	value = (io_read32(base + ETZPC_DECPROT0 + offset) >> shift) &
		ETZPC_DECPROT0_MASK;

	return (enum etzpc_decprot_attributes)value;
}

static void etzpc_do_lock_decprot(struct etzpc_device *etzpc_dev,
				  uint32_t decprot_id)
{
	size_t offset = U(4) * (decprot_id / IDS_PER_DECPROT_LOCK_REGS);
	uint32_t mask = BIT(decprot_id % IDS_PER_DECPROT_LOCK_REGS);
	vaddr_t base = etzpc_dev->pdata.base.va;
	uint32_t exceptions = 0;

	assert(valid_decprot_id(etzpc_dev, decprot_id));

	exceptions = etzpc_lock(etzpc_dev);

	io_write32(base + offset + ETZPC_DECPROT_LOCK0, mask);

	etzpc_unlock(etzpc_dev, exceptions);
}

static bool decprot_is_locked(struct etzpc_device *etzpc_dev,
			      uint32_t decprot_id)
{
	size_t offset = U(4) * (decprot_id / IDS_PER_DECPROT_LOCK_REGS);
	uint32_t mask = BIT(decprot_id % IDS_PER_DECPROT_LOCK_REGS);
	vaddr_t base = etzpc_dev->pdata.base.va;

	assert(valid_decprot_id(etzpc_dev, decprot_id));

	return io_read32(base + offset + ETZPC_DECPROT_LOCK0) & mask;
}

static void etzpc_do_configure_tzma(struct etzpc_device *etzpc_dev,
				    uint32_t tzma_id, uint16_t tzma_value)
{
	size_t offset = sizeof(uint32_t) * tzma_id;
	vaddr_t base = etzpc_dev->pdata.base.va;
	uint32_t exceptions = 0;

	assert(valid_tzma_id(etzpc_dev, tzma_id));

	exceptions = etzpc_lock(etzpc_dev);

	io_write32(base + ETZPC_TZMA0_SIZE + offset, tzma_value);

	etzpc_unlock(etzpc_dev, exceptions);
}

static uint16_t etzpc_do_get_tzma(struct etzpc_device *etzpc_dev,
				  uint32_t tzma_id)
{
	size_t offset = sizeof(uint32_t) * tzma_id;
	vaddr_t base = etzpc_dev->pdata.base.va;

	assert(valid_tzma_id(etzpc_dev, tzma_id));

	return io_read32(base + ETZPC_TZMA0_SIZE + offset);
}

static void etzpc_do_lock_tzma(struct etzpc_device *etzpc_dev, uint32_t tzma_id)
{
	size_t offset = sizeof(uint32_t) * tzma_id;
	vaddr_t base = etzpc_dev->pdata.base.va;
	uint32_t exceptions = 0;

	assert(valid_tzma_id(etzpc_dev, tzma_id));

	exceptions = etzpc_lock(etzpc_dev);

	io_setbits32(base + ETZPC_TZMA0_SIZE + offset, ETZPC_TZMA0_SIZE_LOCK);

	etzpc_unlock(etzpc_dev, exceptions);
}

static bool tzma_is_locked(struct etzpc_device *etzpc_dev, uint32_t tzma_id)
{
	size_t offset = sizeof(uint32_t) * tzma_id;
	vaddr_t base = etzpc_dev->pdata.base.va;

	assert(valid_tzma_id(etzpc_dev, tzma_id));

	return io_read32(base + ETZPC_TZMA0_SIZE + offset) &
	       ETZPC_TZMA0_SIZE_LOCK;
}

static TEE_Result etzpc_pm(enum pm_op op, unsigned int pm_hint __unused,
			   const struct pm_callback_handle *pm_handle)
{
	struct etzpc_device *etzpc_dev = pm_handle->handle;
	struct stm32_etzpc_driver_data *ddata = etzpc_dev->ddata;
	struct stm32_etzpc_platdata *pdata = &etzpc_dev->pdata;
	unsigned int n = 0;

	if (op == PM_OP_SUSPEND) {
		for (n = 0; n < ddata->num_per_sec; n++) {
			pdata->periph_cfg[n] =
				(uint8_t)etzpc_do_get_decprot(etzpc_dev, n);
			if (decprot_is_locked(etzpc_dev, n))
				pdata->periph_cfg[n] |= PERIPH_PM_LOCK_BIT;
		}

		for (n = 0; n < ddata->num_tzma; n++) {
			pdata->tzma_cfg[n] =
				(uint8_t)etzpc_do_get_tzma(etzpc_dev, n);
			if (tzma_is_locked(etzpc_dev, n))
				pdata->tzma_cfg[n] |= TZMA_PM_LOCK_BIT;
		}

		return TEE_SUCCESS;
	}

	/* PM_OP_RESUME */
	for (n = 0; n < ddata->num_per_sec; n++) {
		unsigned int attr = pdata->periph_cfg[n] & PERIPH_PM_ATTR_MASK;

		etzpc_do_configure_decprot(etzpc_dev, n,
					   (enum etzpc_decprot_attributes)attr);

		if (pdata->periph_cfg[n] & PERIPH_PM_LOCK_BIT)
			etzpc_do_lock_decprot(etzpc_dev, n);
	}

	for (n = 0; n < ddata->num_tzma; n++) {
		uint16_t value = pdata->tzma_cfg[n] & TZMA_PM_VALUE_MASK;

		etzpc_do_configure_tzma(etzpc_dev, n, value);

		if (pdata->tzma_cfg[n] & TZMA_PM_LOCK_BIT)
			etzpc_do_lock_tzma(etzpc_dev, n);
	}

	return TEE_SUCCESS;
}
DECLARE_KEEP_PAGER_PM(etzpc_pm);

static TEE_Result stm32_etzpc_check_access(struct firewall_query *firewall)
{
	struct etzpc_device *etzpc_dev = firewall->ctrl->priv;
	enum etzpc_decprot_attributes attr_req = ETZPC_DECPROT_MAX;
	enum etzpc_decprot_attributes attr = ETZPC_DECPROT_MAX;
	uint32_t id = 0;

	if (!firewall || firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	/*
	 * Peripheral configuration, we assume the configuration is as
	 * follows:
	 * firewall->args[0]: Firewall configuration to check using DECPROT
	 * macro
	 */
	id = firewall->args[0] & ETZPC_ID_MASK;
	attr_req = etzpc_binding2decprot((firewall->args[0] &
					  ETZPC_MODE_MASK) >> ETZPC_MODE_SHIFT);

	if (id < etzpc_dev->ddata->num_per_sec) {
		attr = etzpc_do_get_decprot(etzpc_dev, id);

		if (attr == attr_req)
			return TEE_SUCCESS;
		else
			return TEE_ERROR_ACCESS_DENIED;
	} else {
		return TEE_ERROR_BAD_PARAMETERS;
	}
}

static TEE_Result stm32_etzpc_acquire_access(struct firewall_query *firewall)
{
	struct etzpc_device *etzpc_dev = firewall->ctrl->priv;
	enum etzpc_decprot_attributes attr = ETZPC_DECPROT_MCU_ISOLATION;
	uint32_t id = 0;

	if (!firewall || firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	id = firewall->args[0] & ETZPC_ID_MASK;
	if (id < etzpc_dev->ddata->num_per_sec) {
		attr = etzpc_do_get_decprot(etzpc_dev, id);
		if (attr != ETZPC_DECPROT_S_RW &&
		    attr != ETZPC_DECPROT_NS_R_S_W)
			return TEE_ERROR_ACCESS_DENIED;
	} else {
		return TEE_ERROR_BAD_PARAMETERS;
	}

	return TEE_SUCCESS;
}

static TEE_Result
stm32_etzpc_acquire_memory_access(struct firewall_query *firewall,
				  paddr_t paddr, size_t size,
				  bool read __unused, bool write __unused)
{
	struct etzpc_device *etzpc_dev = firewall->ctrl->priv;
	paddr_t tzma_base = 0;
	size_t prot_size = 0;
	uint32_t id = 0;

	if (!firewall || firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	id = firewall->args[0] & ETZPC_ID_MASK;
	switch (id) {
	case ETZPC_TZMA0_ID:
		tzma_base = ROM_BASE;
		prot_size = etzpc_do_get_tzma(etzpc_dev, 0) * SMALL_PAGE_SIZE;
		break;
	case ETZPC_TZMA1_ID:
		tzma_base = SYSRAM_BASE;
		prot_size = etzpc_do_get_tzma(etzpc_dev, 1) * SMALL_PAGE_SIZE;
		break;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}

	DMSG("Acquiring access for TZMA%u, secured from %#"PRIxPA" to %#"PRIxPA,
	     id == ETZPC_TZMA0_ID ? 0 : 1, tzma_base, tzma_base + prot_size);

	if (core_is_buffer_inside(paddr, size, tzma_base, prot_size))
		return TEE_SUCCESS;

	return TEE_ERROR_ACCESS_DENIED;
}

#ifdef CFG_STM32MP15
static bool pager_permits_decprot_config(uint32_t decprot_id,
					 enum etzpc_decprot_attributes attr)
{
	paddr_t ram_base = 0;
	size_t ram_size = 0;

	if (!IS_ENABLED(CFG_WITH_PAGER))
		return true;

	switch (decprot_id) {
	case ETZPC_TZMA1_ID:
		ram_base = SYSRAM_BASE;
		ram_size = SYSRAM_SEC_SIZE;
		break;
	case STM32MP1_ETZPC_SRAM1_ID:
		ram_base = SRAM1_BASE;
		ram_size = SRAM1_SIZE;
		break;
	case STM32MP1_ETZPC_SRAM2_ID:
		ram_base = SRAM2_BASE;
		ram_size = SRAM2_SIZE;
		break;
	case STM32MP1_ETZPC_SRAM3_ID:
		ram_base = SRAM3_BASE;
		ram_size = SRAM3_SIZE;
		break;
	case STM32MP1_ETZPC_SRAM4_ID:
		ram_base = SRAM4_BASE;
		ram_size = SRAM4_SIZE;
		break;
	default:
		return true;
	}

	if (stm32mp1_ram_intersect_pager_ram(ram_base, ram_size) &&
	    attr != ETZPC_DECPROT_S_RW) {
		EMSG("Internal RAM %#"PRIxPA"..%#"PRIxPA" is used by pager, must be secure",
		     ram_base, ram_base + ram_size);
		return false;
	}

	return true;
}
#endif

static TEE_Result stm32_etzpc_configure_memory(struct firewall_query *firewall,
					       paddr_t paddr, size_t size)
{
	struct etzpc_device *etzpc_dev = firewall->ctrl->priv;
	enum etzpc_decprot_attributes attr = ETZPC_DECPROT_MAX;
	unsigned int total_sz = 0;
	bool lock = false;
	uint32_t mode = 0;
	uint32_t id = 0;

	if (firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	id = firewall->args[0] & ETZPC_ID_MASK;
	mode = (firewall->args[0] & ETZPC_MODE_MASK) >> ETZPC_MODE_SHIFT;
	attr = etzpc_binding2decprot(mode);
	lock = firewall->args[0] & ETZPC_LOCK_MASK;

	if (id < etzpc_dev->ddata->num_per_sec &&
	    (id == STM32MP1_ETZPC_SRAM1_ID || id == STM32MP1_ETZPC_SRAM2_ID ||
#if defined(CFG_STM32MP15)
	     id == STM32MP1_ETZPC_SRAM4_ID || id == STM32MP1_ETZPC_RETRAM_ID ||
#endif /* defined(CFG_STM32MP15) */
	     id == STM32MP1_ETZPC_SRAM3_ID)) {
		paddr = stm32mp1_pa_or_sram_alias_pa(paddr);

		switch (id) {
		case (STM32MP1_ETZPC_SRAM1_ID):
			if (paddr != SRAM1_BASE || size != SRAM1_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;
			break;
		case (STM32MP1_ETZPC_SRAM2_ID):
			if (paddr != SRAM2_BASE || size != SRAM2_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;
			break;
		case (STM32MP1_ETZPC_SRAM3_ID):
			if (paddr != SRAM3_BASE || size != SRAM3_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;
			break;
#if defined(CFG_STM32MP15)
		case (STM32MP1_ETZPC_SRAM4_ID):
			if (paddr != SRAM4_BASE || size != SRAM4_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;
			break;
		case (STM32MP1_ETZPC_RETRAM_ID):
			if (paddr != RETRAM_BASE || size != RETRAM_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;
			break;
#endif /* defined(CFG_STM32MP15) */
		default:
			panic();
		}

		if (decprot_is_locked(etzpc_dev, id)) {
			if (etzpc_do_get_decprot(etzpc_dev, id) != attr) {
				EMSG("Internal RAM configuration locked");
				return TEE_ERROR_ACCESS_DENIED;
			}

			return TEE_SUCCESS;
		}

#ifdef CFG_STM32MP15
		if (!pager_permits_decprot_config(id, attr))
			return TEE_ERROR_ACCESS_DENIED;
#endif

		etzpc_do_configure_decprot(etzpc_dev, id, attr);
		if (lock)
			etzpc_do_lock_decprot(etzpc_dev, id);

		return TEE_SUCCESS;
	} else if (id == ETZPC_TZMA0_ID || id == ETZPC_TZMA1_ID) {
		unsigned int tzma_id = 0;

		/*
		* TZMA configuration, we assume the configuration is as
		* follows:
		* firewall->args[0]: Firewall configuration to apply
		*/
		switch (id) {
		case ETZPC_TZMA0_ID:
			if (paddr != ROM_BASE || size > ROM_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;

			tzma_id = 0;
			break;
		case ETZPC_TZMA1_ID:
			if (paddr != SYSRAM_BASE || size > SYSRAM_SIZE)
				return TEE_ERROR_BAD_PARAMETERS;

			tzma_id = 1;
			break;
		default:
			return TEE_ERROR_BAD_PARAMETERS;
		}

		if (!IS_ALIGNED(size, SMALL_PAGE_SIZE))
			return TEE_ERROR_BAD_PARAMETERS;

		total_sz = ROUNDUP_DIV(size, SMALL_PAGE_SIZE);

#ifdef CFG_STM32MP15
		if (!pager_permits_decprot_config(id, attr))
			return TEE_ERROR_ACCESS_DENIED;
#endif

		if (tzma_is_locked(etzpc_dev, tzma_id)) {
			if (etzpc_do_get_tzma(etzpc_dev, tzma_id) != total_sz) {
				EMSG("TZMA configuration locked");
				return TEE_ERROR_ACCESS_DENIED;
			}

			return TEE_SUCCESS;
		}

		etzpc_do_configure_tzma(etzpc_dev, tzma_id, total_sz);
		if (lock)
			etzpc_do_lock_tzma(etzpc_dev, tzma_id);
	} else {
		EMSG("Unknown firewall ID: %"PRIu32, id);

		return TEE_ERROR_BAD_PARAMETERS;
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_etzpc_configure(struct firewall_query *firewall)
{
	struct etzpc_device *etzpc_dev = firewall->ctrl->priv;
	enum etzpc_decprot_attributes attr = ETZPC_DECPROT_MAX;
	uint32_t id = 0;

	if (firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	id = firewall->args[0] & ETZPC_ID_MASK;

	FMSG("Setting firewall configuration for peripheral ID: %"PRIu32, id);

	if (id < etzpc_dev->ddata->num_per_sec) {
		uint32_t mode = 0;

		/*
		 * Peripheral configuration, we assume the configuration is as
		 * follows:
		 * firewall->args[0]: Firewall configuration to apply
		 */

		mode = (firewall->args[0] & ETZPC_MODE_MASK) >>
		       ETZPC_MODE_SHIFT;
		attr = etzpc_binding2decprot(mode);

		if (decprot_is_locked(etzpc_dev, id)) {
			if (etzpc_do_get_decprot(etzpc_dev, id) != attr) {
				EMSG("Peripheral configuration locked");
				return TEE_ERROR_ACCESS_DENIED;
			}

			DMSG("Valid access for periph %"PRIu32" - attr %s",
			     id, etzpc_decprot_strings[attr]);

			return TEE_SUCCESS;
		}

		DMSG("Setting access config for periph %"PRIu32" - attr %s", id,
		     etzpc_decprot_strings[attr]);

#ifdef CFG_STM32MP15
		if (!pager_permits_decprot_config(id, attr))
			return TEE_ERROR_ACCESS_DENIED;
#endif

		etzpc_do_configure_decprot(etzpc_dev, id, attr);
		if (firewall->args[0] & ETZPC_LOCK_MASK)
			etzpc_do_lock_decprot(etzpc_dev, id);
	} else {
		EMSG("Unknown firewall ID: %"PRIu32, id);

		return TEE_ERROR_BAD_PARAMETERS;
	}

	return TEE_SUCCESS;
}

static struct etzpc_device *stm32_etzpc_alloc(void)
{
	struct etzpc_device *etzpc_dev = calloc(1, sizeof(*etzpc_dev));
	struct stm32_etzpc_driver_data *ddata = calloc(1, sizeof(*ddata));

	if (etzpc_dev && ddata) {
		etzpc_dev->ddata = ddata;
		return etzpc_dev;
	}

	free(ddata);
	free(etzpc_dev);

	return NULL;
}

/* Informative unused function */
static void stm32_etzpc_free(struct etzpc_device *etzpc_dev)
{
	if (etzpc_dev) {
		free(etzpc_dev->ddata);
		free(etzpc_dev);
	}
}

static void stm32_etzpc_set_driverdata(struct etzpc_device *dev)
{
	struct stm32_etzpc_driver_data *ddata = dev->ddata;
	vaddr_t base = dev->pdata.base.va;
	uint32_t reg = io_read32(base + ETZPC_HWCFGR);

	ddata->num_tzma = (reg & ETZPC_HWCFGR_NUM_TZMA_MASK) >>
			  ETZPC_HWCFGR_NUM_TZMA_SHIFT;
	ddata->num_per_sec = (reg & ETZPC_HWCFGR_NUM_PER_SEC_MASK) >>
			     ETZPC_HWCFGR_NUM_PER_SEC_SHIFT;
	ddata->num_ahb_sec = (reg & ETZPC_HWCFGR_NUM_AHB_SEC_MASK) >>
			     ETZPC_HWCFGR_NUM_AHB_SEC_SHIFT;

	DMSG("ETZPC revision 0x%02"PRIx8", per_sec %u, ahb_sec %u, tzma %u",
	     io_read8(base + ETZPC_VERR),
	     ddata->num_per_sec, ddata->num_ahb_sec, ddata->num_tzma);
}

static void fdt_etzpc_conf_decprot(struct etzpc_device *dev,
				   const void *fdt, int node)
{
	const fdt32_t *cuint = NULL;
	size_t i = 0;
	int len = 0;

	cuint = fdt_getprop(fdt, node, "st,decprot", &len);
	if (!cuint) {
		DMSG("No ETZPC DECPROT configuration in DT");
		return;
	}

	clk_enable(dev->pdata.clk);

	for (i = 0; i < len / sizeof(uint32_t); i++) {
		uint32_t value = fdt32_to_cpu(cuint[i]);
		uint32_t id = value & ETZPC_ID_MASK;
		uint32_t mode = (value & ETZPC_MODE_MASK) >> ETZPC_MODE_SHIFT;
		bool lock = value & ETZPC_LOCK_MASK;
		enum etzpc_decprot_attributes attr = ETZPC_DECPROT_MAX;

		if (!valid_decprot_id(dev, id)) {
			DMSG("Invalid DECPROT %"PRIu32, id);
			panic();
		}

		attr = etzpc_binding2decprot(mode);

#ifdef CFG_STM32MP15
		if (!pager_permits_decprot_config(id, attr))
			panic();
#endif

		etzpc_do_configure_decprot(dev, id, attr);

		if (lock)
			etzpc_do_lock_decprot(dev, id);
	}

	clk_disable(dev->pdata.clk);
}

static TEE_Result
stm32_etzpc_dt_probe_bus(const void *fdt, int node,
			 struct firewall_controller *ctrl __maybe_unused)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct firewall_query *fw = NULL;
	int subnode = 0;

	DMSG("Populating %s firewall bus", ctrl->name);

	fdt_for_each_subnode(subnode, fdt, node) {
		unsigned int i = 0;

		if (fdt_get_status(fdt, subnode) == DT_STATUS_DISABLED)
			continue;

		if (IS_ENABLED(CFG_INSECURE) &&
		    stm32mp_allow_probe_shared_device(fdt, subnode)) {
			DMSG("Skipping firewall attributes check for %s",
			     fdt_get_name(fdt, subnode, NULL));
			goto skip_check;
		}

		DMSG("Acquiring firewall access for %s when probing bus",
		     fdt_get_name(fdt, subnode, NULL));

		do {
			/*
			 * The access-controllers property is mandatory for
			 * firewall bus devices
			 */
			res = firewall_dt_get_by_index(fdt, subnode, i, &fw);
			if (res == TEE_ERROR_ITEM_NOT_FOUND) {
				/* Stop when nothing more to parse */
				break;
			} else if (res) {
				EMSG("%s: Error on node %s: %#"PRIx32,
				     ctrl->name,
				     fdt_get_name(fdt, subnode, NULL), res);
				panic();
			}

			res = firewall_acquire_access(fw);
			if (res) {
				EMSG("%s: %s not accessible: %#"PRIx32,
				     ctrl->name,
				     fdt_get_name(fdt, subnode, NULL), res);
				panic();
			}

			firewall_put(fw);
			i++;
		} while (true);

skip_check:
		res = dt_driver_maybe_add_probe_node(fdt, subnode);
		if (res) {
			EMSG("Failed on node %s with %#"PRIx32,
			     fdt_get_name(fdt, subnode, NULL), res);
			panic();
		}
	}

	return TEE_SUCCESS;
}

static TEE_Result init_etzpc_from_dt(struct etzpc_device *etzpc_dev,
				     const void *fdt, int node)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct dt_node_info etzpc_info = { };
	struct io_pa_va base = { };
	int len = 0;

	fdt_fill_device_info(fdt, &etzpc_info, node);
	if (etzpc_info.reg == DT_INFO_INVALID_REG ||
	    etzpc_info.reg_size == DT_INFO_INVALID_REG_SIZE)
		return TEE_ERROR_ITEM_NOT_FOUND;

	base.pa = etzpc_info.reg;
	etzpc_dev->pdata.name = fdt_get_name(fdt, node, &len);
	etzpc_dev->pdata.base.va = io_pa_or_va_secure(&base,
						      etzpc_info.reg_size);
	etzpc_dev->pdata.base = base;
	res = clk_dt_get_by_index(fdt, node, 0, &etzpc_dev->pdata.clk);
	if (res)
		return res;

	stm32_etzpc_set_driverdata(etzpc_dev);

	etzpc_dev->pdata.periph_cfg =
		calloc(etzpc_dev->ddata->num_per_sec,
		       sizeof(*etzpc_dev->pdata.periph_cfg));
	if (!etzpc_dev->pdata.periph_cfg)
		return TEE_ERROR_OUT_OF_MEMORY;

	etzpc_dev->pdata.tzma_cfg =
		calloc(etzpc_dev->ddata->num_tzma,
		       sizeof(*etzpc_dev->pdata.tzma_cfg));
	if (!etzpc_dev->pdata.tzma_cfg) {
		free(etzpc_dev->pdata.periph_cfg);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	fdt_etzpc_conf_decprot(etzpc_dev, fdt, node);

	return TEE_SUCCESS;
}

static const struct firewall_controller_ops firewall_ops = {
	.set_conf = stm32_etzpc_configure,
	.set_memory_conf = stm32_etzpc_configure_memory,
	.check_access = stm32_etzpc_check_access,
	.acquire_access = stm32_etzpc_acquire_access,
	.acquire_memory_access = stm32_etzpc_acquire_memory_access,
};

static TEE_Result stm32_etzpc_probe(const void *fdt, int node,
				    const void *compat_data __unused)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct etzpc_device *etzpc_dev = stm32_etzpc_alloc();
	struct firewall_controller *controller = NULL;

	if (!etzpc_dev) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto err;
	}

	res = init_etzpc_from_dt(etzpc_dev, fdt, node);
	if (res) {
		stm32_etzpc_free(etzpc_dev);
		return res;
	}

	controller = calloc(1, sizeof(*controller));
	if (!controller) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto err;
	}

	controller->base = &etzpc_dev->pdata.base;
	controller->name = etzpc_dev->pdata.name;
	controller->priv = etzpc_dev;
	controller->ops = &firewall_ops;

	res = firewall_dt_controller_register(fdt, node, controller);
	if (res)
		goto err;

	res = stm32_etzpc_dt_probe_bus(fdt, node, controller);
	if (res)
		goto err;

	register_pm_core_service_cb(etzpc_pm, etzpc_dev, "stm32-etzpc");

	return TEE_SUCCESS;

err:
	EMSG("ETZPC probe failed: %#"PRIx32, res);
	panic();
}

static const struct dt_device_match etzpc_match_table[] = {
	{ .compatible = "st,stm32-etzpc", },
	{ }
};

DEFINE_DT_DRIVER(etzpc_dt_driver) = {
	.name = "stm32-etzpc",
	.match_table = etzpc_match_table,
	.probe = stm32_etzpc_probe,
};
