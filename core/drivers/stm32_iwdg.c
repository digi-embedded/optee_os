// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2017-2021, STMicroelectronics - All Rights Reserved
 */

#include <assert.h>
#include <drivers/clk_dt.h>
#include <drivers/stm32_firewall.h>
#include <drivers/stm32_iwdg.h>
#include <keep.h>
#include <kernel/boot.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <kernel/interrupt.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <kernel/spinlock.h>
#include <libfdt.h>
#include <mm/core_memprot.h>
#include <sm/sm.h>
#include <stm32_util.h>
#include <stm32mp_pm.h>
#include <string.h>
#include <trace.h>

/* IWDG Compatibility */
#define IWDG_TIMEOUT_US		U(100000)

/* IWDG registers offsets */
#define IWDG_KR_OFFSET		U(0x00)
#define IWDG_PR_OFFSET		U(0x04)
#define IWDG_RLR_OFFSET		U(0x08)
#define IWDG_SR_OFFSET		U(0x0C)
#define IWDG_EWCR_OFFSET	U(0x14)

/* Registers values */
#define IWDG_KR_WPROT_KEY	U(0x0000)
#define IWDG_KR_ACCESS_KEY	U(0x5555)
#define IWDG_KR_RELOAD_KEY	U(0xAAAA)
#define IWDG_KR_START_KEY	U(0xCCCC)

#define IWDG_PR_DIV_4		U(0x00)
#define IWDG_PR_DIV_256		U(0x06)

#define IWDG_RLR_RL_MASK	GENMASK_32(11, 0)

#define IWDG_SR_EWU		BIT(3)
#define IWDG_SR_EWIF		BIT(14)

#define IWDG_EWCR_EWIE		BIT(15)
#define IWDG_EWCR_EWIC		BIT(14)
#define IWDG_EWCR_EWIT_MASK	GENMASK_32(11, 0)

struct stm32_iwdg_device {
	struct stm32_iwdg_platdata pdata;
	struct itr_handler *itr;
	SLIST_ENTRY(stm32_iwdg_device) link;
};

static unsigned int list_lock = SPINLOCK_UNLOCK;

static SLIST_HEAD(iwdg_dev_list_head, stm32_iwdg_device) iwdg_dev_list =
	SLIST_HEAD_INITIALIZER(iwdg_dev_list_head);

static vaddr_t get_base(struct stm32_iwdg_device *iwdg)
{
	return io_pa_or_va(&iwdg->pdata.base, 1);
}

static void iwdg_refresh(struct stm32_iwdg_device *iwdg)
{
	assert(iwdg);

	clk_enable(iwdg->pdata.clock);

	io_write32(get_base(iwdg) + IWDG_KR_OFFSET, IWDG_KR_RELOAD_KEY);

	clk_disable(iwdg->pdata.clock);
}

static enum itr_return stm32_iwdg_it_handler(struct itr_handler *handler)
{
	unsigned int __maybe_unused cpu = get_core_pos();
	struct stm32_iwdg_device *iwdg = handler->data;
	vaddr_t iwdg_base = get_base(iwdg);

	DMSG("CPU %u IT Watchdog %#"PRIxPA, cpu,
	     virt_to_phys((void *)iwdg->pdata.base.pa));

	stm32mp_dump_core_registers(true);

	clk_enable(iwdg->pdata.clock);

	/* Check for spurious interrupt */
	if (!(io_read32(iwdg_base + IWDG_SR_OFFSET) & IWDG_SR_EWIF)) {
		clk_disable(iwdg->pdata.clock);
		return ITRR_NONE;
	}

	/*
	 * Writing IWDG_EWCR_EWIT triggers a watchdog refresh.
	 * To prevent the watchdog refresh, write-protect all the registers;
	 * this makes read-only all IWDG_EWCR fields except IWDG_EWCR_EWIC.
	 */
	io_write32(iwdg_base + IWDG_KR_OFFSET, IWDG_KR_WPROT_KEY);

	/* Disable early interrupt */
	io_setbits32(iwdg_base + IWDG_EWCR_OFFSET, IWDG_EWCR_EWIC);

	clk_disable(iwdg->pdata.clock);

	/*
	 * Ack interrupt as we do not return from next call
	 * and interrupt is no more considered as pending here.
	 */
	stm32mp_gic_set_end_of_interrupt(handler->it);

	panic("Watchdog");

	return ITRR_HANDLED;
}
DECLARE_KEEP_PAGER(stm32_iwdg_it_handler);

void stm32_iwdg_refresh(void)
{
	struct stm32_iwdg_device *iwdg = NULL;
	uint32_t exceptions = cpu_spin_lock_xsave(&list_lock);

	SLIST_FOREACH(iwdg, &iwdg_dev_list, link)
		iwdg_refresh(iwdg);

	cpu_spin_unlock_xrestore(&list_lock, exceptions);
}

static TEE_Result stm32_iwdg_conf_etimeout(struct stm32_iwdg_device *iwdg)
{
	uint32_t reload = 0;
	uint32_t status = 0;
	uint64_t timeout_ref = 0;
	unsigned long long reload_ll = 0;
	vaddr_t iwdg_base = get_base(iwdg);

	/* Prescaler fix to 256 */
	reload_ll = (unsigned long long)iwdg->pdata.sec_timeout *
		    clk_get_rate(iwdg->pdata.clk_lsi);

	reload = ((uint32_t)(reload_ll >> 8) - 1) & IWDG_EWCR_EWIT_MASK;

	clk_enable(iwdg->pdata.clock);
	clk_enable(iwdg->pdata.clk_lsi);

	io_write32(iwdg_base + IWDG_KR_OFFSET, IWDG_KR_START_KEY);
	io_write32(iwdg_base + IWDG_KR_OFFSET, IWDG_KR_ACCESS_KEY);
	io_write32(iwdg_base + IWDG_PR_OFFSET, IWDG_PR_DIV_256);
	io_write32(iwdg_base + IWDG_EWCR_OFFSET, reload | IWDG_EWCR_EWIE);

	timeout_ref = timeout_init_us(IWDG_TIMEOUT_US);
	while (!timeout_elapsed(timeout_ref))
		if (!(io_read32(iwdg_base + IWDG_SR_OFFSET) & IWDG_SR_EWU))
			break;

	status = io_read32(iwdg_base + IWDG_SR_OFFSET) & IWDG_SR_EWU;
	clk_disable(iwdg->pdata.clock);
	if (status)
		return TEE_ERROR_GENERIC;

	iwdg->itr = itr_alloc_add(iwdg->pdata.irq, stm32_iwdg_it_handler,
				  ITRF_TRIGGER_LEVEL, iwdg);

	if (!iwdg->itr)
		return TEE_ERROR_GENERIC;

	itr_enable(iwdg->itr->it);

	return TEE_SUCCESS;
}

static TEE_Result stm32_iwdg_parse_fdt(struct stm32_iwdg_device *iwdg_dev,
				       const void *fdt, int node)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct dt_node_info dt_info = { };
	const fdt32_t *cuint = NULL;
	struct clk *clk = NULL;
	struct io_pa_va *base = &iwdg_dev->pdata.base;
	const struct stm32_firewall_cfg sec_cfg[] = {
		{ FWLL_SEC_RW | FWLL_MASTER(0) },
		{ }, /* Null terminated */
	};

	_fdt_fill_device_info(fdt, &dt_info, node);
	if (dt_info.reg == DT_INFO_INVALID_REG ||
	    dt_info.reg_size == DT_INFO_INVALID_REG_SIZE ||
	    dt_info.clock == DT_INFO_INVALID_CLOCK ||
	    dt_info.interrupt == DT_INFO_INVALID_INTERRUPT)
		return TEE_ERROR_ITEM_NOT_FOUND;

	base->pa = dt_info.reg;

	res = stm32_firewall_check_access(base->pa, dt_info.reg_size, sec_cfg);
	if (res == TEE_SUCCESS)
		io_pa_or_va_secure(base, dt_info.reg_size);
	else
		io_pa_or_va_nsec(base, dt_info.reg_size);

	assert(base->va);

	iwdg_dev->pdata.irq = dt_info.interrupt;

	res = clk_dt_get_by_index(fdt, node, 0, &clk);
	if (res)
		return res;

	iwdg_dev->pdata.clock = clk;

	res = clk_dt_get_by_index(fdt, node, 1, &clk);
	if (!clk)
		return res;

	iwdg_dev->pdata.clk_lsi = clk;

	cuint = fdt_getprop(fdt, node, "timeout-sec", NULL);
	if (!cuint)
		return TEE_ERROR_BAD_PARAMETERS;

	iwdg_dev->pdata.timeout = (int)fdt32_to_cpu(*cuint);

	cuint = fdt_getprop(fdt, node, "secure-timeout-sec", NULL);
	if (!cuint)
		iwdg_dev->pdata.sec_timeout = -1;
	else
		iwdg_dev->pdata.sec_timeout = (int)fdt32_to_cpu(*cuint);

	/* DT can specify low power cases */
	if (!fdt_getprop(fdt, node, "stm32,enable-on-stop", NULL))
		iwdg_dev->pdata.flags |= IWDG_DISABLE_ON_STOP;

	if (!fdt_getprop(fdt, node, "stm32,enable-on-standby", NULL))
		iwdg_dev->pdata.flags |= IWDG_DISABLE_ON_STANDBY;

	return TEE_SUCCESS;
}

static TEE_Result stm32_iwdg_setup(struct stm32_iwdg_device *iwdg_dev,
				   const void *fdt, int node)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	uint32_t hw_init = 0;

	assert(iwdg_dev);

	res = stm32_iwdg_parse_fdt(iwdg_dev, fdt, node);
	if (res)
		return res;

	hw_init = stm32_get_iwdg_otp_config(iwdg_dev->pdata.base.pa);
	if (hw_init & IWDG_HW_ENABLED)
		iwdg_dev->pdata.flags |= IWDG_HW_ENABLED;

	if (hw_init & IWDG_DISABLE_ON_STOP)
		iwdg_dev->pdata.flags |= IWDG_DISABLE_ON_STOP;

	if (hw_init & IWDG_DISABLE_ON_STANDBY)
		iwdg_dev->pdata.flags |= IWDG_DISABLE_ON_STANDBY;

	if (iwdg_dev->pdata.sec_timeout > 0) {
		res = stm32_iwdg_conf_etimeout(iwdg_dev);
		if (res) {
			EMSG("IWDG early timeout config failed: %#"PRIx32, res);
			panic();
		}
	}

	return res;
}

static TEE_Result stm32_iwdg_probe(const void *fdt, int node,
				   const void *compat_data __unused)
{
	struct stm32_iwdg_device *iwdg_dev = NULL;
	TEE_Result res = TEE_SUCCESS;
	uint32_t exceptions = 0;

	iwdg_dev = calloc(1, sizeof(*iwdg_dev));
	if (!iwdg_dev)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = stm32_iwdg_setup(iwdg_dev, fdt, node);
	if (res) {
		free(iwdg_dev);
		return res;
	}

	exceptions = cpu_spin_lock_xsave(&list_lock);
	SLIST_INSERT_HEAD(&iwdg_dev_list, iwdg_dev, link);
	cpu_spin_unlock_xrestore(&list_lock, exceptions);

	return TEE_SUCCESS;
}

static const struct dt_device_match stm32_iwdg_match_table[] = {
	{ .compatible = "st,stm32mp1-iwdg" },
	{ }
};

DEFINE_DT_DRIVER(stm32_iwdg_dt_driver) = {
	.name = "stm32-iwdg",
	.match_table = stm32_iwdg_match_table,
	.probe = stm32_iwdg_probe,
};
