// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2018-2022, STMicroelectronics
 */

#include <assert.h>
#include <crypto/crypto.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <drivers/stm32_rng.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <kernel/boot.h>
#include <kernel/panic.h>
#include <kernel/thread.h>
#include <kernel/pm.h>
#include <libfdt.h>
#include <rng_support.h>
#include <stdbool.h>
#include <stm32_util.h>
#include <string.h>
#include <tee/tee_cryp_utl.h>

#define RNG_CR			U(0x00)
#define RNG_SR			U(0x04)
#define RNG_DR			U(0x08)

#define RNG_CR_RNGEN		BIT(2)
#define RNG_CR_IE		BIT(3)
#define RNG_CR_CED		BIT(5)
#define RNG_CR_CONDRST		BIT(30)
#define RNG_CR_CLKDIV		GENMASK_32(19, 16)
#define RNG_CR_CLKDIV_SHIFT	U(16)

#define RNG_SR_DRDY		BIT(0)
#define RNG_SR_CECS		BIT(1)
#define RNG_SR_SECS		BIT(2)
#define RNG_SR_CEIS		BIT(5)
#define RNG_SR_SEIS		BIT(6)

#define RNG_TIMEOUT_US_10MS	U(10000)
#define RNG_TIMEOUT_US_1MS	U(1000)
#define RNG_FIFO_BYTE_DEPTH	U(16)

#define RNG_NIST_CONFIG_A	U(0x0F00D00)
#define RNG_NIST_CONFIG_B	U(0x1801000)
#define RNG_NIST_CONFIG_MASK	GENMASK_32(25, 8)

#define RNG_MAX_NOISE_CLK_FREQ	U(3000000)

struct stm32_rng_driver_data {
	bool has_cond_reset;
};

struct stm32_rng_device {
	struct stm32_rng_platdata pdata;
	struct stm32_rng_driver_data *ddata;
	unsigned int lock;
	bool error_conceal;
	uint64_t error_to_ref;
	uint32_t pm_cr;
};

/* Expect a single RNG device */
static struct stm32_rng_device stm32_rng;
DECLARE_KEEP_PAGER(stm32_rng);

static vaddr_t get_base(struct stm32_rng_device *dev)
{
	return io_pa_or_va(&dev->pdata.base, 1);
}

/* Seed error management */
static void conceal_seed_error(struct stm32_rng_device *dev)
{
	struct stm32_rng_driver_data *ddata = dev->ddata;
	vaddr_t base = get_base(dev);
	uint32_t sr = 0;

	if (ddata->has_cond_reset) {
		if (!dev->error_conceal) {
			/* Conceal by resetting the subsystem */
			sr = io_read32(base + RNG_SR);
			if (sr & RNG_SR_SECS) {
				io_setbits32(base + RNG_CR, RNG_CR_CONDRST);
				io_clrbits32(base + RNG_CR, RNG_CR_CONDRST);

				dev->error_to_ref =
					timeout_init_us(RNG_TIMEOUT_US_10MS);
				dev->error_conceal = true;

				/* Wait subsystem reset cycle completes */
				return;
			}
		} else {
			/* Measure time before possible reschedule */
			bool timed_out = timeout_elapsed(dev->error_to_ref);

			if ((io_read32(base + RNG_CR) & RNG_CR_CONDRST)) {
				if (timed_out)
					panic();

				/* Wait subsystem reset cycle completes */
				return;
			}

			dev->error_conceal = false;
		}

		io_clrbits32(base + RNG_SR, RNG_SR_SEIS);
	} else {
		size_t i = 0;

		io_mask32(base + RNG_SR, 0, RNG_SR_SEIS);

		for (i = 12; i != 0; i--)
			(void)io_read32(base + RNG_DR);
	}

	if (io_read32(base + RNG_SR) & RNG_SR_SEIS)
		panic("RNG noise");
}

static TEE_Result stm32_rng_read_available(struct stm32_rng_device *dev,
					   uint8_t *out, size_t size)
{
	vaddr_t base = get_base(dev);
	uint8_t *buf = out;

	assert(size <= RNG_FIFO_BYTE_DEPTH);

	if (dev->error_conceal || io_read32(base + RNG_SR) & RNG_SR_SEIS) {
		conceal_seed_error(dev);
		return TEE_ERROR_NO_DATA;
	}

	if (!(io_read32(base + RNG_SR) & RNG_SR_DRDY))
		return TEE_ERROR_NO_DATA;

	/* RNG is ready: read up to 4 32bit words */
	while (size) {
		uint32_t data32 = io_read32(base + RNG_DR);
		size_t sz = MIN(size, sizeof(uint32_t));

		memcpy(buf, &data32, sz);
		buf += sz;
		size -= sz;
	}

	return TEE_SUCCESS;
}

static uint32_t stm32_rng_clock_freq_restrain(struct stm32_rng_device *dev)
{
	unsigned long clock_rate = 0;
	uint32_t clock_div = 0;

	clock_rate = clk_get_rate(dev->pdata.clock);

	/*
	 * Get the exponent to apply on the CLKDIV field in RNG_CR register
	 * No need to handle the case when clock-div > 0xF as it is physically
	 * impossible
	 */
	while ((clock_rate >> clock_div) > RNG_MAX_NOISE_CLK_FREQ)
		clock_div++;

	DMSG("RNG clk rate : %lu", clk_get_rate(dev->pdata.clock) >> clock_div);

	return clock_div;
}

static TEE_Result stm32_rng_init(struct stm32_rng_device *dev)
{
	struct stm32_rng_platdata *pdata = &dev->pdata;
	struct stm32_rng_driver_data *ddata = dev->ddata;
	vaddr_t base = get_base(dev);
	uint64_t timeout_ref = 0;
	uint32_t clock_div = 0;
	uint32_t sr = 0;
	uint32_t cr_ced_mask = 0;

	if (!pdata->clock_error)
		cr_ced_mask = RNG_CR_CED;

	/* Clean error indications */
	io_write32(base + RNG_SR, 0);

	if (ddata->has_cond_reset) {
		clock_div = stm32_rng_clock_freq_restrain(dev);

		/* Update configuration fields */
		io_clrsetbits32(base + RNG_CR, RNG_NIST_CONFIG_MASK,
				RNG_NIST_CONFIG_B | RNG_CR_CONDRST |
				cr_ced_mask);
		io_clrsetbits32(base + RNG_CR, RNG_CR_CLKDIV,
				(clock_div << RNG_CR_CLKDIV_SHIFT));

		/* No need to wait for RNG_CR_CONDRST toggle as we enable clk */
		io_clrsetbits32(base + RNG_CR, RNG_CR_CONDRST, RNG_CR_RNGEN);
	} else {
		io_setbits32(base + RNG_CR, RNG_CR_RNGEN | cr_ced_mask);
	}

	timeout_ref = timeout_init_us(RNG_TIMEOUT_US_10MS);
	while (!(io_read32(base + RNG_SR) & RNG_SR_DRDY))
		if (timeout_elapsed(timeout_ref))
			break;

	if (!(io_read32(base + RNG_SR) & RNG_SR_DRDY))
		return TEE_ERROR_GENERIC;

	sr = io_read32(base + RNG_SR);
	if (sr & (RNG_SR_CEIS | RNG_SR_CECS))
		DMSG("Clock error detected");

	if (sr & (RNG_SR_SECS | RNG_SR_SEIS))
		conceal_seed_error(dev);

	return TEE_SUCCESS;
}

/* Masks interrupts while reading available data from RNG FIFO */
static TEE_Result get_rng_bytes_relaxed(uint8_t *out, size_t len)
{
	uint64_t timeout_ref = timeout_init_us(RNG_TIMEOUT_US_10MS);
	TEE_Result res = TEE_ERROR_GENERIC;
	uint32_t exceptions = 0;
	bool timed_out = false;
	uint8_t *buf = out;

	clk_enable(stm32_rng.pdata.clock);

	while (len) {
		size_t burst_len = MIN(len, RNG_FIFO_BYTE_DEPTH);

		exceptions = may_spin_lock(&stm32_rng.lock);

		res = stm32_rng_read_available(&stm32_rng, buf, burst_len);
		if (res) {
			assert(res == TEE_ERROR_NO_DATA);

			/* Measure timeout before we're possibly rescheduled */
			timed_out = timeout_elapsed(timeout_ref);
		} else {
			buf += burst_len;
			len -= burst_len;
			timeout_ref = timeout_init_us(RNG_TIMEOUT_US_10MS);
		}

		may_spin_unlock(&stm32_rng.lock, exceptions);

		if (len && timed_out)
			break;
	}

	clk_disable(stm32_rng.pdata.clock);

	if (!len)
		return TEE_SUCCESS;

	if (timed_out)
		return TEE_ERROR_NO_DATA;

	return TEE_ERROR_GENERIC;
}

#ifndef CFG_WITH_SOFTWARE_PRNG
void plat_rng_init(void)
{
	if (!stm32_rng.pdata.base.pa)
		panic("RNG not available");
}

TEE_Result crypto_rng_read(void *buf, size_t blen)
#else
TEE_Result stm32_rng_read(void *buf, size_t blen)
#endif
{
	TEE_Result res = TEE_ERROR_GENERIC;
	uint8_t *b = buf;

	if (!b)
		return TEE_ERROR_BAD_PARAMETERS;

	if (!stm32_rng.pdata.base.va) {
		EMSG("No RNG");
		return TEE_ERROR_NOT_SUPPORTED;
	}

	res = get_rng_bytes_relaxed(b, blen);
	if (res)
		memset(b, 0, blen);

	return res;
}

uint8_t hw_get_random_byte(void)
{
	uint8_t data = 0;

	if (get_rng_bytes_relaxed(&data, 1))
		panic();

	return data;
}

static TEE_Result stm32_rng_pm_resume(struct stm32_rng_device *dev)
{
	/* Clean error indications */
	io_write32(get_base(dev) + RNG_SR, 0);

	if (dev->ddata->has_cond_reset) {
		/*
		 * Correct configuration in bits [29:4] must be set in the same
		 * access that set RNG_CR_CONDRST bit. Else config setting is
		 * not taken into account. CONFIGLOCK bit must also be unset but
		 * it is not handled at the moment.
		 */
		io_write32(get_base(dev) + RNG_CR, dev->pm_cr | RNG_CR_CONDRST);

		io_clrsetbits32(get_base(dev) + RNG_CR, RNG_CR_CONDRST,
				RNG_CR_RNGEN);
	} else {
		io_write32(get_base(dev) + RNG_CR, RNG_CR_RNGEN | dev->pm_cr);
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_rng_pm_suspend(struct stm32_rng_device *dev)
{
	dev->pm_cr = io_read32(get_base(dev) + RNG_CR);

	return TEE_SUCCESS;
}

static TEE_Result
stm32_rng_pm(enum pm_op op, unsigned int pm_hint __unused,
	     const struct pm_callback_handle *pm_handle __unused)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	res = clk_enable(stm32_rng.pdata.clock);
	if (res)
		return res;

	if (op == PM_OP_RESUME)
		res = stm32_rng_pm_resume(&stm32_rng);
	else
		res = stm32_rng_pm_suspend(&stm32_rng);

	clk_disable(stm32_rng.pdata.clock);

	return res;
}
DECLARE_KEEP_PAGER(stm32_rng_pm);

#ifdef CFG_EMBED_DTB
static TEE_Result stm32_rng_parse_fdt(const void *fdt, int node,
				      struct stm32_rng_device *dev)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct dt_node_info dt_rng = { };
	struct stm32_rng_platdata *pdata = &dev->pdata;

	_fdt_fill_device_info(fdt, &dt_rng, node);
	if (dt_rng.clock == DT_INFO_INVALID_CLOCK ||
	    dt_rng.reg == DT_INFO_INVALID_REG ||
	    dt_rng.reset == DT_INFO_INVALID_RESET) {
		return TEE_ERROR_BAD_PARAMETERS;
	}

	pdata->base.pa = dt_rng.reg;

	pdata->base.va = io_pa_or_va_secure(&pdata->base, dt_rng.reg_size);
	assert(pdata->base.va);

	pdata->reset = dt_rng.reset;
	pdata->clock_error = false;

	res = clk_dt_get_by_index(fdt, node, 0, &pdata->clock);
	if (res)
		return res;

	if (fdt_getprop(fdt, node, "clock-error-detect", NULL))
		pdata->clock_error = true;

	return TEE_SUCCESS;
}

TEE_Result __weak
stm32_rng_get_platdata(struct stm32_rng_platdata *pdata __unused)
{
	/* In DT config, the platform data are filled by DT file */
	return TEE_SUCCESS;
}
#else /* CFG_EMBED_DTB */
static TEE_Result stm32_rng_parse_fdt(struct stm32_rng_device *dev __unused)
{
	/* Do nothing, there is no fdt to parse in this case */
	return TEE_SUCCESS;
}

/*
 * This function can be overridden by platform to define pdata of stm32 driver
 */
TEE_Result __weak
stm32_rng_get_platdata(struct stm32_rng_platdata *pdata __unused)
{
	return TEE_ERROR_NOT_IMPLEMENTED;
}
#endif /* CFG_EMBED_DTB */

static TEE_Result stm32_rng_probe(const void *fdt, int offs,
				  const void *compat_data)
{
	TEE_Result res = TEE_SUCCESS;

	/* Expect a single RNG instance */
	assert(!stm32_rng.pdata.base.pa);

	res = stm32_rng_get_platdata(&stm32_rng.pdata);
	if (res)
		goto err;

	res = stm32_rng_parse_fdt(fdt, offs, &stm32_rng);
	if (res)
		goto err;

	stm32_rng.ddata = (struct stm32_rng_driver_data *)compat_data;

	res = clk_enable(stm32_rng.pdata.clock);
	if (res)
		goto err;

	res = stm32_reset_assert(stm32_rng.pdata.reset, RNG_TIMEOUT_US_1MS);
	if (res)
		goto err;

	res = stm32_reset_deassert(stm32_rng.pdata.reset, RNG_TIMEOUT_US_1MS);
	if (res)
		goto err;

	res = stm32_rng_init(&stm32_rng);
	if (res)
		goto err;

	clk_disable(stm32_rng.pdata.clock);

#ifdef CFG_STM32MP15
	stm32mp_register_secure_periph_iomem(stm32_rng.pdata.base.pa);
#endif

	register_pm_core_service_cb(stm32_rng_pm, &stm32_rng, "rng-service");

	return TEE_SUCCESS;

err:
	memset(&stm32_rng, 0, sizeof(stm32_rng));
	return res;
}

static const struct stm32_rng_driver_data mp13_data[] = {
	{ .has_cond_reset = true },
};

static const struct stm32_rng_driver_data mp15_data[] = {
	{ .has_cond_reset = false },
};
DECLARE_KEEP_PAGER(mp15_data);

static const struct dt_device_match rng_match_table[] = {
	{ .compatible = "st,stm32mp13-rng", .compat_data = &mp13_data },
	{ .compatible = "st,stm32-rng", .compat_data = &mp15_data },
	{ }
};

DEFINE_DT_DRIVER(stm32_rng_dt_driver) = {
	.name = "stm32_rng",
	.match_table = rng_match_table,
	.probe = stm32_rng_probe,
};
