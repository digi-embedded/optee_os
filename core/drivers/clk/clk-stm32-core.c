// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * Copyright (C) 2018-2021, STMicroelectronics - All Rights Reserved
 */

#include <config.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <io.h>
#include <kernel/boot.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <libfdt.h>
#include <stdio.h>
#include <stm32_util.h>

#include "clk-stm32-core.h"

#define RCC_MP_ENCLRR_OFFSET	0x4

static struct clk_stm32_priv *stm32_clock_data;

inline struct clk_stm32_priv *clk_stm32_get_priv(void)
{
	return stm32_clock_data;
}

uintptr_t clk_stm32_get_rcc_base(void)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();

	return priv->base;
}

#define TIMEOUT_US_200MS	U(200000)
#define TIMEOUT_US_1S		U(1000000)

/* STM32 MUX API */
int clk_stm32_get_parent_mux(uint32_t mux_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct mux_cfg *mux = &priv->muxes[mux_id];
	uint32_t mask = MASK_WIDTH_SHIFT(mux->width, mux->shift);

	return (io_read32(priv->base + mux->offset) & mask) >> mux->shift;
}

int clk_stm32_set_parent_mux(uint16_t mux_id, uint8_t sel)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct mux_cfg *mux = &priv->muxes[mux_id];
	uint32_t mask = MASK_WIDTH_SHIFT(mux->width, mux->shift);
	uintptr_t address = priv->base + mux->offset;

	io_clrsetbits32(address, mask, (sel << mux->shift) & mask);

	if (mux->ready == MUX_NO_RDY)
		return 0;

	return clk_stm32_wait_ready_gate((uint16_t) mux->ready, true);
}

/* STM32 GATE API */
void clk_stm32_endisable_gate(uint16_t gate_id, bool enable)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct gate_cfg *gate = &priv->gates[gate_id];
	uintptr_t addr = priv->base + gate->offset;

	if (enable) {
		if (gate->set_clr != 0U)
			io_write32(addr, BIT(gate->bit_idx));
		else
			io_setbits32_stm32shregs(addr, BIT(gate->bit_idx));
	} else {
		if (gate->set_clr != 0U)
			io_write32(addr + RCC_MP_ENCLRR_OFFSET, BIT(gate->bit_idx));
		else
			io_clrbits32_stm32shregs(addr, BIT(gate->bit_idx));
	}
}

void clk_stm32_disable_gate(uint16_t gate_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	uint8_t *gate_cpt = priv->gate_cpt;

	if (--gate_cpt[gate_id] > 0)
		return;

	clk_stm32_endisable_gate(gate_id, false);
}

int clk_stm32_enable_gate(uint16_t gate_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	uint8_t *gate_cpt = priv->gate_cpt;

	if (gate_cpt[gate_id]++ > 0)
		return 0;

	clk_stm32_endisable_gate(gate_id, true);

	return 0;
}

bool clk_stm32_is_enabled_gate(uint16_t gate_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct gate_cfg *gate = &priv->gates[gate_id];
	uintptr_t addr = priv->base + gate->offset;

	return (io_read32(addr) & BIT(gate->bit_idx)) != 0U;
}

int clk_stm32_wait_ready_gate(uint16_t gate_id, bool ready_on)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct gate_cfg *gate = &priv->gates[gate_id];
	uintptr_t address = priv->base + gate->offset;
	uint32_t mask_rdy = BIT(gate->bit_idx);
	uint64_t timeout = timeout_init_us(TIMEOUT_US_1S);
	uint32_t mask = 0U;

	if (ready_on)
		mask = BIT(gate->bit_idx);

	while ((io_read32(address) & mask_rdy) != mask)
		if (timeout_elapsed(timeout))
			break;

	if ((io_read32(address) & mask_rdy) != mask)
		return -1;

	return 0;
}

/* STM32 GATE READY CLOCK OPS */
int clk_stm32_gate_ready_endisable(uint16_t gate_id, bool enable, bool wait_rdy)
{
	clk_stm32_endisable_gate(gate_id, enable);

	if (wait_rdy == true)
		return clk_stm32_wait_ready_gate(gate_id + 1, enable);

	return 0;
}

int clk_stm32_gate_rdy_enable(uint16_t gate_id)
{
	return clk_stm32_gate_ready_endisable(gate_id, true, true);
}

int clk_stm32_gate_rdy_disable(uint16_t gate_id)
{
	return clk_stm32_gate_ready_endisable(gate_id, false, true);
}

/* STM32 DIV API */
#define clk_div_mask(_width) GENMASK_32(((_width) - 1U), 0U)

static unsigned int _get_table_div(const struct div_table_cfg *table,
				   unsigned int val)
{
	const struct div_table_cfg *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->val == val)
			return clkt->div;

	return 0;
}


static unsigned int _get_table_val(const struct div_table_cfg *table,
				   unsigned int div)
{
	const struct div_table_cfg *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->div == div)
			return clkt->val;
	return 0;
}

static unsigned int _get_div(const struct div_table_cfg *table,
			     unsigned int val, unsigned long flags,
			     uint8_t width)
{
	if ((flags & CLK_DIVIDER_ONE_BASED) != 0UL)
		return val;

	if ((flags & CLK_DIVIDER_POWER_OF_TWO) != 0UL)
		return BIT(val);

	if ((flags & CLK_DIVIDER_MAX_AT_ZERO) != 0UL)
		return (val != 0U) ? val : BIT(width);

	if (table != NULL)
		return _get_table_div(table, val);

	return val + 1U;
}

static unsigned int _get_val(const struct div_table_cfg *table,
			     unsigned int div, unsigned long flags,
			     uint8_t width)
{
	if ((flags & CLK_DIVIDER_ONE_BASED) != 0UL)
		return div;

	if ((flags & CLK_DIVIDER_POWER_OF_TWO) != 0UL)
		return __builtin_ffs(div) - 1;

	if ((flags & CLK_DIVIDER_MAX_AT_ZERO) != 0UL)
		return (div != 0U) ? div : BIT(width);

	if (table != NULL)
		return _get_table_val(table, div);

	return div - 1U;
}

static bool is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

static bool _is_valid_table_div(const struct div_table_cfg *table,
				unsigned int div)
{
	const struct div_table_cfg *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->div == div)
			return true;

	return false;
}

static bool _is_valid_div(const struct div_table_cfg *table,
			  unsigned int div, unsigned long flags)
{
	if (flags & CLK_DIVIDER_POWER_OF_TWO)
		return is_power_of_2(div);

	if (table)
		return _is_valid_table_div(table, div);

	return true;
}

static int divider_get_val(unsigned long rate, unsigned long parent_rate,
			   const struct div_table_cfg *table, uint8_t width,
			   unsigned long flags)
{
	unsigned int div = 0U;
	unsigned int value = 0U;

	div = UDIV_ROUND_NEAREST((uint64_t)parent_rate, rate);

	if (!_is_valid_div(table, div, flags))
		return -1;

	value = _get_val(table, div, flags, width);

	return MIN(value, clk_div_mask(width));
}

uint32_t clk_stm32_div_get_value(int div_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct div_cfg *divider = &priv->div[div_id];
	uint32_t val = 0;

	val = io_read32(priv->base + divider->offset) >> divider->shift;
	val &= clk_div_mask(divider->width);

	return val;
}

int clk_stm32_set_div_value(uint32_t div_id, uint32_t value)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct div_cfg *divider;
	uintptr_t address;
	uint32_t mask;

	if (div_id >= priv->nb_div)
		panic();

	divider = &priv->div[div_id];
	address = priv->base + divider->offset;

	mask = MASK_WIDTH_SHIFT(divider->width, divider->shift);
	io_clrsetbits32(address, mask, (value << divider->shift) & mask);

	if (divider->ready == DIV_NO_RDY)
		return 0;

	return clk_stm32_wait_ready_gate((uint16_t) divider->ready, true);
}

unsigned long clk_stm32_get_rate_divider(int div_id, unsigned long prate)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct div_cfg *divider = &priv->div[div_id];
	uint32_t val = clk_stm32_div_get_value(div_id);
	unsigned int div = 0U;

	div = _get_div(divider->table, val, divider->flags, divider->width);
	if (div == 0U)
		return prate;

	return ROUNDUP_DIV((uint64_t)prate, div);
}

int clk_stm32_set_rate_divider(int div_id, unsigned long rate,
			       unsigned long prate)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	const struct div_cfg *divider = &priv->div[div_id];
	int value = 0;

	value = divider_get_val(rate, prate, divider->table,
				divider->width, divider->flags);

	if (value < 0)
		return value;

	return clk_stm32_set_div_value(div_id, value);
}

/* STM32 MUX CLOCK OPS */
size_t clk_stm32_mux_get_parent(struct clk *clk)
{
	struct clk_stm32_mux_cfg *cfg = clk->priv;

	return clk_stm32_get_parent_mux(cfg->mux_id);
}

TEE_Result clk_stm32_mux_set_parent(struct clk *clk, size_t pidx)
{
	struct clk_stm32_mux_cfg *cfg = clk->priv;

	return clk_stm32_set_parent_mux(cfg->mux_id, pidx);
}

const struct clk_ops clk_stm32_mux_ops = {
	.get_parent	= clk_stm32_mux_get_parent,
	.set_parent	= clk_stm32_mux_set_parent,
};

/* STM32 GATE CLOCK OPS */
static TEE_Result clk_stm32_gate_enable(struct clk *clk)
{
	struct clk_stm32_gate_cfg *cfg = clk->priv;

	return clk_stm32_enable_gate(cfg->gate_id);
}

static void clk_stm32_gate_disable(struct clk *clk)
{
	struct clk_stm32_gate_cfg *cfg = clk->priv;

	clk_stm32_disable_gate(cfg->gate_id);
}

static bool clk_stm32_gate_is_enabled(struct clk *clk)
{
	struct clk_stm32_gate_cfg *cfg = clk->priv;

	return clk_stm32_is_enabled_gate(cfg->gate_id);
}

const struct clk_ops clk_stm32_gate_ops = {
	.enable		= clk_stm32_gate_enable,
	.disable	= clk_stm32_gate_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
};

static TEE_Result clk_stm32_gate_ready_enable(struct clk *clk)
{
	struct clk_stm32_gate_cfg *cfg = clk->priv;

	if (clk_stm32_gate_rdy_enable(cfg->gate_id) != 0U) {
		EMSG("%s timeout\n", clk_get_name(clk));
		panic();
	}

	return 0;
}

static void clk_stm32_gate_ready_disable(struct clk *clk)
{
	struct clk_stm32_gate_cfg *cfg = clk->priv;

	if (clk_stm32_gate_rdy_disable(cfg->gate_id) != 0U)
		panic();
}

const struct clk_ops clk_stm32_gate_ready_ops = {
	.enable		= clk_stm32_gate_ready_enable,
	.disable	= clk_stm32_gate_ready_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
};

/* STM32 DIV CLOCK OPS */
unsigned long clk_stm32_divider_get_rate(struct clk *clk,
					unsigned long parent_rate)
{
	struct clk_stm32_div_cfg *cfg = clk->priv;

	return clk_stm32_get_rate_divider(cfg->div_id, parent_rate);
}

TEE_Result clk_stm32_divider_set_rate(struct clk *clk,
				      unsigned long rate,
				      unsigned long parent_rate)
{
	struct clk_stm32_div_cfg *cfg = clk->priv;

	return clk_stm32_set_rate_divider(cfg->div_id, rate, parent_rate);
}

const struct clk_ops clk_stm32_divider_ops = {
	.get_rate	= clk_stm32_divider_get_rate,
	.set_rate	= clk_stm32_divider_set_rate,
};

/* STM32 COMPOSITE CLOCK OPS */
size_t clk_stm32_composite_get_parent(struct clk *clk)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	if (cfg->mux_id == NO_MUX) {
		/* TODO: it could be a normal case */
		return 0;
	}

	return clk_stm32_get_parent_mux(cfg->mux_id);
}

TEE_Result clk_stm32_composite_set_parent(struct clk *clk, size_t pidx)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	if (cfg->mux_id == NO_MUX) {
		panic();
	}

	return clk_stm32_set_parent_mux(cfg->mux_id, pidx);
}

unsigned long clk_stm32_composite_get_rate(struct clk *clk,
					   unsigned long parent_rate)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	if (cfg->div_id == NO_DIV)
		return parent_rate;

	return clk_stm32_get_rate_divider(cfg->div_id, parent_rate);
}

TEE_Result clk_stm32_composite_set_rate(struct clk *clk, unsigned long rate,
					unsigned long parent_rate)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	if (cfg->div_id == NO_DIV)
		return 0;

	return clk_stm32_set_rate_divider(cfg->div_id, rate, parent_rate);
}

TEE_Result clk_stm32_composite_gate_enable(struct clk *clk)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	return clk_stm32_enable_gate(cfg->gate_id);
}

void clk_stm32_composite_gate_disable(struct clk *clk)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	clk_stm32_disable_gate(cfg->gate_id);
}

bool clk_stm32_composite_gate_is_enabled(struct clk *clk)
{
	struct clk_stm32_composite_cfg *cfg = clk->priv;

	return clk_stm32_is_enabled_gate(cfg->gate_id);
}

const struct clk_ops clk_stm32_composite_ops = {
	.get_parent	= clk_stm32_composite_get_parent,
	.set_parent	= clk_stm32_composite_set_parent,
	.get_rate	= clk_stm32_composite_get_rate,
	.set_rate	= clk_stm32_composite_set_rate,
	.enable		= clk_stm32_composite_gate_enable,
	.disable	= clk_stm32_composite_gate_disable,
	.is_enabled	= clk_stm32_composite_gate_is_enabled,
};

TEE_Result clk_stm32_set_parent_by_index(struct clk *clk, size_t pidx)
{
	struct clk *parent = clk_get_parent_by_index(clk, pidx);
	TEE_Result res = TEE_ERROR_GENERIC;

	if (parent)
		res = clk_set_parent(clk, parent);

	return res;
}

static bool clk_stm32_get_ignore_unused_property(void)
{
	int node;
	const char *prop;
	const void *fdt;

	fdt = get_embedded_dt();
	if (!fdt)
		panic();

	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return false;

	prop = fdt_getprop(fdt, node, "bootargs", NULL);
	if (prop == NULL)
		return false;

	return strcmp(prop, "clk_ignore_unused") == 0;
}

int clk_stm32_parse_fdt_by_name(const void *fdt, int node, const char *name,
				uint32_t *tab, uint32_t *nb)
{
	const fdt32_t *cell;
	int len = 0;
	uint32_t i;

	cell = fdt_getprop(fdt, node, name, &len);
	if (cell != NULL) {
		for (i = 0; i < ((uint32_t)len / sizeof(uint32_t)); i++) {
			uint32_t val = fdt32_to_cpu(cell[i]);

			tab[i] = val;
		}
	}

	*nb = (uint32_t)len / sizeof(uint32_t);

	return 0;
}
int clk_stm32_init(struct clk_stm32_priv *priv, uintptr_t base)
{
	stm32_clock_data = priv;

	priv->base = base;

	priv->gate_cpt = calloc(priv->nb_gates, sizeof(uint8_t));
	if (!priv->gate_cpt)
		return TEE_ERROR_OUT_OF_MEMORY;

	priv->clk_ignore_unused = clk_stm32_get_ignore_unused_property();

	return 0;
}

unsigned long fixed_factor_get_rate(struct clk *clk, unsigned long parent_rate)
{
	struct fixed_factor_cfg *d = clk->priv;

	unsigned long long rate = (unsigned long long)parent_rate * d->mult;

	if (d->div == 0U)
		panic("error division by zero\n");

	return (unsigned long)(rate / d->div);
};

const struct clk_ops clk_fixed_factor_ops = {
	.get_rate	= fixed_factor_get_rate,
};

static unsigned long clk_fixed_get_rate(struct clk *clk,
					unsigned long parent_rate __unused)
{
	struct clk_fixed_rate_cfg *cfg = clk->priv;

	return cfg->rate;
}

const struct clk_ops clk_fixed_clk_ops = {
	.get_rate	= clk_fixed_get_rate,
};

struct clk *stm32mp_rcc_clock_id_to_clk(unsigned long clock_id)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();

	if (clock_id > priv->nb_clk_refs)
		return NULL;

	return priv->clk_refs[clock_id];
}

static struct clk *stm32mp_clk_dt_get_clk(struct dt_driver_phandle_args *pargs,
					  void *data __unused, TEE_Result *res)
{
	unsigned long clock_id = pargs->args[0];
	struct clk *clk = NULL;

	*res = TEE_ERROR_BAD_PARAMETERS;

	if (pargs->args_count != 1)
		return NULL;

	clk = stm32mp_rcc_clock_id_to_clk(clock_id);
	if (!clk)
		return NULL;

	*res = TEE_SUCCESS;
	return clk;
}

static void clk_stm32_register_clocks(struct clk_stm32_priv *priv)
{
	size_t i = 0;

	for(i = 0; i < priv->nb_clk_refs; i++) {
		struct clk *clk = priv->clk_refs[i];

		if (clk == NULL)
			continue;

		refcount_set(&clk->enabled_count, 0);

		if (clk_register(clk))
			panic();
	}

	/* Critical clocks management */
	for(i = 0; i < priv->nb_clk_refs; i++) {
		struct clk *clk = priv->clk_refs[i];

		if (clk == NULL)
			continue;

		if (priv->is_critical && priv->is_critical(clk))
			clk_enable(clk);
	}
}

#ifdef CFG_STM32_CLK_DEBUG
static void clk_stm32_display_clock_ignore_unused(void)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	size_t i;

	printf("\nCLOCK CLK_IGNORE_UNUSED:\n");

	printf("\tSTATUS = %s\n",
		priv->clk_ignore_unused ? "ENABLED" : "DISABLED");

	printf("CLOCK WITH CLK_IGNORE_UNUSED FLAGS:\n");
	for(i = 0; i < priv->nb_clk_refs; i++) {
		struct clk *clk = priv->clk_refs[i];

		if (priv->is_ignore_unused && priv->is_ignore_unused(clk))
				printf("\t%s\n", clk_get_name(clk));
	}

	printf("CLOCK DISABLED IF CLK_IGNORE_UNUSED IS DISABLED:\n");
	for(i = 0; i < priv->nb_clk_refs; i++) {
		struct clk *clk = priv->clk_refs[i];

		if (clk == NULL)
			continue;

		/* if counter > 0 */
		if (clk_is_enabled(clk))
			continue;

		if (priv->is_ignore_unused && priv->is_ignore_unused(clk))
			continue;

		if (clk->ops->is_enabled && clk->ops->is_enabled(clk) &&
		    clk->ops->disable) {
			printf("\t%s, EN = %d COUNTER = %d\n", clk_get_name(clk),
				clk->ops->is_enabled(clk),
				clk->enabled_count.val);
		}
	}
	printf("\n");
}
#endif

void clk_stm32_clock_ignore_unused(void)
{
	struct clk_stm32_priv *priv = clk_stm32_get_priv();
	size_t i;

#ifdef CFG_STM32_CLK_DEBUG
	clk_stm32_display_clock_ignore_unused();
#endif

	if (priv->clk_ignore_unused)
		return;

	for(i = 0; i < priv->nb_clk_refs; i++) {
		struct clk *clk = priv->clk_refs[i];

		if (clk == NULL)
			continue;

		/* if counter > 0 */
		if (clk_is_enabled(clk))
			continue;

		if (priv->is_ignore_unused && priv->is_ignore_unused(clk))
			continue;

		if (clk->ops->is_enabled && clk->ops->is_enabled(clk) &&
		    clk->ops->disable) {
			DMSG("%s: disabling %s ...\n", __func__,
			     clk_get_name(clk));
			clk->ops->disable(clk);
		    }
	}
}

void stm32mp_clk_provider_probe_final(const void *fdt, int node,
				      struct clk_stm32_priv *priv)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	clk_stm32_register_clocks(priv);

	res = clk_dt_register_clk_provider(fdt, node, stm32mp_clk_dt_get_clk,
					   priv);
	if (res)
		panic("Couldn't register clock provider");
}
