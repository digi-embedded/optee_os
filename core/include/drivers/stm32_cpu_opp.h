/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2022-2024, STMicroelectronics
 */

#ifndef DRIVERS_STM32_CPU_OPP_H
#define DRIVERS_STM32_CPU_OPP_H

#include <tee_api_types.h>
#ifdef CFG_SCMI_SCPFW
#include <scmi_agent_configuration.h>
#endif /* CFG_SCMI_SCPFW */

struct regulator;
struct clk;

/* Return the number of CPU operating points */
size_t stm32_cpu_opp_count(void);

bool opp_voltage_is_supported(struct regulator *regul, uint32_t *volt_uv);

/* Get level value identifying CPU operating point @opp_index */
unsigned int stm32_cpu_opp_level(size_t opp_index);

#ifdef CFG_SCPFW_MOD_DVFS
/*
 * Initialize SCMI performance protocol with DVFS information
 * Returns a TEE_Result compliant value
 */
TEE_Result optee_scmi_server_cpu_dvfs(int perf_id,
				      struct scpfw_channel_config *channel_cfg);

TEE_Result optee_scmi_server_gpu_dvfs(int perf_id,
				      struct scpfw_channel_config *channel_cfg);

/* Request to switch to operating point related to @rate */
TEE_Result stm32_cpu_opp_set_rate(unsigned int rate);
TEE_Result stm32_gpu_opp_set_rate(unsigned int rate);

/* Get rate related to current operating point */
unsigned int stm32_cpu_opp_get_rate(void);
unsigned int stm32_gpu_opp_get_rate(void);

/* Request to operating point related to @level */
TEE_Result stm32_cpu_opp_get_rate_for_level(unsigned int level,
					    unsigned int *rate);
TEE_Result stm32_gpu_opp_get_rate_for_level(unsigned int level,
					    unsigned int *rate);

/* Request number of operating point */
TEE_Result stm32_gpu_opp_get_count(void);

/* Function used by CFG_SCPFW_MOD_DVFS to manage OPP on several domain */
#define OPP_ID_CPU		1
#define OPP_ID_GPU		2

static inline TEE_Result stm32_opp_set_rate(unsigned int opp_id,
					    unsigned int rate)
{
	if (opp_id == OPP_ID_CPU)
		return stm32_cpu_opp_set_rate(rate);
	if (opp_id == OPP_ID_GPU)
		return stm32_gpu_opp_set_rate(rate);
	return TEE_ERROR_NOT_SUPPORTED;
}

static inline unsigned int stm32_opp_get_rate(unsigned int opp_id)
{
	if (opp_id == OPP_ID_CPU)
		return stm32_cpu_opp_get_rate();
	if (opp_id == OPP_ID_GPU)
		return stm32_gpu_opp_get_rate();
	return 0;
}

static inline unsigned int stm32_opp_get_count(unsigned int opp_id)
{
	if (opp_id == OPP_ID_CPU)
		return stm32_cpu_opp_count();
	if (opp_id == OPP_ID_GPU)
		return stm32_gpu_opp_get_count();
	return 0;
}

static inline unsigned int stm32_opp_get_rate_for_level(unsigned int opp_id,
							unsigned int level,
							unsigned int *rate)
{
	if (opp_id == OPP_ID_CPU)
		return stm32_cpu_opp_get_rate_for_level(level, rate);
	if (opp_id == OPP_ID_GPU)
		return stm32_gpu_opp_get_rate_for_level(level, rate);
	return 0;
}

#else
/* Request to switch to CPU operating point related to @level */
TEE_Result stm32_cpu_opp_set_level(unsigned int level);

/* Get level related to current CPU operating point */
TEE_Result stm32_cpu_opp_read_level(unsigned int *level);

#endif /*CFG_SCPFW_MOD_DVFS*/
#endif /*DRIVERS_STM32_CPU_OPP_H*/
