// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2022, STMicroelectronics
 */

#include <assert.h>
#include <config.h>
#include <drivers/stm32_bsec.h>
#include <kernel/tee_common_otp.h>
#include <stdint.h>
#include <string_ext.h>
#include <types_ext.h>
#include <tee_api_defines.h>

#ifdef CFG_OTP_HW_TESTKEY
static const uint8_t otp_hw_test_key[] = {
	0xD8, 0x30, 0xA1, 0x88, 0x14, 0xE0, 0x2F, 0xE9,
	0x43, 0x6B, 0xB3, 0x8E, 0x03, 0x02, 0xC4, 0x8C
};

TEE_Result tee_otp_get_hw_unique_key(struct tee_hw_unique_key *hwkey)
{
	COMPILE_TIME_ASSERT(sizeof(otp_hw_test_key) >= sizeof(hwkey->data));

	memcpy(hwkey->data, otp_hw_test_key, sizeof(hwkey->data));

	return TEE_SUCCESS;
}

#else

TEE_Result tee_otp_get_hw_unique_key(struct tee_hw_unique_key *hwkey)
{
	uint32_t otp_id = 0;
	size_t otp_bit_len = 0;
	uint32_t tmp = 0;
	bool provisioned = true;
	size_t i = 0;
	TEE_Result res = TEE_ERROR_GENERIC;

	if (!hwkey || !hwkey->data)
		return TEE_ERROR_BAD_PARAMETERS;

	res = stm32_bsec_find_otp_in_nvmem_layout("huk_otp", &otp_id,
						  &otp_bit_len);
	if (res)
		goto out;

	if (otp_bit_len != (HW_UNIQUE_KEY_LENGTH * CHAR_BIT)) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out;
	}

	for (i = 0; i < HW_UNIQUE_KEY_LENGTH / sizeof(uint32_t); i++) {
		res = stm32_bsec_read_permanent_lock(otp_id + i, &provisioned);
		if (res)
			goto out;

		if (!provisioned) {
			res = TEE_ERROR_SECURITY;
			goto out;
		}

		res = stm32_bsec_shadow_read_otp(&tmp, otp_id + i);
		if (res)
			goto out;

		memcpy(hwkey->data + i * sizeof(uint32_t), &tmp,
		       sizeof(uint32_t));
	}

out:
	if (res) {
		EMSG("HUK not found");
		memzero_explicit(hwkey->data, HW_UNIQUE_KEY_LENGTH);
	}

	return res;
}
#endif
