// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2022-2024, STMicroelectronics
 */

#include <config.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <drivers/regulator.h>
#include <drivers/stm32_cpu_opp.h>
#ifdef CFG_STM32MP13
#include <drivers/stm32mp1_pwr.h>
#endif
#include <initcall.h>
#include <io.h>
#include <keep.h>
#include <kernel/boot.h>
#include <kernel/dt.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <libfdt.h>
#ifdef CFG_SCMI_SCPFW
#include <scmi_agent_configuration.h>
#endif
#include <stm32mp_pm.h>
#include <stm32_util.h>
#include <trace.h>
#include <util.h>

/*
 * struct cpu_dvfs - CPU DVFS registered operating points
 * @freq_khz: CPU frequency in kilohertz (kHz)
 * @volt_uv: CPU voltage level in microvolts (uV)
 */
struct cpu_dvfs {
	unsigned int freq_khz;
	int volt_uv;
};

/*
 * struct cpu_opp - CPU operating point
 *
 * @current_opp: Index of current CPU operating point in @dvfs array
 * @default_opp: Index of OPP to set at boot or during suspend in @dvfs array
 * @opp_count: Number of cells of @dvfs
 * @clock: CPU clock handle
 * @regul: CPU regulator supply handle
 * @dvfs: Arrays of the supported CPU operating points
 * @scp_clock: Clock instance exposed to scp-firmware SCMI DVFS
 * @scp_regulator: Regulator instance exposed to scp-firmware SCMI DVFS
 * @scp_levels_desc: Description of voltage levels for scp-firmware SCMI DVFS
 * @scp_cpu_opp_levels_uv: Array of voltage levels described by @scp_levels_desc
 */
struct cpu_opp {
	unsigned int current_opp;
	unsigned int default_opp;
	unsigned int opp_count;
	struct clk *clock;
	struct regulator *regul;
	struct cpu_dvfs *dvfs;
#ifdef CFG_SCPFW_MOD_DVFS
	struct clk scp_clock;
	struct regulator scp_regulator;
	struct regulator_voltages_desc scp_levels_desc;
	int *scp_cpu_opp_levels_uv;
#endif
};

static struct cpu_opp cpu_opp;

#ifndef CFG_SCPFW_MOD_DVFS
/* Mutex for protecting CPU OPP changes */
static struct mutex cpu_opp_mu = MUTEX_INITIALIZER;
#endif

#define MPU_RAM_LOW_SPEED_THRESHOLD 1320000

#ifndef CFG_SCPFW_MOD_DVFS
size_t stm32_cpu_opp_count(void)
{
	return cpu_opp.opp_count;
}
#endif

unsigned int stm32_cpu_opp_level(size_t opp_index)
{
	assert(opp_index < cpu_opp.opp_count);

	return cpu_opp.dvfs[opp_index].freq_khz;
}

static TEE_Result _set_opp_clk_rate(unsigned int opp)
{
#ifdef CFG_STM32MP15
	return stm32mp1_set_opp_khz(cpu_opp.dvfs[opp].freq_khz);
#else
	return clk_set_rate(cpu_opp.clock, cpu_opp.dvfs[opp].freq_khz * 1000UL);
#endif
}

static TEE_Result opp_set_voltage(struct regulator *regul, int volt_uv)
{
	return regulator_set_voltage(regul, volt_uv);
}

/*
 * This function returns true if the given OPP voltage can be managed.
 * If the exact voltage value is not supported by the regulator,
 * the function may adjust the input parameter volt_uv to a higher
 * supported value and still return true.
 */
bool opp_voltage_is_supported(struct regulator *regul, uint32_t *volt_uv)
{
	const int target_volt_uv = *volt_uv;
	int new_volt_uv = 0;
	int min_uv = 0;
	int max_uv = 0;
	unsigned int i = 0;
	struct regulator_voltages_desc *desc = NULL;
	const int *levels = NULL;
	TEE_Result res = TEE_ERROR_GENERIC;

	res = regulator_supported_voltages(regul, &desc, &levels);
	if (res) {
		regulator_get_range(regul, &min_uv, &max_uv);
		if (target_volt_uv > max_uv)
			return false;
		if (target_volt_uv < min_uv)
			*volt_uv = min_uv;
		return true;
	}

	if (desc->type == VOLTAGE_TYPE_FULL_LIST) {
		for (i = 0 ; i < desc->num_levels; i++) {
			if (levels[i] >= target_volt_uv) {
				new_volt_uv = levels[i];
				break;
			}
		}
		if (new_volt_uv == 0)
			return false;

	} else if (desc->type == VOLTAGE_TYPE_INCREMENT) {
		new_volt_uv = levels[0]; /* min */
		while (new_volt_uv < target_volt_uv)
			new_volt_uv += levels[2]; /* increment */
		if (new_volt_uv > levels[1]) /* max */
			return false;
	} else {
		return false;
	}

	*volt_uv = new_volt_uv;

	return true;
}

static TEE_Result set_clock_then_voltage(unsigned int opp)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	if (_set_opp_clk_rate(opp)) {
		EMSG("Failed to set clock to %ukHz",
		     cpu_opp.dvfs[opp].freq_khz);
		return TEE_ERROR_GENERIC;
	}

#ifdef CFG_STM32MP13
	if (cpu_opp.dvfs[opp].volt_uv <= MPU_RAM_LOW_SPEED_THRESHOLD)
		io_setbits32(stm32_pwr_base(), PWR_CR1_MPU_RAM_LOW_SPEED);
#endif

	res = opp_set_voltage(cpu_opp.regul, cpu_opp.dvfs[opp].volt_uv);
	if (res) {
		unsigned int current_opp = cpu_opp.current_opp;

		if (current_opp == cpu_opp.opp_count)
			panic();

		if (_set_opp_clk_rate(current_opp))
			EMSG("Failed to restore clock");

		return res;
	}

	return TEE_SUCCESS;
}

static TEE_Result set_voltage_then_clock(unsigned int opp)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	res = opp_set_voltage(cpu_opp.regul, cpu_opp.dvfs[opp].volt_uv);
	if (res)
		return res;

#ifdef CFG_STM32MP13
	if (cpu_opp.dvfs[opp].volt_uv > MPU_RAM_LOW_SPEED_THRESHOLD)
		io_clrbits32(stm32_pwr_base(), PWR_CR1_MPU_RAM_LOW_SPEED);
#endif

	if (_set_opp_clk_rate(opp)) {
		unsigned int current_opp = cpu_opp.current_opp;
		unsigned int previous_volt = 0U;

		EMSG("Failed to set clock");

		if (current_opp == cpu_opp.opp_count)
			panic();

		previous_volt = cpu_opp.dvfs[current_opp].volt_uv;

		opp_set_voltage(cpu_opp.regul, previous_volt);

		return TEE_ERROR_GENERIC;
	}

	return TEE_SUCCESS;
}

#ifdef CFG_SCPFW_MOD_DVFS
/* Expose a CPU clock instance for scp-firmware DVFS module */
static TEE_Result scp_set_cpu_rate(struct clk *clk __unused, unsigned long rate,
				   unsigned long parent_rate __unused)
{
	unsigned int __maybe_unused khz = rate / 1000;

#ifdef CFG_STM32MP15
	res = stm32mp1_set_opp_khz(khz);
	if (res)
		return res;
#else
	if (rate != clk_get_rate(cpu_opp.clock))
		return TEE_ERROR_GENERIC;
#endif

	return TEE_SUCCESS;
}

static unsigned long scp_read_cpu_rate(struct clk *clk __unused,
				       unsigned long parent_rate)
{
	return parent_rate;
}

static TEE_Result scp_get_cpu_rates_array(struct clk *clk __unused,
					  size_t start_index,
					  unsigned long *rates,
					  size_t *nb_elts)
{
	size_t rates_cells = *nb_elts;
	size_t opp = 0;

	if (start_index >= cpu_opp.opp_count)
		return TEE_ERROR_BAD_PARAMETERS;

	*nb_elts = cpu_opp.opp_count - start_index;

	if (!rates || rates_cells < *nb_elts)
		return TEE_ERROR_SHORT_BUFFER;

	for (opp = start_index; opp < cpu_opp.opp_count; opp++)
		rates[opp] = (unsigned long)stm32_cpu_opp_level(opp) * 1000;

	return TEE_SUCCESS;
}

static const struct clk_ops stm32_cpu_opp_clk_ops = {
	.get_rate = scp_read_cpu_rate,
	.set_rate = scp_set_cpu_rate,
	.get_rates_array = scp_get_cpu_rates_array,
};

/*
 * Expose a regulator for PSU (Power Supply Unit) instance used
 * by scp-firmware DVFS module
 */
static TEE_Result scp_set_regu_state(struct regulator *regulator __unused,
				     bool enabled __unused)
{
	return TEE_SUCCESS;
}

static TEE_Result scp_read_regu_state(struct regulator *regulator __unused,
				      bool *enabled)
{
	*enabled = true;

	return TEE_SUCCESS;
}

static TEE_Result scp_set_regu_voltage(struct regulator *regulator __unused,
				       int uv)
{
	TEE_Result res = TEE_ERROR_GENERIC;

#ifdef CFG_STM32MP13
	if (uv <= MPU_RAM_LOW_SPEED_THRESHOLD)
		io_setbits32(stm32_pwr_base(), PWR_CR1_MPU_RAM_LOW_SPEED);
#endif

	res = opp_set_voltage(cpu_opp.regul, uv);
	if (res)
		return res;

#ifdef CFG_STM32MP13
	if (uv > MPU_RAM_LOW_SPEED_THRESHOLD)
		io_clrbits32(stm32_pwr_base(), PWR_CR1_MPU_RAM_LOW_SPEED);
#endif

	return TEE_SUCCESS;
}

static TEE_Result scp_read_regu_voltage(struct regulator *regulator __unused,
					int *uv)
{
	*uv = regulator_get_voltage(cpu_opp.regul);

	return TEE_SUCCESS;
}

static TEE_Result scp_regu_voltages(struct regulator *regulator __unused,
				    struct regulator_voltages_desc **desc,
				    const int **levels)
{
	*desc = &cpu_opp.scp_levels_desc;
	*levels = cpu_opp.scp_cpu_opp_levels_uv;

	return TEE_SUCCESS;
}

static const struct regulator_ops stm32_scp_cpu_opp_regu = {
	.set_state = scp_set_regu_state,
	.get_state = scp_read_regu_state,
	.set_voltage = scp_set_regu_voltage,
	.get_voltage = scp_read_regu_voltage,
	.supported_voltages = scp_regu_voltages,
};

static int cmp_cpu_opp_by_freq(const void *a, const void *b)
{
	const struct cpu_dvfs *opp_a = a;
	const struct cpu_dvfs *opp_b = b;

	if (opp_a->freq_khz == opp_b->freq_khz)
		return CMP_TRILEAN(opp_a->volt_uv, opp_b->volt_uv);
	else
		return CMP_TRILEAN(opp_a->freq_khz, opp_b->freq_khz);
}

static int min_cpu_voltage(struct cpu_dvfs *dvfs, size_t count)
{
	int min_mv = INT_MAX;
	size_t n = 0;

	for (n = 0; n < count; n++)
		min_mv = MIN(min_mv, dvfs[n].volt_uv);

	assert(min_mv < INT_MAX);
	return min_mv;
}

static int max_cpu_voltage(struct cpu_dvfs *dvfs, size_t count)
{
	int max_mv = INT_MIN;
	size_t n = 0;

	for (n = 0; n < count; n++)
		max_mv = MAX(max_mv, dvfs[n].volt_uv);

	assert(max_mv > 0);
	return max_mv;
}

TEE_Result optee_scmi_server_init_dvfs(const void *fdt __unused,
				       int node __unused,
				       struct scpfw_agent_config *agent_cfg,
				       struct scpfw_channel_config *channel_cfg)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	unsigned int *dvfs_khz = NULL;
	unsigned int *dvfs_mv = NULL;
	unsigned int *dvfs_opp_khz = NULL;
	unsigned int *dvfs_opp_mv = NULL;
	size_t opp = 0;
	struct cpu_dvfs *sorted_dvfs = NULL;

	assert(agent_cfg && channel_cfg);

	/*
	 * Platform currenty expect only non-secure Cortex-A
	 * (aka agent 1/channel 0) exposes a CPU DFVS service.
	 */
	if (agent_cfg->agent_id != 1 || channel_cfg->channel_id != 0)
		return TEE_SUCCESS;

	/*
	 * Sort operating points by increasing frequencies as expected by
	 * SCP-firmware DVFS module.
	 */
	sorted_dvfs = calloc(cpu_opp.opp_count, sizeof(*sorted_dvfs));
	assert(sorted_dvfs);
	memcpy(sorted_dvfs, cpu_opp.dvfs,
	       cpu_opp.opp_count * sizeof(*sorted_dvfs));

	qsort(sorted_dvfs, cpu_opp.opp_count, sizeof(*sorted_dvfs),
	      cmp_cpu_opp_by_freq);

	/* Setup  a clock for scp-firmare DVFS module clock instance */
	cpu_opp.scp_clock = (struct clk){
		.ops = &stm32_cpu_opp_clk_ops,
		.name = "stm32-cpu-opp",
		.rate = cpu_opp.dvfs[cpu_opp.current_opp].freq_khz * 1000,
#ifndef CFG_STM32MP15
		.flags = CLK_SET_RATE_PARENT,
		.parent = cpu_opp.clock,
#endif
	};

	res = clk_register(&cpu_opp.scp_clock);
	if (res) {
		free(sorted_dvfs);
		return res;
	}

	/* Setup a regulator for scp-firmare DVFS module PSU instance */
	cpu_opp.scp_regulator = (struct regulator){
		.ops = &stm32_scp_cpu_opp_regu,
		.min_uv = min_cpu_voltage(sorted_dvfs, cpu_opp.opp_count),
		.max_uv = max_cpu_voltage(sorted_dvfs, cpu_opp.opp_count),
		.name = (char *)"stm32-cpu-opp",
	};

	cpu_opp.scp_levels_desc = (struct regulator_voltages_desc){
		.type = VOLTAGE_TYPE_FULL_LIST,
		.num_levels = cpu_opp.opp_count,
	};

	cpu_opp.scp_cpu_opp_levels_uv =
		calloc(cpu_opp.opp_count,
		       sizeof(*cpu_opp.scp_cpu_opp_levels_uv));

	assert(cpu_opp.scp_cpu_opp_levels_uv);

	for (opp = 0; opp < cpu_opp.opp_count; opp++)
		cpu_opp.scp_cpu_opp_levels_uv[opp] = sorted_dvfs[opp].volt_uv;

	/* Feed SCP-firmware with CPU DVFS configuration data */
	dvfs_khz = calloc(cpu_opp.opp_count, sizeof(*dvfs_khz));
	dvfs_mv = calloc(cpu_opp.opp_count, sizeof(*dvfs_mv));
	assert(dvfs_khz && dvfs_mv);

	for (opp = 0; opp < cpu_opp.opp_count; opp++) {
		dvfs_khz[opp] = sorted_dvfs[opp].freq_khz;
		dvfs_mv[opp] = sorted_dvfs[opp].volt_uv / U(1000);
	}

	free(sorted_dvfs);

	/*
	 * Fill struct scmi_perfd
	 *
	 * Only perfd[0] will be filled
	 */
	dvfs_opp_khz = calloc(cpu_opp.opp_count, sizeof(*dvfs_opp_khz));
	dvfs_opp_mv = calloc(cpu_opp.opp_count, sizeof(*dvfs_opp_mv));

	channel_cfg->perfd_count = 1;
	channel_cfg->perfd = calloc(channel_cfg->perfd_count,
				    sizeof(*channel_cfg->perfd));

	if (!dvfs_opp_khz || !dvfs_opp_mv || !channel_cfg->perfd) {
		free(dvfs_opp_mv);
		free(dvfs_opp_khz);
		free(cpu_opp.scp_cpu_opp_levels_uv);
		free(channel_cfg->perfd);

		return TEE_ERROR_OUT_OF_MEMORY;
	}
	memcpy(dvfs_opp_mv, dvfs_mv, cpu_opp.opp_count * sizeof(*dvfs_opp_mv));
	memcpy(dvfs_opp_khz, dvfs_khz,
	       cpu_opp.opp_count * sizeof(*dvfs_opp_khz));

	channel_cfg->perfd[0] = (struct scmi_perfd){
		.name = "CPU DVFS",
		.initial_opp = cpu_opp.default_opp,
		.dvfs_opp_count = cpu_opp.opp_count,
		.dvfs_opp_khz = dvfs_opp_khz,
		.dvfs_opp_mv = dvfs_opp_mv,
		.clk = &cpu_opp.scp_clock,
		.regulator = &cpu_opp.scp_regulator,
	};

	free(dvfs_khz);
	free(dvfs_mv);

	return TEE_SUCCESS;
}
#else /*CFG_SCPFW_MOD_DVFS*/
TEE_Result stm32_cpu_opp_set_level(unsigned int level)
{
	unsigned int current_level = 0;
	TEE_Result res = TEE_ERROR_GENERIC;
	unsigned int opp = 0;

	mutex_lock(&cpu_opp_mu);

	/* Perf level relates straight to CPU frequency in kHz */
	current_level = cpu_opp.dvfs[cpu_opp.current_opp].freq_khz;

	if (level == current_level) {
		mutex_unlock(&cpu_opp_mu);
		return TEE_SUCCESS;
	}

	for (opp = 0; opp < cpu_opp.opp_count; opp++)
		if (level == cpu_opp.dvfs[opp].freq_khz)
			break;

	if (opp == cpu_opp.opp_count) {
		mutex_unlock(&cpu_opp_mu);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	if (level < current_level)
		res = set_clock_then_voltage(opp);
	else
		res = set_voltage_then_clock(opp);

	if (!res)
		cpu_opp.current_opp = opp;

	mutex_unlock(&cpu_opp_mu);

	return res;
}

TEE_Result stm32_cpu_opp_read_level(unsigned int *level)
{
	if (cpu_opp.current_opp >= cpu_opp.opp_count)
		return TEE_ERROR_BAD_STATE;

	*level = cpu_opp.dvfs[cpu_opp.current_opp].freq_khz;

	return TEE_SUCCESS;
}
#endif /*CFG_SCPFW_MOD_DVFS*/

static TEE_Result set_opp(unsigned int opp)
{
	unsigned long clk_cpu = clk_get_rate(cpu_opp.clock);
	TEE_Result res = TEE_ERROR_GENERIC;

	assert(clk_cpu);
	if (cpu_opp.dvfs[opp].freq_khz * 1000U >= clk_cpu)
		res = set_voltage_then_clock(opp);
	else
		res = set_clock_then_voltage(opp);

	if (res)
		DMSG("Failed to set OPP %ukHz",
		     cpu_opp.dvfs[opp].freq_khz);

	return res;
}

static TEE_Result cpu_opp_pm(enum pm_op op, unsigned int pm_hint,
			     const struct pm_callback_handle *hdl __unused)
{
	assert(op == PM_OP_SUSPEND || op == PM_OP_RESUME);

	/* nothing to do if OPP is managed by Linux and not by SCMI */
	if (!IS_ENABLED(CFG_SCMI_MSG_PERF_DOMAIN) &&
	    !IS_ENABLED(CFG_SCPFW_MOD_DVFS))
		return TEE_SUCCESS;

#if defined(CFG_STM32MP25) || defined(CFG_STM32MP23) || defined(CFG_STM32MP21)
	/* Nothing to do on Stop1, LP-Stop1 modes */
	if (PM_HINT_PLATFORM_STATE(pm_hint) == PM_CORE_LEVEL)
		return TEE_SUCCESS;
#else
	/* Nothing to do if RCC clock tree is not lost */
	if (!PM_HINT_IS_STATE(pm_hint, CONTEXT))
		return TEE_SUCCESS;
#endif

	if (op == PM_OP_SUSPEND) {
#ifdef CFG_SCPFW_MOD_DVFS
		unsigned int opp = 0;

		/*
		 * When CFG_SCPFW_MOD_DVFS is enabled, save CPU OPP on suspend
		 * for restoration at resume. If CPU is not in an expected
		 * OPP state, fallback to default OPP at PM resume.
		 */
		unsigned int cur_khz = 0;
		int cur_uv = 0;

		cur_khz = clk_get_rate(cpu_opp.clock) / 1000;

		cur_uv = regulator_get_voltage(cpu_opp.regul);
		if (!cur_uv)
			panic();

		/* Get current OPP or fallback to default */
		for (opp = 0; opp < cpu_opp.opp_count; opp++)
			if (cur_khz == cpu_opp.dvfs[opp].freq_khz  &&
			    cur_uv == cpu_opp.dvfs[opp].volt_uv)
				break;
		if (opp >= cpu_opp.opp_count) {
			EMSG("Unexpected OPP state, select default");

			opp = cpu_opp.default_opp;
		}
		cpu_opp.current_opp = opp;
#endif
		/*
		 * Force default OPP before suspend
		 * On STM32MP2, it is not allowed to enter LPLV-Stop
		 * with overdrive voltage on VDDCPU
		 */
		DMSG("Suspend to OPP %u", cpu_opp.default_opp);

		if (cpu_opp.current_opp == cpu_opp.default_opp)
			return TEE_SUCCESS;

		return set_opp(cpu_opp.default_opp);
	}

	if (op == PM_OP_RESUME) {
		DMSG("Resume to OPP %u", cpu_opp.current_opp);

		/*
		 * Optimize resume from Stop modes: do not reset
		 * the OPP if we where on default before suspend.
		 */
		if (!PM_HINT_IS_STATE(pm_hint, CONTEXT) &&
		    cpu_opp.current_opp == cpu_opp.default_opp)
			return TEE_SUCCESS;

		return set_opp(cpu_opp.current_opp);
	}

	return TEE_SUCCESS;
}
DECLARE_KEEP_PAGER_PM(cpu_opp_pm);

static TEE_Result stm32_cpu_opp_is_supported(const void *fdt, int subnode)
{
	const fdt32_t *cuint32 = NULL;
	uint32_t opp = 0;

	cuint32 = fdt_getprop(fdt, subnode, "opp-supported-hw", NULL);

	if (!cuint32) {
		DMSG("Can't find property opp-supported-hw");
		return TEE_ERROR_GENERIC;
	}

	opp = fdt32_to_cpu(*cuint32);
	if (!stm32mp_supports_cpu_opp(opp)) {
		DMSG("Not supported opp-supported-hw %#"PRIx32, opp);
		return TEE_ERROR_GENERIC;
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_cpu_opp_get_dt_subnode(const void *fdt, int node)
{
	const fdt64_t *cuint64 = NULL;
	const fdt32_t *cuint32 = NULL;
	uint64_t freq_hz = 0;
	uint64_t freq_khz = 0;
	uint64_t freq_khz_opp_def = 0;
	uint32_t volt_uv = 0;
	unsigned int i = 0;
	int subnode = -1;
	TEE_Result res = TEE_ERROR_GENERIC;
	bool opp_default = false;

	fdt_for_each_subnode(subnode, fdt, node)
		if (stm32_cpu_opp_is_supported(fdt, subnode) == TEE_SUCCESS)
			cpu_opp.opp_count++;

	cpu_opp.dvfs = calloc(cpu_opp.opp_count, sizeof(*cpu_opp.dvfs));
	if (!cpu_opp.dvfs)
		return TEE_ERROR_OUT_OF_MEMORY;

	cpu_opp.default_opp = cpu_opp.opp_count;

	fdt_for_each_subnode(subnode, fdt, node) {
		if (stm32_cpu_opp_is_supported(fdt, subnode) != TEE_SUCCESS)
			continue;

		cuint64 = fdt_getprop(fdt, subnode, "opp-hz", NULL);
		if (!cuint64) {
			EMSG("Missing opp-hz in node %s",
			     fdt_get_name(fdt, subnode, NULL));
			res = TEE_ERROR_GENERIC;
			goto err;
		}

		freq_hz = fdt64_to_cpu(*cuint64);
		freq_khz = freq_hz / 1000ULL;
		if (freq_khz > (uint64_t)UINT32_MAX) {
			EMSG("Invalid opp-hz %"PRIu64" in node %s",
			     freq_khz, fdt_get_name(fdt, subnode, NULL));
			res = TEE_ERROR_GENERIC;
			goto err;
		}

		cuint32 = fdt_getprop(fdt, subnode, "opp-microvolt", NULL);
		if (!cuint32) {
			EMSG("Missing opp-microvolt in node %s",
			     fdt_get_name(fdt, subnode, NULL));
			res = TEE_ERROR_GENERIC;
			goto err;
		}

		volt_uv = fdt32_to_cpu(*cuint32);

		/* skip OPP when frequency is not supported */
		if (freq_hz != clk_round_rate(cpu_opp.clock, freq_hz)) {
			DMSG("Skip OPP %"PRIu64"kHz/%"PRIu32"uV",
			     freq_khz, volt_uv);
			cpu_opp.opp_count--;
			continue;
		}

		/* skip OPP when voltage is not supported */
		if (!opp_voltage_is_supported(cpu_opp.regul, &volt_uv)) {
			DMSG("Skip OPP %"PRIu64"kHz/%"PRIu32"uV",
			     freq_khz, volt_uv);
			cpu_opp.opp_count--;
			continue;
		}

		cpu_opp.dvfs[i].freq_khz = freq_khz;
		cpu_opp.dvfs[i].volt_uv = volt_uv;

		DMSG("Found OPP %u (%"PRIu64"kHz/%"PRIu32"uV) from DT",
		     i, freq_khz, volt_uv);

		if (fdt_getprop(fdt, subnode, "st,opp-default", NULL) &&
		    freq_khz > freq_khz_opp_def) {
			opp_default = true;
			cpu_opp.default_opp = i;
			freq_khz_opp_def = freq_khz;
		}

		i++;
	}

	/* At least one OPP node shall have a "st,opp-default" property */
	if (!opp_default) {
		EMSG("No default OPP found");
		res = TEE_ERROR_GENERIC;
		goto err;
	}

	/* Select the max "st,opp-default" node as current OPP */
	res = set_opp(cpu_opp.default_opp);
	if (res)
		goto err;

	cpu_opp.current_opp = cpu_opp.default_opp;

	register_pm_driver_cb(cpu_opp_pm, NULL, "cpu-opp");

	return TEE_SUCCESS;

err:
	free(cpu_opp.dvfs);

	return res;
}

static TEE_Result
stm32_cpu_opp_init(const void *fdt, int cpu_node, int node,
		   const void *compat_data __unused)
{
	TEE_Result res = TEE_SUCCESS;
	uint16_t __maybe_unused cpu_voltage = 0;

	res = clk_dt_get_by_index(fdt, cpu_node, 0, &cpu_opp.clock);
	if (res)
		return res;

	res = regulator_dt_get_supply(fdt, cpu_node, "cpu", &cpu_opp.regul);
	if (res)
		return res;

#ifdef CFG_STM32MP15
	cpu_voltage = regulator_get_voltage(cpu_opp.regul);

	if (stm32mp1_clk_compute_all_pll1_settings(fdt, node, cpu_voltage))
		panic();
#endif

	res = stm32_cpu_opp_get_dt_subnode(fdt, node);
	if (res)
		return res;

	return TEE_SUCCESS;
}

static TEE_Result
stm32_cpu_init(const void *fdt, int node, const void *compat_data __unused)
{
	const fdt32_t *cuint = NULL;
	int opp_node = 0;
	int len = 0;
	uint32_t phandle = 0;

	cuint = fdt_getprop(fdt, node, "operating-points-v2", &len);
	if (!cuint || len != sizeof(uint32_t)) {
		DMSG("Missing operating-points-v2");
		return TEE_SUCCESS;
	}

	phandle = fdt32_to_cpu(*cuint);
	opp_node = fdt_node_offset_by_phandle(fdt, phandle);

	return stm32_cpu_opp_init(fdt, node, opp_node, compat_data);
}

static const struct dt_device_match stm32_cpu_match_table[] = {
	{ .compatible = "arm,cortex-a7" },
	{ .compatible = "arm,cortex-a35" },
	{ }
};

DEFINE_DT_DRIVER(stm32_cpu_dt_driver) = {
	.name = "stm32-cpu",
	.match_table = stm32_cpu_match_table,
	.probe = &stm32_cpu_init,
};

static TEE_Result stm32_cpu_initcall(void)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	const void *fdt = get_dt();
	int node = fdt_path_offset(fdt, "/cpus/cpu@0");

	if (node < 0) {
		EMSG("cannot find /cpus/cpu@0 node");
		panic();
	}

	res = dt_driver_maybe_add_probe_node(fdt, node);
	if (res) {
		EMSG("Failed on node %s with %#"PRIx32,
		     fdt_get_name(fdt, node, NULL), res);
		panic();
	}

	return TEE_SUCCESS;
}

driver_init(stm32_cpu_initcall);

