// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2020-2021, STMicroelectronics
 */

#include <assert.h>
#include <compiler.h>
#include <drivers/regulator.h>
#include <drivers/stm32_gpio.h>
#include <gpio.h>
#include <initcall.h>
#include <keep.h>
#include <kernel/boot.h>
#include <kernel/dt.h>
#include <kernel/panic.h>
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <stm32_util.h>
#include <trace.h>

#define GPIO_REGULATOR_NAME_LEN	16
#define GPIO_REGULATOR_MAX_STATES	2

struct gpio_regul {
	char name[GPIO_REGULATOR_NAME_LEN];
	struct regul_desc desc;
	struct stm32_pinctrl_list *pinctrl;
	uint16_t gpio_low_mv;
	uint16_t gpio_high_mv;
	uint16_t gpio_voltage_table[2];
};

static TEE_Result gpio_set_state(const struct regul_desc *desc __unused,
				 bool enable __unused)
{
	return TEE_SUCCESS;
}

static TEE_Result gpio_get_state(const struct regul_desc *desc __unused,
				 bool *enabled)
{
	*enabled = true;

	return TEE_SUCCESS;
}

static TEE_Result gpio_get_voltage(const struct regul_desc *desc, uint16_t *mv)
{
	struct gpio_regul *gr = (struct gpio_regul *)desc->driver_data;
	struct stm32_pinctrl *pinctrl = NULL;
	unsigned int gpio = 0;

	FMSG("%s: get volt", desc->node_name);

	pinctrl = STAILQ_FIRST(gr->pinctrl);
	if (!pinctrl)
		panic();

	gpio = stm32_pinctrl_get_gpio_id(pinctrl);

	if (stm32_gpio_get_ops()->get_value(NULL, gpio) == GPIO_LEVEL_HIGH)
		*mv = gr->gpio_high_mv;
	else
		*mv = gr->gpio_low_mv;

	return TEE_SUCCESS;
}

static TEE_Result gpio_set_voltage(const struct regul_desc *desc, uint16_t mv)
{
	struct gpio_regul *gr = (struct gpio_regul *)desc->driver_data;
	struct stm32_pinctrl *pinctrl = NULL;
	unsigned int gpio = 0;
	enum gpio_level level = GPIO_LEVEL_LOW;

	FMSG("%s: set volt", desc->node_name);

	pinctrl = STAILQ_FIRST(gr->pinctrl);
	if (!pinctrl)
		panic();

	gpio = stm32_pinctrl_get_gpio_id(pinctrl);

	if (mv == gr->gpio_high_mv)
		level = GPIO_LEVEL_HIGH;
	else if (mv != gr->gpio_low_mv)
		panic();

	stm32_gpio_get_ops()->set_value(NULL, gpio, level);

	return TEE_SUCCESS;
}

static TEE_Result gpio_list_voltages(const struct regul_desc *desc,
				     uint16_t **levels, size_t *count)
{
	struct gpio_regul *gr = (struct gpio_regul *)desc->driver_data;

	*count = 2;
	*levels = gr->gpio_voltage_table;

	return TEE_SUCCESS;
}

static struct regul_ops gpio_regul_ops = {
	.set_state = gpio_set_state,
	.get_state = gpio_get_state,
	.set_voltage = gpio_set_voltage,
	.get_voltage = gpio_get_voltage,
	.list_voltages = gpio_list_voltages,
};
DECLARE_KEEP_PAGER(gpio_regul_ops);

static TEE_Result gpio_regulator_probe(const void *fdt, int node,
				       const void *compat_data __unused)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	size_t len = 0;
	struct gpio_regul *gr = NULL;
	const char *reg_name = NULL;
	const fdt32_t *cuint = NULL;
	struct stm32_pinctrl *pinctrl = NULL;

	gr = calloc(1, sizeof(*gr));
	if (!gr)
		return TEE_ERROR_OUT_OF_MEMORY;

	reg_name = fdt_get_name(fdt, node, NULL);
	len = snprintf(gr->name, sizeof(gr->name) - 1, "%s", reg_name);
	assert(len > 0 && len < (sizeof(gr->name) - 1));

	gr->desc.node_name = gr->name;
	gr->desc.driver_data = gr;
	gr->desc.ops = &gpio_regul_ops;

	res = stm32_pinctrl_dt_get_by_index(fdt, node, 0, &gr->pinctrl);
	if(res)
		return res;

	len = 0;

	STAILQ_FOREACH(pinctrl, gr->pinctrl, link)
		len++;

	if (len > 1)
		panic("Too many PINCTRLs found");

	cuint = fdt_getprop(fdt, node, "low-level-microvolt", NULL);
	if (cuint)
		gr->gpio_low_mv = (uint16_t)(fdt32_to_cpu(*cuint) / 1000U);

	cuint = fdt_getprop(fdt, node, "high-level-microvolt", NULL);
	if (cuint)
		gr->gpio_high_mv = (uint16_t)(fdt32_to_cpu(*cuint) / 1000U);

	if (gr->gpio_low_mv < gr->gpio_high_mv) {
		gr->gpio_voltage_table[0] = gr->gpio_low_mv;
		gr->gpio_voltage_table[1] = gr->gpio_high_mv;
	} else {
		gr->gpio_voltage_table[0] = gr->gpio_high_mv;
		gr->gpio_voltage_table[1] = gr->gpio_low_mv;
	}

	res = regulator_register(&gr->desc, node);
	if (res) {
		EMSG("regulator_register(%s) failed with %#"PRIx32,
		     reg_name, res);
		panic();
	}

	return TEE_SUCCESS;
}

static const struct dt_device_match gpio_regulator_match_table[] = {
	{ .compatible = "st,stm32-regulator-gpio" },
	{ }
};

DEFINE_DT_DRIVER(stm32_gpio_regulator_dt_driver) = {
	.name = "stm32-gpio-regulator",
	.match_table = gpio_regulator_match_table,
	.probe = gpio_regulator_probe,
};
