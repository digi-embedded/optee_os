// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2017-2021, STMicroelectronics
 */

#include <assert.h>
#include <config.h>
#include <drivers/stm32_bsec.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <kernel/boot.h>
#include <kernel/pm.h>
#include <kernel/spinlock.h>
#include <libfdt.h>
#include <limits.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <stm32_util.h>
#include <string.h>
#include <tee_api_defines.h>
#include <types_ext.h>
#include <util.h>

#ifdef CFG_STM32MP13
#define DT_BSEC_COMPAT "st,stm32mp13-bsec"
#endif
#ifdef CFG_STM32MP15
#define DT_BSEC_COMPAT "st,stm32mp15-bsec"
#endif

#define BSEC_OTP_MASK			GENMASK_32(4, 0)
#define BSEC_OTP_BANK_SHIFT		U(5)

/* Permanent lock bitmasks */
#define DATA_LOWER_OTP_PERLOCK_BIT	U(3)
#define DATA_UPPER_OTP_PERLOCK_BIT	U(1)

/* BSEC register offset */
#define BSEC_OTP_CONF_OFF		U(0x000)
#define BSEC_OTP_CTRL_OFF		U(0x004)
#define BSEC_OTP_WRDATA_OFF		U(0x008)
#define BSEC_OTP_STATUS_OFF		U(0x00C)
#define BSEC_OTP_LOCK_OFF		U(0x010)
#define BSEC_DEN_OFF			U(0x014)
#define BSEC_FEN_OFF			U(0x018)
#define BSEC_DISTURBED_OFF		U(0x01C)
#define BSEC_DISTURBED1_OFF		U(0x020)
#define BSEC_DISTURBED2_OFF		U(0x024)
#define BSEC_ERROR_OFF			U(0x034)
#define BSEC_ERROR1_OFF			U(0x038)
#define BSEC_ERROR2_OFF			U(0x03C)
#define BSEC_WRLOCK_OFF			U(0x04C)
#define BSEC_WRLOCK1_OFF		U(0x050)
#define BSEC_WRLOCK2_OFF		U(0x054)
#define BSEC_SPLOCK_OFF			U(0x064)
#define BSEC_SPLOCK1_OFF		U(0x068)
#define BSEC_SPLOCK2_OFF		U(0x06C)
#define BSEC_SWLOCK_OFF			U(0x07C)
#define BSEC_SWLOCK1_OFF		U(0x080)
#define BSEC_SWLOCK2_OFF		U(0x084)
#define BSEC_SRLOCK_OFF			U(0x094)
#define BSEC_SRLOCK1_OFF		U(0x098)
#define BSEC_SRLOCK2_OFF		U(0x09C)
#define BSEC_JTAG_IN_OFF		U(0x0AC)
#define BSEC_JTAG_OUT_OFF		U(0x0B0)
#define BSEC_SCRATCH_OFF		U(0x0B4)
#define BSEC_OTP_DATA_OFF		U(0x200)
#define BSEC_IPHW_CFG_OFF		U(0xFF0)
#define BSEC_IPVR_OFF			U(0xFF4)
#define BSEC_IP_ID_OFF			U(0xFF8)
#define BSEC_IP_MAGIC_ID_OFF		U(0xFFC)

/* BSEC_CONFIGURATION Register */
#define BSEC_CONF_POWER_UP_MASK		BIT(0)
#define BSEC_CONF_POWER_UP_SHIFT	U(0)
#define BSEC_CONF_FRQ_MASK		GENMASK_32(2, 1)
#define BSEC_CONF_FRQ_SHIFT		U(1)
#define BSEC_CONF_PRG_WIDTH_MASK	GENMASK_32(6, 3)
#define BSEC_CONF_PRG_WIDTH_SHIFT	U(3)
#define BSEC_CONF_TREAD_MASK		GENMASK_32(8, 7)
#define BSEC_CONF_TREAD_SHIFT		U(7)

/* BSEC_CONTROL Register */
#define BSEC_READ			U(0x000)
#define BSEC_WRITE			U(0x100)
#define BSEC_LOCK			U(0x200)

/* BSEC_STATUS Register */
#define BSEC_MODE_STATUS_MASK		GENMASK_32(2, 0)
#define BSEC_MODE_SECURED		BIT(0)
#define BSEC_MODE_INVALID		BIT(2)
#define BSEC_MODE_BUSY_MASK		BIT(3)
#define BSEC_MODE_PROGFAIL_MASK		BIT(4)
#define BSEC_MODE_PWR_MASK		BIT(5)
#define BSEC_MODE_BIST1_LOCK_MASK	BIT(6)
#define BSEC_MODE_BIST2_LOCK_MASK	BIT(7)

/* BSEC_DEBUG */
#define BSEC_DEN_ALL_MSK		GENMASK_32(10, 0)

/*
 * OTP Lock services definition
 * Value must corresponding to the bit position in the register
 */
#define BSEC_LOCK_UPPER_OTP		U(0x00)
#define BSEC_LOCK_DEBUG			U(0x02)
#define BSEC_LOCK_PROGRAM		U(0x04)

/* Timeout when polling on status */
#define BSEC_TIMEOUT_US			U(10000)

struct bsec_dev {
	struct io_pa_va base;
	unsigned int upper_base;
	unsigned int max_id;
	uint32_t *nsec_access;
};

/* Only 1 instance of BSEC is expected per platform */
static struct bsec_dev bsec_dev;

/* BSEC access protection */
static unsigned int lock = SPINLOCK_UNLOCK;

static uint32_t bsec_lock(void)
{
	return may_spin_lock(&lock);
}

static void bsec_unlock(uint32_t exceptions)
{
	may_spin_unlock(&lock, exceptions);
}

static uint32_t otp_max_id(void)
{
	return bsec_dev.max_id;
}

static uint32_t otp_upper_base(void)
{
	return bsec_dev.upper_base;
}

static uint32_t otp_bank_offset(uint32_t otp_id)
{
	assert(otp_id <= otp_max_id());

	return ((otp_id & ~BSEC_OTP_MASK) >> BSEC_OTP_BANK_SHIFT) *
		sizeof(uint32_t);
}

static vaddr_t bsec_base(void)
{
	return io_pa_or_va_secure(&bsec_dev.base, BSEC_IP_MAGIC_ID_OFF + 1);
}

static uint32_t bsec_status(void)
{
	return io_read32(bsec_base() + BSEC_OTP_STATUS_OFF);
}

static bool is_invalid_mode(void)
{
	return (bsec_status() & BSEC_MODE_INVALID) != 0;
}

static bool is_secured_mode(void)
{
	return (bsec_status() & BSEC_MODE_SECURED) != 0;
}

static bool is_closed_mode(void)
{
	uint32_t otp_cfg = 0;
	uint32_t close_mode = 0;
	TEE_Result res = TEE_ERROR_GENERIC;
	const uint32_t mask = CFG0_CLOSED_MASK;

	res = stm32_bsec_find_otp_in_nvmem_layout("cfg0_otp", &otp_cfg, NULL);
	if (res)
		panic("CFG0 OTP not found");

	if (stm32_bsec_read_otp(&close_mode, otp_cfg))
		panic("Unable to read OTP");

	return (close_mode & mask) == mask;
}

/*
 * Check that BSEC interface does not report an error
 * @otp_id : OTP number
 * @check_disturbed: check only error (false) or all sources (true)
 * Return a TEE_Result compliant value
 */
static TEE_Result check_no_error(uint32_t otp_id, bool check_disturbed)
{
	uint32_t bit = BIT(otp_id & BSEC_OTP_MASK);
	uint32_t bank = otp_bank_offset(otp_id);

	if (io_read32(bsec_base() + BSEC_ERROR_OFF + bank) & bit)
		return TEE_ERROR_GENERIC;

	if (check_disturbed &&
	    io_read32(bsec_base() + BSEC_DISTURBED_OFF + bank) & bit)
		return TEE_ERROR_GENERIC;

	return TEE_SUCCESS;
}

static TEE_Result power_up_safmem(void)
{
	uint64_t timeout_ref = timeout_init_us(BSEC_TIMEOUT_US);

	io_mask32(bsec_base() + BSEC_OTP_CONF_OFF, BSEC_CONF_POWER_UP_MASK,
		  BSEC_CONF_POWER_UP_MASK);

	/*
	 * If a timeout is detected, test the condition again to consider
	 * cases where timeout is due to the executing TEE thread rescheduling.
	 */
	while (!timeout_elapsed(timeout_ref))
		if (bsec_status() & BSEC_MODE_PWR_MASK)
			break;

	if (bsec_status() & BSEC_MODE_PWR_MASK)
		return TEE_SUCCESS;

	return TEE_ERROR_GENERIC;
}

static TEE_Result power_down_safmem(void)
{
	uint64_t timeout_ref = timeout_init_us(BSEC_TIMEOUT_US);

	io_mask32(bsec_base() + BSEC_OTP_CONF_OFF, 0, BSEC_CONF_POWER_UP_MASK);

	/*
	 * If a timeout is detected, test the condition again to consider
	 * cases where timeout is due to the executing TEE thread rescheduling.
	 */
	while (!timeout_elapsed(timeout_ref))
		if (!(bsec_status() & BSEC_MODE_PWR_MASK))
			break;

	if (!(bsec_status() & BSEC_MODE_PWR_MASK))
		return TEE_SUCCESS;

	return TEE_ERROR_GENERIC;
}

TEE_Result stm32_bsec_shadow_register(uint32_t otp_id)
{
	TEE_Result result = 0;
	uint32_t exceptions = 0;
	uint64_t timeout_ref = 0;
	bool locked = false;

	/* Check if shadowing of OTP is locked, informative only */
	result = stm32_bsec_read_sr_lock(otp_id, &locked);
	if (result)
		return result;

	if (locked)
		DMSG("BSEC shadow warning: OTP locked");

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	result = power_up_safmem();
	if (result)
		goto out;

	io_write32(bsec_base() + BSEC_OTP_CTRL_OFF, otp_id | BSEC_READ);

	timeout_ref = timeout_init_us(BSEC_TIMEOUT_US);
	while (!timeout_elapsed(timeout_ref))
		if (!(bsec_status() & BSEC_MODE_BUSY_MASK))
			break;

	if (bsec_status() & BSEC_MODE_BUSY_MASK)
		result = TEE_ERROR_BUSY;
	else
		result = check_no_error(otp_id, true /* check-disturbed */);

	power_down_safmem();

out:
	bsec_unlock(exceptions);

	return result;
}

TEE_Result stm32_bsec_read_otp(uint32_t *value, uint32_t otp_id)
{
	if (otp_id > otp_max_id())
		return TEE_ERROR_BAD_PARAMETERS;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	*value = io_read32(bsec_base() + BSEC_OTP_DATA_OFF +
			   (otp_id * sizeof(uint32_t)));

	return TEE_SUCCESS;
}

TEE_Result stm32_bsec_shadow_read_otp(uint32_t *otp_value, uint32_t otp_id)
{
	TEE_Result result = 0;

	result = stm32_bsec_shadow_register(otp_id);
	if (result) {
		EMSG("BSEC %"PRIu32" Shadowing Error %#"PRIx32, otp_id, result);
		return result;
	}

	result = stm32_bsec_read_otp(otp_value, otp_id);
	if (result)
		EMSG("BSEC %"PRIu32" Read Error %#"PRIx32, otp_id, result);

	return result;
}

TEE_Result stm32_bsec_write_otp(uint32_t value, uint32_t otp_id)
{
	TEE_Result result = 0;
	uint32_t exceptions = 0;
	vaddr_t otp_data_base = bsec_base() + BSEC_OTP_DATA_OFF;
	bool locked = false;

	/* Check if write of OTP is locked, informative only */
	result = stm32_bsec_read_sw_lock(otp_id, &locked);
	if (result)
		return result;

	if (locked)
		DMSG("BSEC write warning: OTP locked");

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	io_write32(otp_data_base + (otp_id * sizeof(uint32_t)), value);

	bsec_unlock(exceptions);

	return TEE_SUCCESS;
}

#ifdef CFG_STM32_BSEC_WRITE
TEE_Result stm32_bsec_program_otp(uint32_t value, uint32_t otp_id)
{
	TEE_Result result = 0;
	uint32_t exceptions = 0;
	uint64_t timeout_ref = 0;
	bool locked = false;

	/* Check if shadowing of OTP is locked, informative only */
	result = stm32_bsec_read_sp_lock(otp_id, &locked);
	if (result)
		return result;

	if (locked)
		DMSG("BSEC program warning: OTP locked");

	if (io_read32(bsec_base() + BSEC_OTP_LOCK_OFF) & BIT(BSEC_LOCK_PROGRAM))
		DMSG("BSEC program warning: GPLOCK activated");

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	result = power_up_safmem();
	if (result)
		goto out;

	io_write32(bsec_base() + BSEC_OTP_WRDATA_OFF, value);
	io_write32(bsec_base() + BSEC_OTP_CTRL_OFF, otp_id | BSEC_WRITE);

	timeout_ref = timeout_init_us(BSEC_TIMEOUT_US);
	while (!timeout_elapsed(timeout_ref))
		if (!(bsec_status() & BSEC_MODE_BUSY_MASK))
			break;

	if (bsec_status() & BSEC_MODE_BUSY_MASK)
		result = TEE_ERROR_BUSY;
	else if (bsec_status() & BSEC_MODE_PROGFAIL_MASK)
		result = TEE_ERROR_BAD_PARAMETERS;
	else
		result = check_no_error(otp_id, true /* check-disturbed */);

	power_down_safmem();

out:
	bsec_unlock(exceptions);

	return result;
}
#endif /*CFG_STM32_BSEC_WRITE*/

TEE_Result stm32_bsec_permanent_lock_otp(uint32_t otp_id)
{
	TEE_Result result = 0;
	uint32_t data = 0;
	uint32_t addr = 0;
	uint32_t exceptions = 0;
	vaddr_t base = bsec_base();
	uint64_t timeout_ref = 0;
	uint32_t upper_base = otp_upper_base();

	if (otp_id > otp_max_id())
		return TEE_ERROR_BAD_PARAMETERS;

	/*
	 * 2 bits word for lower OTPs, 1 bit per word for upper OTPs
	 * and only 16 bits used in WRDATA: lower=8 OTPs by word, upper=16
	 */
	if (otp_id < upper_base) {
		addr = otp_id / 8U;
		data = DATA_LOWER_OTP_PERLOCK_BIT << ((otp_id * 2U) & 0xF);
	} else {
		addr = upper_base / 8U + (otp_id - upper_base) / 16U;
		data = DATA_UPPER_OTP_PERLOCK_BIT << (otp_id & 0xF);
	}

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	result = power_up_safmem();
	if (result)
		goto out;

	io_write32(base + BSEC_OTP_WRDATA_OFF, data);
	io_write32(base + BSEC_OTP_CTRL_OFF, addr | BSEC_WRITE | BSEC_LOCK);

	timeout_ref = timeout_init_us(BSEC_TIMEOUT_US);
	while (!timeout_elapsed(timeout_ref))
		if (!(bsec_status() & BSEC_MODE_BUSY_MASK))
			break;

	if (bsec_status() & BSEC_MODE_BUSY_MASK)
		result = TEE_ERROR_BUSY;
	else if (bsec_status() & BSEC_MODE_PROGFAIL_MASK)
		result = TEE_ERROR_BAD_PARAMETERS;
	else
		result = check_no_error(otp_id, false /* not-disturbed */);

#ifdef CFG_STM32MP13
	io_write32(base + BSEC_OTP_CTRL_OFF, addr | BSEC_READ | BSEC_LOCK);
#endif

	power_down_safmem();

out:
	bsec_unlock(exceptions);

	return result;
}

TEE_Result stm32_bsec_write_debug_conf(uint32_t value)
{
	TEE_Result result = TEE_ERROR_GENERIC;
	uint32_t masked_val = value & BSEC_DEN_ALL_MSK;
	uint32_t exceptions = 0;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	io_write32(bsec_base() + BSEC_DEN_OFF, value);

	if ((io_read32(bsec_base() + BSEC_DEN_OFF) ^ masked_val) == 0U)
		result = TEE_SUCCESS;

	bsec_unlock(exceptions);

	return result;
}

uint32_t stm32_bsec_read_debug_conf(void)
{
	return io_read32(bsec_base() + BSEC_DEN_OFF);
}

static TEE_Result set_bsec_lock(uint32_t otp_id, size_t lock_offset)
{
	uint32_t bank = otp_bank_offset(otp_id);
	uint32_t otp_mask = BIT(otp_id & BSEC_OTP_MASK);
	vaddr_t lock_addr = bsec_base() + bank + lock_offset;
	uint32_t exceptions = 0;

	if (otp_id > STM32MP1_OTP_MAX_ID)
		return TEE_ERROR_BAD_PARAMETERS;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	exceptions = bsec_lock();

	io_write32(lock_addr, otp_mask);

	bsec_unlock(exceptions);

	return TEE_SUCCESS;
}

TEE_Result stm32_bsec_set_sr_lock(uint32_t otp_id)
{
	return set_bsec_lock(otp_id, BSEC_SRLOCK_OFF);
}

TEE_Result stm32_bsec_set_sw_lock(uint32_t otp_id)
{
	return set_bsec_lock(otp_id, BSEC_SWLOCK_OFF);
}

TEE_Result stm32_bsec_set_sp_lock(uint32_t otp_id)
{
	return set_bsec_lock(otp_id, BSEC_SPLOCK_OFF);
}

static TEE_Result read_bsec_lock(uint32_t otp_id, bool *locked,
				 size_t lock_offset)
{
	uint32_t bank = otp_bank_offset(otp_id);
	uint32_t otp_mask = BIT(otp_id & BSEC_OTP_MASK);
	vaddr_t lock_addr = bsec_base() + bank + lock_offset;

	if (otp_id > STM32MP1_OTP_MAX_ID)
		return TEE_ERROR_BAD_PARAMETERS;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	*locked = (io_read32(lock_addr) & otp_mask) != 0;

	return TEE_SUCCESS;
}

TEE_Result stm32_bsec_read_sr_lock(uint32_t otp_id, bool *locked)
{
	return read_bsec_lock(otp_id, locked, BSEC_SRLOCK_OFF);
}

TEE_Result stm32_bsec_read_sw_lock(uint32_t otp_id, bool *locked)
{
	return read_bsec_lock(otp_id, locked, BSEC_SWLOCK_OFF);
}

TEE_Result stm32_bsec_read_sp_lock(uint32_t otp_id, bool *locked)
{
	return read_bsec_lock(otp_id, locked, BSEC_SPLOCK_OFF);
}

TEE_Result stm32_bsec_read_permanent_lock(uint32_t otp_id, bool *locked)
{
	return read_bsec_lock(otp_id, locked, BSEC_WRLOCK_OFF);
}

TEE_Result stm32_bsec_otp_lock(uint32_t service)
{
	vaddr_t addr = bsec_base() + BSEC_OTP_LOCK_OFF;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	switch (service) {
	case BSEC_LOCK_UPPER_OTP:
		io_write32(addr, BIT(BSEC_LOCK_UPPER_OTP));
		break;
	case BSEC_LOCK_DEBUG:
		io_write32(addr, BIT(BSEC_LOCK_DEBUG));
		break;
	case BSEC_LOCK_PROGRAM:
		io_write32(addr, BIT(BSEC_LOCK_PROGRAM));
		break;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}

	return TEE_SUCCESS;
}

static size_t nsec_access_array_size(void)
{
	size_t upper_count = otp_max_id() - otp_upper_base() + 1;

	return ROUNDUP_DIV(upper_count, BSEC_BITS_PER_WORD);
}

static bool nsec_access_granted(unsigned int index)
{
	uint32_t *array = bsec_dev.nsec_access;

	return array &&
	       (index / BSEC_BITS_PER_WORD) < nsec_access_array_size() &&
	       array[index / BSEC_BITS_PER_WORD] &
	       BIT(index % BSEC_BITS_PER_WORD);
}

bool stm32_bsec_can_access_otp(uint32_t otp_id)
{
	if (otp_id > otp_max_id())
		return TEE_ERROR_BAD_PARAMETERS;

	if (is_invalid_mode())
		return TEE_ERROR_SECURITY;

	return TEE_SUCCESS;
}

bool stm32_bsec_nsec_can_access_otp(uint32_t otp_id)
{
	return otp_id < otp_upper_base() ||
	       nsec_access_granted(otp_id - otp_upper_base());
}

struct nvmem_layout {
	char *name;
	uint32_t otp_id;
	size_t bit_len;
};

static struct nvmem_layout *nvmem_layout;
static size_t nvmem_layout_count;

TEE_Result stm32_bsec_find_otp_in_nvmem_layout(const char *name,
					       uint32_t *otp_id,
					       size_t *otp_bit_len)
{
	size_t i = 0;

	if (!name)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < nvmem_layout_count; i++) {
		if (!nvmem_layout[i].name || strcmp(name, nvmem_layout[i].name))
			continue;

		if (otp_id)
			*otp_id = nvmem_layout[i].otp_id;

		if (otp_bit_len)
			*otp_bit_len = nvmem_layout[i].bit_len;

		DMSG("nvmem %s = %zu: %"PRId32" %zu", name, i,
		     nvmem_layout[i].otp_id, nvmem_layout[i].bit_len);

		return TEE_SUCCESS;
	}

	DMSG("nvmem %s failed", name);

	return TEE_ERROR_ITEM_NOT_FOUND;
}

TEE_Result stm32_bsec_get_state(uint32_t *state)
{
	uint32_t otp_enc_id = 0;
	size_t otp_bit_len = 0;
	TEE_Result res = TEE_SUCCESS;

	if (!state)
		return TEE_ERROR_BAD_PARAMETERS;

	if (is_invalid_mode() || !is_secured_mode()) {
		*state = BSEC_STATE_INVALID;
	} else {
		if (is_closed_mode())
			*state = BSEC_STATE_SEC_CLOSED;
		else
			*state = BSEC_STATE_SEC_OPEN;
	}

	if (!IS_ENABLED(CFG_STM32MP13))
		return TEE_SUCCESS;

	res = stm32_bsec_find_otp_in_nvmem_layout("oem_enc_key",
						  &otp_enc_id, &otp_bit_len);
	if (!res && otp_bit_len) {
		unsigned int start = otp_enc_id / BSEC_BITS_PER_WORD;
		size_t otp_nb = ROUNDUP_DIV(otp_bit_len, BSEC_BITS_PER_WORD);
		unsigned int idx = 0;

		for (idx = start; idx < start + otp_nb; idx++) {
			bool locked = false;

			res = stm32_bsec_read_sp_lock(idx, &locked);
			if (res || !locked)
				return TEE_SUCCESS;
		}

		*state |= BSEC_HARDWARE_KEY;
	}

	return TEE_SUCCESS;
}

#ifdef CFG_EMBED_DTB
static void enable_nsec_access(unsigned int otp_id)
{
	unsigned int idx = (otp_id - otp_upper_base()) / BSEC_BITS_PER_WORD;
	TEE_Result result = 0;

	if (otp_id < otp_upper_base())
		return;

	if (otp_id > otp_max_id())
		panic();

	result = stm32_bsec_shadow_register(otp_id);
	if (result) {
		EMSG("BSEC %"PRIu32" Shadowing Error %#"PRIx32, otp_id, result);
		return;
	}

	bsec_dev.nsec_access[idx] |= BIT(otp_id % BSEC_BITS_PER_WORD);
}

static void bsec_dt_otp_nsec_access(void *fdt, int bsec_node)
{
	int bsec_subnode = 0;

	bsec_dev.nsec_access = calloc(nsec_access_array_size(),
				      sizeof(*bsec_dev.nsec_access));
	if (!bsec_dev.nsec_access)
		panic();

	fdt_for_each_subnode(bsec_subnode, fdt, bsec_node) {
		const fdt32_t *cuint = NULL;
		unsigned int otp_id = 0;
		unsigned int i, j = 0;
		size_t size = 0;
		uint32_t offset = 0;
		uint32_t length = 0;

		cuint = fdt_getprop(fdt, bsec_subnode, "reg", NULL);
		assert(cuint);

		offset = fdt32_to_cpu(*cuint);
		cuint++;
		length = fdt32_to_cpu(*cuint);

		otp_id = offset / sizeof(uint32_t);

		if (otp_id < STM32MP1_UPPER_OTP_START) {
			unsigned int otp_end = ROUNDUP(offset + length,
						       sizeof(uint32_t)) /
					       sizeof(uint32_t);

			if (otp_end > STM32MP1_UPPER_OTP_START) {
				/*
				 * OTP crosses Lower/Upper boundary, consider
				 * only the upper part.
				 */
				otp_id = STM32MP1_UPPER_OTP_START;
				length -= (STM32MP1_UPPER_OTP_START *
					   sizeof(uint32_t)) - offset;
				offset = STM32MP1_UPPER_OTP_START *
					 sizeof(uint32_t);

				DMSG("OTP crosses Lower/Upper boundary");
			} else {
				continue;
			}
		}

		/* Handle different kinds of non-secure accesses */
		if (fdt_getprop(fdt, bsec_subnode,
				"st,non-secure-otp-provisioning", NULL)) {
			bool locked, locked_next = false;
			/* Check if write of OTP is locked */
			if (stm32_bsec_read_permanent_lock(otp_id, &locked))
				panic("BSEC: Couldn't read permanent lock at init");

			/*
			 * Check if fuses of the subnode
			 * have the same lock status
			 */
			for (j = 1; j < (length / sizeof(uint32_t)); j++) {
				stm32_bsec_read_permanent_lock(otp_id + j,
							       &locked_next);
				if (locked != locked_next) {
					EMSG("Inconsistent status OTP id %u",
					     otp_id + j);
					locked = true;
					continue;
				}
			}

			if (locked) {
				DMSG("BSEC: OTP locked");
				continue;
			}

		} else if (!fdt_getprop(fdt, bsec_subnode, "st,non-secure-otp",
				   NULL)) {
			continue;
		}

		if ((offset % sizeof(uint32_t)) || (length % sizeof(uint32_t)))
			panic("Unaligned non-secure OTP");

		size = length / sizeof(uint32_t);

		if (otp_id + size > OTP_MAX_SIZE)
			panic("OTP range oversized");

		for (i = otp_id; i < otp_id + size; i++)
			enable_nsec_access(i);
	}
}

static void save_dt_nvmem_layout(void *fdt, int bsec_node)
{
	int cell_max = 0;
	int cell_cnt = 0;
	int node = 0;

	fdt_for_each_subnode(node, fdt, bsec_node)
		cell_max++;
	if (!cell_max)
		return;

	nvmem_layout = calloc(cell_max, sizeof(*nvmem_layout));
	if (!nvmem_layout)
		panic();

	fdt_for_each_subnode(node, fdt, bsec_node) {
		const fdt32_t *cuint = NULL;
		const char *string = NULL;
		const char *s = NULL;
		int len = 0;
		struct nvmem_layout *layout_cell = &nvmem_layout[cell_cnt];

		string = fdt_get_name(fdt, node, &len);
		if (!string || !len)
			continue;

		cuint = fdt_getprop(fdt, node, "reg", &len);
		if (!cuint || (len != (2 * (int)sizeof(uint32_t))))  {
			IMSG("Malformed nvmem %s: ignored", string);
			continue;
		}

		if (fdt32_to_cpu(*cuint) % sizeof(uint32_t)) {
			DMSG("Misaligned nvmem %s: ignored", string);
			continue;
		}
		layout_cell->otp_id = fdt32_to_cpu(*cuint) / sizeof(uint32_t);
		layout_cell->bit_len = fdt32_to_cpu(*(cuint + 1)) * CHAR_BIT;

		s = strchr(string, '@');
		if (s)
			len = s - string;

		layout_cell->name = calloc(1, len + 1);
		if (!layout_cell->name)
			panic();

		memcpy(layout_cell->name, string, len);
		cell_cnt++;
		DMSG("nvmem[%d] = %s %"PRId32" %zu", cell_cnt,
		     layout_cell->name, layout_cell->otp_id, layout_cell->bit_len);
	}

	if (cell_cnt != cell_max) {
		nvmem_layout = realloc(nvmem_layout, cell_cnt * sizeof(*nvmem_layout));
		if (!nvmem_layout)
			panic();
	}

	nvmem_layout_count = cell_cnt;
}

static void initialize_bsec_from_dt(void)
{
	void *fdt = NULL;
	int node = 0;
	struct dt_node_info bsec_info = { };

	fdt = get_embedded_dt();
	node = fdt_node_offset_by_compatible(fdt, 0, DT_BSEC_COMPAT);
	if (node < 0)
		panic();

	_fdt_fill_device_info(fdt, &bsec_info, node);

	if (bsec_info.reg != bsec_dev.base.pa ||
	    !(bsec_info.status & DT_STATUS_OK_SEC))
		panic();

	bsec_dt_otp_nsec_access(fdt, node);

	save_dt_nvmem_layout(fdt, node);
}
#else
static void initialize_bsec_from_dt(void)
{
}
#endif /*CFG_EMBED_DTB*/

static TEE_Result bsec_pm(enum pm_op op, uint32_t pm_hint __unused,
			  const struct pm_callback_handle *hdl __unused)
{
	static uint32_t debug_conf;

	if (op == PM_OP_SUSPEND)
		debug_conf = stm32_bsec_read_debug_conf();

	if (op == PM_OP_RESUME)
		stm32_bsec_write_debug_conf(debug_conf);

	return TEE_SUCCESS;
}
DECLARE_KEEP_PAGER(bsec_pm);

static TEE_Result initialize_bsec(void)
{
	struct stm32_bsec_static_cfg cfg = { };

	stm32mp_get_bsec_static_cfg(&cfg);

	bsec_dev.base.pa = cfg.base;
	bsec_dev.upper_base = cfg.upper_start;
	bsec_dev.max_id = cfg.max_id;

	if (is_invalid_mode())
		panic();

	if (IS_ENABLED(CFG_EMBED_DTB))
		initialize_bsec_from_dt();

	register_pm_core_service_cb(bsec_pm, NULL, "bsec-service");

	return TEE_SUCCESS;
}

early_init(initialize_bsec);
