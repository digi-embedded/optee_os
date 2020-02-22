/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2018-2020, Linaro Limited
 */

#ifndef PKCS11_HELPERS_H
#define PKCS11_HELPERS_H

#include <stdint.h>
#include <stddef.h>

/*
 * TEE invocation parameter#0 is an in/out buffer of at least 32bit
 * to store the TA PKCS#11 compliant return value.
 */
#define TEE_PARAM0_SIZE_MIN		sizeof(uint32_t)

#if CFG_TEE_TA_LOG_LEVEL > 0
/* Id-to-string conversions only for trace support */
const char *id2str_ta_cmd(uint32_t id);
const char *id2str_rc(uint32_t id);
#endif
#endif /*PKCS11_HELPERS_H*/