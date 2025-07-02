/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2023, Digi-International
 */

#ifndef TA_STM32MP_CRYP_H
#define TA_STM32MP_CRYP_H

#define TA_STM32MP_CRYP_UUID { 0xc2fad363, 0x5d9f, 0x4fc4, \
		{ 0xa4, 0x17, 0x55, 0x58, 0x41, 0xe0, 0x57, 0x45 } }

#define TA_STM32MP_CRYP_VERSION	0x02

#define AES128_KEY_BIT_SIZE		128
#define AES128_KEY_BYTE_SIZE		(AES128_KEY_BIT_SIZE / 8)
#define AES192_KEY_BIT_SIZE		192
#define AES192_KEY_BYTE_SIZE		(AES192_KEY_BIT_SIZE / 8)
#define AES256_KEY_BIT_SIZE		256
#define AES256_KEY_BYTE_SIZE		(AES256_KEY_BIT_SIZE / 8)

#define TA_AES_CMD_PREPARE		0

#define TA_AES_ALGO_ECB			0
#define TA_AES_ALGO_CBC			1
#define TA_AES_ALGO_CTR			2

#define TA_AES_SIZE_128BIT		(128 / 8)
#define TA_AES_SIZE_256BIT		(256 / 8)

#define TA_AES_MODE_ENCODE		1
#define TA_AES_MODE_DECODE		0

/*
 * TA_AES_CMD_SET_KEY - Allocate resources for the AES ciphering
 * param[0] (memref) key data, size shall equal key length
 * param[1] unused
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_SET_KEY		1

/*
 * TA_AES_CMD_SET_IV - reset IV
 * param[0] (memref) initial vector, size shall equal block length
 * param[1] unused
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_SET_IV		2

/*
 * TA_AES_CMD_CIPHER - Cipher input buffer into output buffer
 * param[0] (memref) input buffer
 * param[1] (memref) output buffer (shall be bigger than input buffer)
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_CIPHER		3

struct aes_cipher {
	uint32_t algo;			/* AES flavour */
	uint32_t mode;			/* Encode or decode */
	uint32_t key_size;		/* AES key size in byte */
	TEE_OperationHandle op_handle;	/* AES ciphering operation */
	TEE_ObjectHandle key_handle;	/* transient object to load the key */
};

#endif /* TA_STM32MP_CRYP_H */
