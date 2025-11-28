/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2018-2022, STMicroelectronics
 */

#ifndef __STM32_UTIL_H__
#define __STM32_UTIL_H__

#include <assert.h>
#include <drivers/clk.h>
#include <drivers/pinctrl.h>
#include <drivers/stm32mp1_rcc_util.h>
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

/* Get device ID from SYSCFG registers */
uint32_t stm32mp_syscfg_get_chip_dev_id(void);

/* Erase ESRAM3 */
TEE_Result stm32mp_syscfg_erase_sram3(void);

/* Platform util for the GIC */
vaddr_t get_gicd_base(void);

#ifdef CFG_STPMIC1
bool stm32_stpmic1_is_present(void);
#else
static inline bool stm32_stpmic1_is_present(void)
{
	return false;
}
#endif

#ifdef CFG_STPMIC2
bool stm32_stpmic2_is_present(void);
#else
static inline bool stm32_stpmic2_is_present(void)
{
	return false;
}
#endif

static inline bool stm32mp_with_pmic(void)
{
	return stm32_stpmic1_is_present() || stm32_stpmic2_is_present();
}

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

/*
 * Shared reference counter: increments by 2 on secure increment
 * request, decrements by 2 on secure decrement request. Bit #0
 * is set to 1 on non-secure increment request and reset to 0 on
 * non-secure decrement request. These counters initialize to
 * either 0, 1 or 2 upon their expect default state.
 * Counters saturate to UINT_MAX / 2.
 */
#define SHREFCNT_NONSECURE_FLAG		0x1ul
#define SHREFCNT_SECURE_STEP		0x2ul
#define SHREFCNT_MAX			(UINT_MAX / 2)

/* Return 1 if refcnt increments from 0, else return 0 */
static inline int incr_shrefcnt(unsigned int *refcnt, bool secure)
{
	int rc = !*refcnt;

	if (secure) {
		if (*refcnt < SHREFCNT_MAX) {
			*refcnt += SHREFCNT_SECURE_STEP;
			assert(*refcnt < SHREFCNT_MAX);
		}
	} else {
		*refcnt |= SHREFCNT_NONSECURE_FLAG;
	}

	return rc;
}

/* Return 1 if refcnt decrements to 0, else return 0 */
static inline int decr_shrefcnt(unsigned int *refcnt, bool secure)
{
	int  rc = 0;

	if (secure) {
		if (*refcnt < SHREFCNT_MAX) {
			if (*refcnt < SHREFCNT_SECURE_STEP)
				panic();

			*refcnt -= SHREFCNT_SECURE_STEP;
			rc = !*refcnt;
		}
	} else {
		rc = (*refcnt == SHREFCNT_NONSECURE_FLAG);
		*refcnt &= ~SHREFCNT_NONSECURE_FLAG;
	}

	return rc;
}

static inline int incr_refcnt(unsigned int *refcnt)
{
	return incr_shrefcnt(refcnt, true);
}

static inline int decr_refcnt(unsigned int *refcnt)
{
	return decr_shrefcnt(refcnt, true);
}

void __noreturn do_reset(const char *str);

TEE_Result stm32_activate_internal_tamper(int id);

bool stm32mp_allow_probe_shared_device(const void *fdt, int node);

#if defined(CFG_STM32MP15) && defined(CFG_WITH_PAGER)
/*
 * Return the SRAM alias physical address related to @pa when applicable or
 * @pa if it does not relate to an SRAMx non-aliased memory address.
 */
paddr_t stm32mp1_pa_or_sram_alias_pa(paddr_t pa);

/* Return whether or not the physical address range intersec pager secure RAM */
bool stm32mp1_ram_intersect_pager_ram(paddr_t base, size_t size);
#else
static inline paddr_t stm32mp1_pa_or_sram_alias_pa(paddr_t pa)
{
	return pa;
}

static inline bool stm32mp1_ram_intersect_pager_ram(paddr_t base __unused,
						    size_t size __unused)
{
	return false;
}
#endif
#endif /*__STM32_UTIL_H__*/
