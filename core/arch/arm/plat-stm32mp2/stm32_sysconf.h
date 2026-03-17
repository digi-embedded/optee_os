/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2022, STMicroelectronics
 */

#ifndef __STM32_SYSCONF_H__
#define __STM32_SYSCONF_H__

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <tee_api_types.h>
#include <types_ext.h>

/* syscon banks */
enum syscon_banks {
	SYSCON_SYSCFG,
	SYSCON_CA35SS,
	SYSCON_NB_BANKS
};

#define SYSCON_ID(bank, offset) (((bank) << 16) | \
				((offset) & GENMASK_32(15, 0)))

/*
 * SYSCFG register offsets (base relative)
 */
#define SYSCFG_VDERAMCR		SYSCON_ID(SYSCON_SYSCFG, 0x1800U)
#define SYSCFG_POTTAMPRSTCR	SYSCON_ID(SYSCON_SYSCFG, 0x1804U)
#define SYSCFG_HPDMAARCR	SYSCON_ID(SYSCON_SYSCFG, 0x2050U)

/*
 * SYSCFG_VDERAMCR register fields
 */
#define VDERAMCR_VDERAM_EN		BIT(0)
#define VDERAMCR_MASK			BIT(0)

/*
 * SYSCFG HPDMA address remapping control register (SYSCFG_HPDMAARCR)
 */
#define SYSCFG_HPDMAARCR_HPDMA1AREN	BIT(0)
#define SYSCFG_HPDMAARCR_HPDMA2AREN	BIT(1)
#define SYSCFG_HPDMAARCR_HPDMA3AREN	BIT(2)

/*
 * CA35SS register offsets (base relative)
 * - Standardized Status and Control registers (SSC) access modes (offset=0x0)
 * - SYSCFG registers (offset=0x2000)
 */
#define CA35SS_SSC_CHGCLKREQ		SYSCON_ID(SYSCON_CA35SS, 0x0U)
#define CA35SS_SSC_PLL_FREQ1		SYSCON_ID(SYSCON_CA35SS, 0x80U)
#define CA35SS_SSC_PLL_FREQ2		SYSCON_ID(SYSCON_CA35SS, 0x90U)
#define CA35SS_SSC_PLL_EN		SYSCON_ID(SYSCON_CA35SS, 0xA0U)
#define CA35SS_SSC_LPI_TSGEN_NTS_CR	SYSCON_ID(SYSCON_CA35SS, 0xD0U)
#define CA35SS_SSC_LPI_STGEN_NTS_CR	SYSCON_ID(SYSCON_CA35SS, 0x140U)
#define CA35SS_SSC_OPP_REQ		SYSCON_ID(SYSCON_CA35SS, 0x1A0U)
#define CA35SS_SSC_MEM_CTRL		SYSCON_ID(SYSCON_CA35SS, 0x1C0U)
#define CA35SS_SYSCFG_M33_ACCESS_CR	SYSCON_ID(SYSCON_CA35SS, 0x2088U)
#define CA35SS_SYSCFG_M33_TZEN_CR	SYSCON_ID(SYSCON_CA35SS, 0x20A0U)
#define CA35SS_SYSCFG_M33_INITSVTOR_CR	SYSCON_ID(SYSCON_CA35SS, 0x20A4U)
#define CA35SS_SYSCFG_M33_INITNSVTOR_CR	SYSCON_ID(SYSCON_CA35SS, 0x20A8U)


/*
 * CA35SS_SYSCFG_M33_ACCESS_CR register fields
 */
#define CA35SS_SYSCFG_M33_ACCESS_CR_SEC			BIT(0)
#define CA35SS_SYSCFG_M33_ACCESS_CR_PRIV		BIT(1)

/*
 * CA35SS_SYSCFG_M33_TZEN_CR register fields
 */
#define CA35SS_SYSCFG_M33_TZEN_CR_CFG_SECEXT		BIT(0)

/*
 * CA35SS_SSC_CHGCLKREQ register fields
 */
#define CA35SS_SSC_CHGCLKREQ_ARM_CHGCLKREQ		BIT(0)
#define CA35SS_SSC_CHGCLKREQ_ARM_CHGCLKREQ_MASK		BIT(0)

#define CA35SS_SSC_CHGCLKREQ_ARM_CHGCLKACK_MASK		BIT(1)
#define CA35SS_SSC_CHGCLKREQ_ARM_CHGCLKACK_SHIFT	U(1)

#define CA35SS_SSC_CHGCLKREQ_ARM_DIVSEL			BIT(16)
#define CA35SS_SSC_CHGCLKREQ_ARM_DIVSELACK		BIT(17)

/*
 * CA35SS_SSC_PLL_FREQ1 register fields
 */
#define CA35SS_SSC_PLL_FREQ1_FBDIV_MASK		GENMASK_32(11, 0)
#define CA35SS_SSC_PLL_FREQ1_FBDIV_SHIFT	U(0)

#define CA35SS_SSC_PLL_FREQ1_REFDIV_MASK	GENMASK_32(21, 16)
#define CA35SS_SSC_PLL_FREQ1_REFDIV_SHIFT	U(16)

#define CA35SS_SSC_PLL_FREQ1_MASK	(CA35SS_SSC_PLL_FREQ1_REFDIV_MASK | \
					 CA35SS_SSC_PLL_FREQ1_FBDIV_MASK)

/*
 * CA35SS_SSC_PLL_FREQ2 register fields
 */
#define CA35SS_SSC_PLL_FREQ2_POSTDIV1_MASK	GENMASK_32(2, 0)
#define CA35SS_SSC_PLL_FREQ2_POSTDIV1_SHIFT	U(0)

#define CA35SS_SSC_PLL_FREQ2_POSTDIV2_MASK	GENMASK_32(5, 3)
#define CA35SS_SSC_PLL_FREQ2_POSTDIV2_SHIFT	U(3)

#define CA35SS_SSC_PLL_FREQ2_MASK		GENMASK_32(5, 0)

/*
 * CA35SS_SSC_PLL_EN register fields
 */
#define CA35SS_SSC_PLL_EN_PLL_EN		BIT(0)

#define CA35SS_SSC_PLL_EN_LOCKP_MASK		BIT(1)

#define CA35SS_SSC_PLL_EN_NRESET_SWPLL		BIT(2)
#define CA35SS_SSC_PLL_EN_NRESET_SWPLL_MASK	BIT(2)

/*
 * CA35SS_SSC_LPI_TSGEN_NTS_CR register fields
 */
#define CA35SS_SSC_LPI_TSGEN_CSYSREQ			BIT(8)
#define CA35SS_SSC_LPI_TSGEN_CSYSACK			BIT(9)

/*
 * CA35SS_SSC_LPI_STGEN_NTS_CR register fields
 */
#define CA35SS_SSC_LPI_STGEN_CSYSREQ			BIT(24)
#define CA35SS_SSC_LPI_STGEN_CSYSACK			BIT(25)

/*
 * CA35SS_SSC_OPP_REQ register fields
 */
#define CA35SS_SSC_OPP_REQ_REQ				BIT(0)
#define CA35SS_SSC_OPP_REQ_ACK				BIT(1)

/*
 * CA35SS_SSC_MEM_CTRL register fields
 */
#define CA35SS_SSC_MEM_CTRL_RM_MASK			GENMASK_32(2, 0)
#define CA35SS_SSC_MEM_CTRL_RM_OVERDRIVE		U(3)
#define CA35SS_SSC_MEM_CTRL_RME				BIT(3)

void stm32mp_syscfg_write(uint32_t id, uint32_t value, uint32_t bitmsk);
uint32_t stm32mp_syscfg_read(uint32_t id);

/* IO comp identifiers */
enum syscfg_io_ids {
	SYSFG_VDDIO1_ID,
	SYSFG_VDDIO2_ID,
	SYSFG_VDDIO3_ID,
	SYSFG_VDDIO4_ID,
	SYSFG_VDD_IO_ID,
	SYSFG_NB_IO_ID
};

TEE_Result stm32mp25_syscfg_enable_iocomp(enum syscfg_io_ids id);
TEE_Result stm32mp25_syscfg_disable_iocomp(enum syscfg_io_ids id);
void stm32mp25_syscfg_fixed_iocomp(enum syscfg_io_ids id,
				   uint32_t pmos, uint32_t nmos);

void stm32mp25_syscfg_set_amcr(size_t mm1_size, size_t mm2_size);

/* Safe Reset */
void stm32mp25_syscfg_set_safe_reset(bool status);

#endif /*__STM32_SYSCONF_H__*/
