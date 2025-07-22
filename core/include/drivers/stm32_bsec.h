/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2017-2022, STMicroelectronics
 */

#ifndef __DRIVERS_STM32_BSEC_H
#define __DRIVERS_STM32_BSEC_H

#include <compiler.h>
#include <stdint.h>
#include <tee_api.h>
#include <types_ext.h>

/* BSEC_DEBUG */
#if defined(CFG_STM32MP25) || defined(CFG_STM32MP23) || defined(CFG_STM32MP21)
#define BSEC_DBGENA			BIT(1)
#define BSEC_NIDENA			BIT(2)
#define BSEC_DEVICEEN			BIT(3)
#define BSEC_HDPEN			BIT(4)
#define BSEC_SPIDENA			BIT(5)
#define BSEC_SPNIDENA			BIT(6)
#define BSEC_DBGSWEN			BIT(7)
#define BSEC_DBGENM			BIT(8)
#define BSEC_NIDENM			BIT(9)
#define BSEC_SPIDENM			BIT(10)
#define BSEC_SPNIDENM			BIT(11)
#else /* STM32MP1x */
#define BSEC_HDPEN			BIT(4)
#define BSEC_SPIDEN			BIT(5)
#define BSEC_SPINDEN			BIT(6)
#define BSEC_DBGSWGEN			BIT(10)
#define BSEC_DEBUG_ALL			(BSEC_HDPEN | \
					 BSEC_SPIDEN | \
					 BSEC_SPINDEN | \
					 BSEC_DBGSWGEN)
#endif
#if defined(CFG_STM32MP21)
#define BSEC_AUTH_UNLOCK_MSK		GENMASK_32(15, 8)
#define BSEC_AUTH_UNLOCK(val)		(((val) << 8) & BSEC_AUTH_UNLOCK_MSK)
#define BSEC_AUTH_HDPL_MSK		GENMASK_32(23, 16)
#define BSEC_AUTH_HDPL(val)		(((val) << 16) & BSEC_AUTH_HDPL_MSK)
#define BSEC_AUTH_SEC_MSK		GENMASK_32(31, 24)
#define BSEC_AUTH_SEC(val)		(((val) << 24) & BSEC_AUTH_SEC_MSK)
#define BSEC_AUTH_UNLOCKED		0xb4
#define BSEC_AUTH_LOCKED		0xff
#define BSEC_AUTH_HDPL0			0xb4
#define BSEC_AUTH_HDPL1			0x51
#define BSEC_AUTH_HDPL2			0x8a
#define BSEC_AUTH_HDPL3			0x6f
#endif

#define BSEC_BITS_PER_WORD		(8U * sizeof(uint32_t))
#define BSEC_BYTES_PER_WORD		sizeof(uint32_t)

/* BSEC different global states */
enum stm32_bsec_sec_state {
	BSEC_STATE_SEC_CLOSED,
	BSEC_STATE_SEC_OPEN,
	BSEC_STATE_INVALID
};

/*
 * Structure and API function for BSEC driver to get some platform data.
 *
 * @base: BSEC interface registers physical base address
 * @mirror: BSEC mirror base address
 * @upper_start: Base ID for the BSEC upper words in the platform
 * @max_id: Max value for BSEC word ID for the platform
 */
struct stm32_bsec_static_cfg {
	paddr_t base;
	paddr_t mirror;
	unsigned int upper_start;
	unsigned int max_id;
};

void plat_bsec_get_static_cfg(struct stm32_bsec_static_cfg *cfg);

/*
 * Load OTP from SAFMEM and provide its value
 * @value: Output read value
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_shadow_read_otp(uint32_t *value, uint32_t otp_id);

/*
 * Copy SAFMEM OTP to BSEC data.
 * @otp_id: OTP number.
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_shadow_register(uint32_t otp_id);

/*
 * Read an OTP data value
 * @value: Output read value
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_read_otp(uint32_t *value, uint32_t otp_id);

/*
 * Read a range of OTP data values thanks to the name of the cell
 * @name: Name of the cell describing the OTP range
 * @len : Size of the OTP range to read
 * @values : Output read values
 */
TEE_Result stm32_bsec_read_otp_range_by_name(const char *name,
					     size_t len, uint8_t **values);

/*
 * Write value in BSEC data register
 * @value: Value to write
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_write_otp(uint32_t value, uint32_t otp_id);

/*
 * Program a bit in SAFMEM without BSEC data refresh
 * @value: Value to program.
 * @otp_id: OTP number.
 * Return a TEE_Result compliant return value
 */
#ifdef CFG_STM32_BSEC_WRITE
TEE_Result stm32_bsec_program_otp(uint32_t value, uint32_t otp_id);
#else
static inline TEE_Result stm32_bsec_program_otp(uint32_t value __unused,
						uint32_t otp_id __unused)
{
	return TEE_ERROR_NOT_SUPPORTED;
}
#endif

/*
 * Permanent lock of OTP in SAFMEM
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
#ifdef CFG_STM32_BSEC_WRITE
TEE_Result stm32_bsec_permanent_lock_otp(uint32_t otp_id);
#else
static inline TEE_Result stm32_bsec_permanent_lock_otp(uint32_t otp_id __unused)
{
	return TEE_ERROR_NOT_SUPPORTED;
}
#endif

/*
 * Enable/disable debug service
 * @value: Value to write
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_write_debug_conf(uint32_t value);

/* Return debug configuration read from BSEC */
uint32_t stm32_bsec_read_debug_conf(void);

#ifdef CFG_STM32MP21
/*
 * Enable/disable debug with temporal isolation level for Cortex-A and Cortex-M
 * @ca_value: Value to write in BSEC debug control register for Cortex-A
 * @cm_value: Value to write in BSEC debug control register for Cortex-M
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_write_debug_ctrl(uint32_t ca_value, uint32_t cm_value);

/*
 * Parse permissions mask and prepare values to feed stm32_bsec_write_debug_conf
 * and stm32_bsec_write_debug_ctrl functions to enable/disable debug
 * @perm_mask: Permissions mask to enable/disable debug features
 * @dbg_en_val: (out) Value to write in BSEC debug enable register
 * @dbg_a_ctrl_val: (out) Value to write in BSEC debug control register for
 *			  Cortex-A
 * @dbg_m_ctrl_val: (out) Value to write in BSEC debug control register for
 *			  Cortex-M
 */
void stm32_bsec_parse_permissions(uint32_t perm_mask,
				  uint32_t *dbg_en_val,
				  uint32_t *dbg_a_ctrl_val,
				  uint32_t *dbg_m_ctrl_val);
#endif

/*
 * Write shadow-read lock
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_set_sr_lock(uint32_t otp_id);

/*
 * Read shadow-read lock
 * @otp_id: OTP number
 * @locked: (out) true if shadow-read is locked, false if not locked.
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_read_sr_lock(uint32_t otp_id, bool *locked);

/*
 * Write shadow-write lock
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_set_sw_lock(uint32_t otp_id);

/*
 * Read shadow-write lock
 * @otp_id: OTP number
 * @locked: (out) true if shadow-write is locked, false if not locked.
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_read_sw_lock(uint32_t otp_id, bool *locked);

/*
 * Write shadow-program lock
 * @otp_id: OTP number
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_set_sp_lock(uint32_t otp_id);

/*
 * Read shadow-program lock
 * @otp_id: OTP number
 * @locked: (out) true if shadow-program is locked, false if not locked.
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_read_sp_lock(uint32_t otp_id, bool *locked);

/*
 * Read permanent lock status
 * @otp_id: OTP number
 * @locked: (out) true if permanent lock is locked, false if not locked.
 * Return a TEE_Result compliant return value
 */
TEE_Result stm32_bsec_read_permanent_lock(uint32_t otp_id, bool *locked);

/*
 * Return true if OTP can be read, false otherwise
 * @otp_id: OTP number
 */
bool stm32_bsec_can_access_otp(uint32_t otp_id);

/*
 * Return true if non-secure world is allowed to read the target OTP
 * @otp_id: OTP number
 */
bool stm32_bsec_nsec_can_access_otp(uint32_t otp_id);

/*
 * Return true if host-self debug is enabled.
 */
bool stm32_bsec_self_hosted_debug_is_enabled(void);

/*
 * Program BSEC for dummy ADAC (open access to every AP).
 */
void stm32_bsec_mp21_dummy_adac(void);

/*
 * Program BSEC to open DBGMCU_APB_AP (AP0)
 */
void stm32_bsec_mp21_ap0_unlock(void);

/*
 * Find and get OTP location from its name.
 * @name: sub-node name to look up.
 * @otp_id: pointer to output OTP number or NULL.
 * @otp_bit_offset: pointer to output OTP bit offset in the NVMEM cell or NULL.
 * @otp_bit_len: pointer to output OTP length in bits or NULL.
 * Return a TEE_Result compliant status
 */
TEE_Result stm32_bsec_find_otp_in_nvmem_layout(const char *name,
					       uint32_t *otp_id,
					       uint8_t *otp_bit_offset,
					       size_t *otp_bit_len);

/*
 * Find and get OTP location from its phandle.
 * @phandle: node phandle to look up.
 * @otp_id: pointer to read OTP number or NULL.
 * @otp_bit_offset: pointer to read offset in OTP in bits or NULL.
 * @otp_bit_len: pointer to read OTP length in bits or NULL.
 * Return a TEE_Result compliant status
 */
TEE_Result stm32_bsec_find_otp_by_phandle(const uint32_t phandle,
					  uint32_t *otp_id,
					  uint8_t *otp_bit_offset,
					  size_t *otp_bit_len);

/*
 * Get BSEC global sec state.
 * @sec_state: Global BSEC current sec state
 * Return a TEE_Result compliant status
 */
TEE_Result stm32_bsec_get_state(enum stm32_bsec_sec_state *sec_state);

#endif /*__DRIVERS_STM32_BSEC_H*/
