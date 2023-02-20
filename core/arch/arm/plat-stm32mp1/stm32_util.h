/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2018-2021, STMicroelectronics
 */

#ifndef __STM32_UTIL_H__
#define __STM32_UTIL_H__

#include <assert.h>
#include <drivers/clk.h>
#include <drivers/stm32_bsec.h>
#include <drivers/stm32_gpio.h>
#include <kernel/panic.h>
#include <stdbool.h>
#include <stdint.h>
#include <tee_api_types.h>
#include <types_ext.h>

/* SoC versioning and device ID */
TEE_Result stm32mp1_dbgmcu_get_chip_version(uint32_t *chip_version);
TEE_Result stm32mp1_dbgmcu_get_chip_dev_id(uint32_t *chip_dev_id);

/* OPP service */
bool stm32mp_supports_cpu_opp(uint32_t opp_id);

/*  Crypto HW support */
bool stm32mp_supports_hw_cryp(void);

/*  Second core support */
bool stm32mp_supports_second_core(void);

/* Backup registers and RAM utils */
vaddr_t stm32mp_bkpreg(unsigned int idx);
vaddr_t stm32mp_bkpsram_base(void);

/* Platform util for the STGEN driver */
vaddr_t stm32mp_stgen_base(void);

/* Platform util for the GIC */
void stm32mp_gic_set_end_of_interrupt(uint32_t it);

/*
 * SYSCFG IO compensation.
 * These functions assume non-secure world is suspended.
 */
void stm32mp_syscfg_enable_io_compensation(void);
void stm32mp_syscfg_disable_io_compensation(void);

static inline void stm32mp_syscfg_enable_io_comp(void)
{
	stm32mp_syscfg_enable_io_compensation();
}

static inline void stm32mp_syscfg_disable_io_comp(void)
{
	stm32mp_syscfg_disable_io_compensation();
}

/* Get device ID from SYSCFG registers */
uint32_t stm32mp_syscfg_get_chip_dev_id(void);

/* Erase ESRAM3 */
TEE_Result stm32mp_syscfg_erase_sram3(void);

/* Platform util for the GIC */
vaddr_t get_gicc_base(void);
vaddr_t get_gicd_base(void);

/*
 * Platform util for the IWDG driver
 */

/* Get IWDG_* enable flags mask from watchdog configuration read from fuses */
unsigned long stm32_get_iwdg_otp_config(vaddr_t pbase);

#if TRACE_LEVEL >= TRACE_DEBUG
/*
 * stm32mp_dump_core_registers - Print CPU registers to console
 *
 * @panicking:  False if we are not called from a panic sequence. True if we
 *              are panicking. Trace messages are emitted only once this
 *              function is called with @panicking being true. Until then,
 *              calling with @panicking being false emits no trace.
 */
void stm32mp_dump_core_registers(bool panicking);
#else
static inline void stm32mp_dump_core_registers(bool panicking __unused) { }
#endif

/* Platform util for PMIC support */
bool stm32mp_with_pmic(void);

/* Power management service */
#ifdef CFG_PSCI_ARM32
void stm32mp_register_online_cpu(void);
#else
static inline void stm32mp_register_online_cpu(void)
{
}
#endif

/*
 * Generic spinlock function that bypass spinlock if MMU is disabled or
 * lock is NULL.
 */
uint32_t may_spin_lock(unsigned int *lock);
void may_spin_unlock(unsigned int *lock, uint32_t exceptions);

#ifdef CFG_DRIVERS_CLK
/* Helper from platform RCC clock driver */
struct clk *stm32mp_rcc_clock_id_to_clk(unsigned long clock_id);
unsigned int stm32mp_rcc_clk_to_clock_id(struct clk *clk);
#endif

/* Return true if @clock_id is shared by secure and non-secure worlds */
bool stm32mp_nsec_can_access_clock(unsigned long clock_id);

extern const struct clk_ops stm32mp1_clk_ops;

#if defined(CFG_STPMIC1)
/* Return true if non-secure world can manipulate regulator @pmic_regu_name */
bool stm32mp_nsec_can_access_pmic_regu(const char *pmic_regu_name);
#else
static inline bool stm32mp_nsec_can_access_pmic_regu(const char *name __unused)
{
	return false;
}
#endif

/* PM sequences specific to SoC STOP mode support */
void stm32mp1_clk_save_context_for_stop(void);
void stm32mp1_clk_restore_context_for_stop(void);

/* Protect the MCU clock subsytem */
void stm32mp1_clk_mcuss_protect(bool enable);

/*
 * Util for PLL1 settings management based on DT OPP table content.
 */
int stm32mp1_clk_compute_all_pll1_settings(uint32_t buck1_voltage);
void stm32mp1_clk_lp_save_opp_pll1_settings(uint8_t *data, size_t size);
bool stm32mp1_clk_pll1_settings_are_valid(void);
TEE_Result stm32mp1_set_opp_khz(uint32_t freq_khz);
int stm32mp1_round_opp_khz(uint32_t *freq_khz);
TEE_Result initialize_pll1_settings(void);

/*
 * Util for reset signal assertion/desassertion for stm32 and platform drivers
 * @id: Target peripheral ID, ID used in reset DT bindings
 * @to_us: Timeout out in microsecond, or 0 if not waiting signal state
 */
TEE_Result stm32_reset_assert(unsigned int id, unsigned int timeout_us);
TEE_Result stm32_reset_deassert(unsigned int id, unsigned int timeout_us);

/* Specific reset to manage the MCU hold boot */
void stm32_reset_assert_deassert_mcu(bool assert_not_deassert);

static inline void stm32_reset_set(unsigned int id)
{
	(void)stm32_reset_assert(id, 0);
}

static inline void stm32_reset_release(unsigned int id)
{
	(void)stm32_reset_deassert(id, 0);
}

void stm32_reset_system(void);

/* Return true if and only if @reset_id relates to a non-secure peripheral */
bool stm32mp_nsec_can_access_reset(unsigned int reset_id);

/*
 * Structure and API function for BSEC driver to get some platform data.
 *
 * @base: BSEC interface registers physical base address
 * @upper_start: Base ID for the BSEC upper words in the platform
 * @max_id: Max value for BSEC word ID for the platform
 */
struct stm32_bsec_static_cfg {
	paddr_t base;
	unsigned int upper_start;
	unsigned int max_id;
};

void stm32mp_get_bsec_static_cfg(struct stm32_bsec_static_cfg *cfg);

/*
 * Return true if platform is in closed_device mode
 */
bool stm32mp_is_closed_device(void);

/*
 * Shared registers support: common lock for accessing SoC registers
 * shared between several drivers.
 */
void io_mask32_stm32shregs(vaddr_t va, uint32_t value, uint32_t mask);

static inline void io_setbits32_stm32shregs(vaddr_t va, uint32_t value)
{
	io_mask32_stm32shregs(va, value, value);
}

static inline void io_clrbits32_stm32shregs(vaddr_t va, uint32_t value)
{
	io_mask32_stm32shregs(va, 0, value);
}

void io_clrsetbits32_stm32shregs(vaddr_t va, uint32_t clr, uint32_t set);

/*
 * Shared peripherals and resources registration
 *
 * Resources listed in enum stm32mp_shres assigned at run-time to the
 * non-secure world, to the secure world or shared by both worlds.
 * In the later case, there must exist a secure service in OP-TEE
 * for the non-secure world to access the resource.
 *
 * Resources may be a peripheral, a bus, a clock or a memory.
 *
 * Shared resources driver API functions allows drivers to register the
 * resource as secure, non-secure or shared and to get the resource
 * assignation state.
 */
enum stm32mp_shres {
	STM32MP1_SHRES_IWDG1,
	STM32MP1_SHRES_USART1,
	STM32MP1_SHRES_SPI6,
	STM32MP1_SHRES_I2C4,
	STM32MP1_SHRES_RNG1,
	STM32MP1_SHRES_HASH1,
	STM32MP1_SHRES_CRYP1,
	STM32MP1_SHRES_I2C6,
	STM32MP1_SHRES_RTC,
	STM32MP1_SHRES_MCU,
	STM32MP1_SHRES_PLL3,
	STM32MP1_SHRES_MDMA,

	STM32MP1_SHRES_COUNT
};

/* Register resource @id as a secure peripheral */
void stm32mp_register_secure_periph(enum stm32mp_shres id);

/* Register resource @id as a non-secure peripheral */
void stm32mp_register_non_secure_periph(enum stm32mp_shres id);

/*
 * Register resource identified by @base as a secure peripheral
 * @base: IOMEM physical base address of the resource
 */
void stm32mp_register_secure_periph_iomem(vaddr_t base);

/*
 * Register resource identified by @base as a non-secure peripheral
 * @base: IOMEM physical base address of the resource
 */
void stm32mp_register_non_secure_periph_iomem(vaddr_t base);

/* Return true if and only if resource @id is registered as secure */
bool stm32mp_periph_is_secure(enum stm32mp_shres id);

/* Register parent clocks of @clock (ID used in clock DT bindings) as secure */
void stm32mp_register_clock_parents_secure(unsigned long clock_id);

/*
 * CPU operating point
 */

/* Return the number of CPU operating points */
size_t stm32mp1_cpu_opp_count(void);

/* Get level value identifying CPU operating point @opp_index */
unsigned int stm32mp1_cpu_opp_level(size_t opp_index);

/* Request to switch to CPU operating point related to @level */
TEE_Result stm32mp1_cpu_opp_set_level(unsigned int level);

/* Get level related to current CPU operating point */
TEE_Result stm32mp1_cpu_opp_read_level(unsigned int *level);
#endif /*__STM32_UTIL_H__*/
