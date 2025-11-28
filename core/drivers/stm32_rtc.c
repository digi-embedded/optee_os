// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2018-2022, STMicroelectronics - All Rights Reserved
 *
 */
#ifdef CFG_ARM64_core
#include <arm64.h>
#else
#include <arm32.h>
#endif /* CFG_ARM64_core */
#include <config.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <drivers/rtc.h>
#include <drivers/stm32_rtc.h>
#include <drivers/stm32_rif.h>
#include <io.h>
#include <keep.h>
#include <kernel/dt.h>
#include <kernel/interrupt.h>
#include <kernel/notif.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <libfdt.h>
#include <mm/core_memprot.h>

/*
 * Registers
 */
#define RTC_TR				U(0x00)
#define RTC_DR				U(0x04)
#define RTC_SSR				U(0x08)
#define RTC_ICSR			U(0x0C)
#define RTC_PRER			U(0x10)
#define RTC_WUTR			U(0x14)
#define RTC_CR				U(0x18)
#define RTC_PRIVCFGR			U(0x1C)
/* RTC_SMCR is linked to RTC3v1_2 */
#define RTC_SMCR			U(0x20)
/* RTC_SECCFGR is linked to RTC3v3_2 and above */
#define RTC_SECCFGR			U(0x20)
#define RTC_WPR				U(0x24)
#define RTC_CALR			U(0x28)
#define RTC_SHIFTR			U(0x2C)
#define RTC_TSTR			U(0x30)
#define RTC_TSDR			U(0x34)
#define RTC_TSSSR			U(0x38)
#define RTC_ALRMAR			U(0x40)
#define RTC_ALRMASSR			U(0x44)
#define RTC_ALRMBR			U(0x48)
#define RTC_ALRMBSSR			U(0x4C)
#define RTC_SR				U(0x50)
#define RTC_SCR				U(0x5C)
#define RTC_OR				U(0x60)
#define RTC_CIDCFGR(x)			(U(0x80) + U(0x4) * (x))

#define RTC_TR_SU_MASK			GENMASK_32(3, 0)
#define RTC_TR_ST_MASK			GENMASK_32(6, 4)
#define RTC_TR_ST_SHIFT			U(4)
#define RTC_TR_MNU_MASK			GENMASK_32(11, 8)
#define RTC_TR_MNU_SHIFT		U(8)
#define RTC_TR_MNT_MASK			GENMASK_32(14, 12)
#define RTC_TR_MNT_SHIFT		U(12)
#define RTC_TR_HU_MASK			GENMASK_32(19, 16)
#define RTC_TR_HU_SHIFT			U(16)
#define RTC_TR_HT_MASK			GENMASK_32(21, 20)
#define RTC_TR_HT_SHIFT			U(20)
#define RTC_TR_PM			BIT(22)

#define RTC_DR_DU_MASK			GENMASK_32(3, 0)
#define RTC_DR_DT_MASK			GENMASK_32(5, 4)
#define RTC_DR_DT_SHIFT			U(4)
#define RTC_DR_MU_MASK			GENMASK_32(11, 8)
#define RTC_DR_MU_SHIFT			U(8)
#define RTC_DR_MT_MASK			BIT(12)
#define RTC_DR_MT_SHIFT			U(12)
#define RTC_DR_WDU_MASK			GENMASK_32(15, 13)
#define RTC_DR_WDU_SHIFT		U(13)
#define RTC_DR_YU_MASK			GENMASK_32(19, 16)
#define RTC_DR_YU_SHIFT			U(16)
#define RTC_DR_YT_MASK			GENMASK_32(23, 20)
#define RTC_DR_YT_SHIFT			U(20)

#define RTC_SSR_SS_MASK			GENMASK_32(15, 0)

#define RTC_ICSR_RSF			BIT(5)
#define RTC_ICSR_INITF			BIT(6)
#define RTC_ICSR_INIT			BIT(7)

#define RTC_PRER_PREDIV_S_SHIFT		U(0)
#define RTC_PRER_PREDIV_S_MASK		GENMASK_32(14, 0)
#define RTC_PRER_PREDIV_A_SHIFT		U(16)
#define RTC_PRER_PREDIV_A_MASK		GENMASK_32(22, 16)

#define RTC_CR_BYPSHAD			BIT(5)
#define RTC_CR_BYPSHAD_SHIFT		U(5)
#define RTC_CR_FMT			BIT(6)
#define RTC_CR_ALRAE			BIT(8)
#define RTC_CR_ALRAIE			BIT(12)
#define RTC_CR_TAMPTS			BIT(25)

#define RTC_PRIVCFGR_VALUES		GENMASK_32(3, 0)
#define RTC_PRIVCFGR_VALUES_TO_SHIFT	GENMASK_32(5, 4)
#define RTC_PRIVCFGR_SHIFT		U(9)
#define RTC_PRIVCFGR_MASK		(GENMASK_32(14, 13) | GENMASK_32(3, 0))
#define RTC_PRIVCFGR_FULL_PRIV		BIT(15)

#define RTC_SMCR_TS_DPROT		BIT(3)

#define RTC_SECCFGR_VALUES		GENMASK_32(3, 0)
#define RTC_SECCFGR_TS_SEC		BIT(3)
#define RTC_SECCFGR_VALUES_TO_SHIFT	GENMASK_32(5, 4)
#define RTC_SECCFGR_SHIFT		U(9)
#define RTC_SECCFGR_MASK		(GENMASK_32(14, 13) | GENMASK_32(3, 0))
#define RTC_SECCFGR_FULL_SEC		BIT(15)

#define RTC_WPR_KEY1			U(0xCA)
#define RTC_WPR_KEY2			U(0x53)
#define RTC_WPR_KEY_LOCK		U(0xFF)

#define RTC_TSDR_MU_MASK		GENMASK_32(11, 8)
#define RTC_TSDR_MU_SHIFT		U(8)
#define RTC_TSDR_DT_MASK		GENMASK_32(5, 4)
#define RTC_TSDR_DT_SHIFT		U(4)
#define RTC_TSDR_DU_MASK		GENMASK_32(3, 0)
#define RTC_TSDR_DU_SHIFT		U(0)

#define RTC_ALRMXR_SEC_UNITS_MASK	GENMASK_32(3, 0)
#define RTC_ALRMXR_SEC_UNITS_SHIFT	U(0)
#define RTC_ALRMXR_SEC_TENS_MASK	GENMASK_32(6, 4)
#define RTC_ALRMXR_SEC_TENS_SHIFT	U(4)
#define RTC_ALRMXR_SEC_MASK		BIT(7)
#define RTC_ALRMXR_MIN_UNITS_MASK	GENMASK_32(11, 8)
#define RTC_ALRMXR_MIN_UNITS_SHIFT	U(8)
#define RTC_ALRMXR_MIN_TENS_MASK	GENMASK_32(14, 12)
#define RTC_ALRMXR_MIN_TENS_SHIFT	U(12)
#define RTC_ALRMXR_MIN_MASK		BIT(15)
#define RTC_ALRMXR_HOUR_UNITS_MASK	GENMASK_32(19, 16)
#define RTC_ALRMXR_HOUR_UNITS_SHIFT	U(16)
#define RTC_ALRMXR_HOUR_TENS_MASK	GENMASK_32(21, 20)
#define RTC_ALRMXR_HOUR_TENS_SHIFT	U(20)
#define RTC_ALRMXR_PM			BIT(22)
#define RTC_ALRMXR_HOUR_MASK		BIT(23)
#define RTC_ALRMXR_DATE_UNITS_MASK	GENMASK_32(27, 24)
#define RTC_ALRMXR_DATE_UNITS_SHIFT	U(24)
#define RTC_ALRMXR_DATE_TENS_MASK	GENMASK_32(29, 28)
#define RTC_ALRMXR_DATE_TENS_SHIFT	U(28)

#define RTC_SR_ALRAF			BIT(0)
#define RTC_SR_TSF			BIT(3)
#define RTC_SR_TSOVF			BIT(4)

#define RTC_SCR_CALRAF			BIT(0)
#define RTC_SCR_CTSF			BIT(3)
#define RTC_SCR_CTSOVF			BIT(4)

#define RTC_CIDCFGR_SCID_MASK		GENMASK_32(6, 4)
#define RTC_CIDCFGR_SCID_MASK_SHIFT	U(4)
#define RTC_CIDCFGR_CONF_MASK		(_CIDCFGR_CFEN |	 \
					 RTC_CIDCFGR_SCID_MASK)

/*
 * RIF miscellaneous
 */
#define RTC_NB_RIF_RESOURCES		U(6)

#define RTC_RIF_FULL_PRIVILEGED		U(0x3F)
#define RTC_RIF_FULL_SECURED		U(0x3F)

#define RTC_NB_MAX_CID_SUPPORTED	U(7)

#define RTC_CID_1			U(1)

/*
 * Driver miscellaneous
 */
#define RTC_RES_TIMESTAMP		U(3)

#define RTC_FLAGS_READ_TWICE		BIT(0)
#define RTC_FLAGS_SECURE		BIT(1)

#define TIMEOUT_US_RTC_SHADOW		U(10000)
#define MS_PER_SEC			U(1000)
#define TIMEOUT_US_RTC_GENERIC		U(100000)

struct rtc_compat {
	bool has_seccfgr;
	bool has_rif_support;
};

/*
 * struct rtc_device - RTC device structure
 * @base: Base address of the RTC device
 * @compat: RTC device compatibility
 * @pclk: RTC peripheral clock
 * @rtc_ck: RTC core clock
 * @conf_data: array of RIF configuration data
 * @nb_res: Number of RIF configurations
 * @flags: RTC device flags
 * @is_secured: RTC device secured status
 * @itr_chip: Interrupt chip
 * @itr_num: Interrupt number
 * @itr_handler: Interrupt handler
 * @notif_id: Notification ID
 * @wait_alarm_return_status: Status of the wait alarm
 * @rtc: information for OP-TEE RTC device
 */
struct rtc_device {
	struct io_pa_va base;
	struct rtc_compat compat;
	struct clk *pclk;
	struct clk *rtc_ck;
	struct rif_conf_data *conf_data;
	unsigned int nb_res;
	uint8_t flags;
	bool is_secured;
	struct itr_chip *itr_chip;
	size_t itr_num;
	struct itr_handler *itr_handler;
	uint32_t notif_id;
	enum rtc_wait_alarm_status wait_alarm_return_status;
	struct rtc *rtc;
};

/* Expect a single RTC instance */
static struct rtc_device rtc_dev;

static vaddr_t get_base(void)
{
	assert(rtc_dev.base.pa);

	return io_pa_or_va(&rtc_dev.base, 1);
}

static void stm32_rtc_write_unprotect(void)
{
	vaddr_t rtc_base = get_base();

	io_write32(rtc_base + RTC_WPR, RTC_WPR_KEY1);
	io_write32(rtc_base + RTC_WPR, RTC_WPR_KEY2);
}

static void stm32_rtc_write_protect(void)
{
	vaddr_t rtc_base = get_base();

	io_write32(rtc_base + RTC_WPR, RTC_WPR_KEY_LOCK);
}

static bool stm32_rtc_get_bypshad(void)
{
	return io_read32(get_base() + RTC_CR) & RTC_CR_BYPSHAD;
}

/* Get calendar data from RTC devicecalendar valueregister values */
static void stm32_rtc_read_calendar(struct stm32_rtc_calendar *calendar)
{
	vaddr_t rtc_base = get_base();
	bool bypshad = stm32_rtc_get_bypshad();

	if (!bypshad) {
		uint64_t to = 0;

		/* Wait calendar registers are ready */
		io_clrbits32(rtc_base + RTC_ICSR, RTC_ICSR_RSF);

		to = timeout_init_us(TIMEOUT_US_RTC_SHADOW);
		while (!(io_read32(rtc_base + RTC_ICSR) & RTC_ICSR_RSF))
			if (timeout_elapsed(to))
				break;

		if (!(io_read32(rtc_base + RTC_ICSR) & RTC_ICSR_RSF))
			panic();
	}

	calendar->ssr = io_read32(rtc_base + RTC_SSR);
	calendar->tr = io_read32(rtc_base + RTC_TR);
	calendar->dr = io_read32(rtc_base + RTC_DR);
}

/* Fill the RTC timestamp structure from a given RTC time-in-day value */
static void stm32_rtc_fill_time(struct stm32_rtc_calendar *cal,
				struct optee_rtc_time *tm)
{
	tm->tm_hour = (((cal->tr & RTC_TR_HT_MASK) >> RTC_TR_HT_SHIFT) * 10) +
		   ((cal->tr & RTC_TR_HU_MASK) >> RTC_TR_HU_SHIFT);

	if (cal->tr & RTC_TR_PM)
		tm->tm_hour += 12;

	tm->tm_min = (((cal->tr & RTC_TR_MNT_MASK) >> RTC_TR_MNT_SHIFT) * 10) +
		  ((cal->tr & RTC_TR_MNU_MASK) >> RTC_TR_MNU_SHIFT);
	tm->tm_sec = (((cal->tr & RTC_TR_ST_MASK) >> RTC_TR_ST_SHIFT) * 10) +
		  (cal->tr & RTC_TR_SU_MASK);
}

/* Fill the RTC timestamp structure from a given RTC date value */
static void stm32_rtc_fill_date(struct stm32_rtc_calendar *cal,
				struct optee_rtc_time *tm)
{
	tm->tm_wday = (((cal->dr & RTC_DR_WDU_MASK) >> RTC_DR_WDU_SHIFT));

	tm->tm_mday = (((cal->dr & RTC_DR_DT_MASK) >> RTC_DR_DT_SHIFT) * 10) +
		  (cal->dr & RTC_DR_DU_MASK);

	tm->tm_mon = (((cal->dr & RTC_DR_MT_MASK) >> RTC_DR_MT_SHIFT) * 10) +
		    ((cal->dr & RTC_DR_MU_MASK) >> RTC_DR_MU_SHIFT);

	tm->tm_year = (((cal->dr & RTC_DR_YT_MASK) >> RTC_DR_YT_SHIFT) * 10) +
		   ((cal->dr & RTC_DR_YU_MASK) >> RTC_DR_YU_SHIFT) + 2000;
}

/* Update time value with RTC timestamp */
static void stm32_rtc_read_timestamp(struct optee_rtc_time *time)
{
	struct stm32_rtc_calendar cal_tamp = { };
	vaddr_t rtc_base = get_base();

	cal_tamp.tr = io_read32(rtc_base + RTC_TSTR);
	cal_tamp.dr = io_read32(rtc_base + RTC_TSDR);
	stm32_rtc_fill_time(&cal_tamp, time);
	stm32_rtc_fill_date(&cal_tamp, time);
}

TEE_Result stm32_rtc_get_calendar(struct stm32_rtc_calendar *calendar)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	if (!rtc_dev.pclk)
		return TEE_ERROR_GENERIC;

	res = clk_enable(rtc_dev.pclk);
	if (res)
		return res;

	stm32_rtc_read_calendar(calendar);

	/* RTC may need to be read twice, depending of clocks configuration */
	if (rtc_dev.flags & RTC_FLAGS_READ_TWICE) {
		uint32_t tr_save = calendar->tr;

		stm32_rtc_read_calendar(calendar);

		if (calendar->tr != tr_save)
			stm32_rtc_read_calendar(calendar);
	}

	clk_disable(rtc_dev.pclk);

	return TEE_SUCCESS;
}

/*
 * Get the subsecond value.
 *
 * Subsecond is a counter that grows from 0 to stm32_rtc_get_subsecond_scale()
 * every second.
 */
static uint32_t stm32_rtc_get_subsecond(struct stm32_rtc_calendar *cal)
{
	uint32_t prediv_s = io_read32(get_base() + RTC_PRER) &
			    RTC_PRER_PREDIV_S_MASK;
	uint32_t ss = cal->ssr & RTC_SSR_SS_MASK;

	return prediv_s - ss;
}

/*
 * Get the subsecond scale.
 *
 * Number of subseconds in a second is linked to RTC PREDIV_S value.
 * The more PREDIV_S will be high, the more subseconds will be precise.
 */
static uint32_t stm32_rtc_get_subsecond_scale(void)
{
	return (io_read32(get_base() + RTC_PRER) & RTC_PRER_PREDIV_S_MASK) + 1;
}

/* Return relative difference of ticks count between two rtc calendar */
static signed long long
stm32_rtc_diff_subs_tick(struct stm32_rtc_calendar *cur,
			 struct stm32_rtc_calendar *ref,
			 unsigned long tick_rate)
{
	return (((signed long long)stm32_rtc_get_subsecond(cur) -
		 (signed long long)stm32_rtc_get_subsecond(ref)) *
		(signed long long)tick_rate) /
		stm32_rtc_get_subsecond_scale();
}

/* Return relative difference in milliseconds on subsecond */
static signed long long stm32_rtc_diff_subs_ms(struct stm32_rtc_calendar *cur,
					       struct stm32_rtc_calendar *ref)
{
	return stm32_rtc_diff_subs_tick(cur, ref, MS_PER_SEC);
}

/* Return absolute difference in milliseconds on seconds-in-day fraction */
static signed long long stm32_rtc_diff_time_ms(struct optee_rtc_time *current,
					       struct optee_rtc_time *ref)
{
	signed long long curr_s = 0;
	signed long long ref_s = 0;

	curr_s = (signed long long)current->tm_sec +
		 (((signed long long)current->tm_min +
		  (((signed long long)current->tm_hour * 60))) * 60);

	ref_s = (signed long long)ref->tm_sec +
		(((signed long long)ref->tm_min +
		 (((signed long long)ref->tm_hour * 60))) * 60);

	return (curr_s - ref_s) * 1000U;
}

/* Return absolute difference in milliseconds on day-in-year fraction */
static signed long long stm32_rtc_diff_date_ms(struct optee_rtc_time *current,
					       struct optee_rtc_time *ref)
{
	uint32_t diff_in_days = 0;
	uint32_t m = 0;
	const uint8_t month_len[] = {
		31, 28, 31, 30, 31, 30,
		31, 31, 30, 31, 30, 31
	};

	/* Get the number of non-entire month days */
	if (current->tm_mday >= ref->tm_mday)
		diff_in_days += current->tm_mday - ref->tm_mday;
	else
		diff_in_days += month_len[ref->tm_mon - 1] -
				ref->tm_mday + current->tm_mday;

	/* Get the number of entire months, and compute the related days */
	if (current->tm_mon > (ref->tm_mon + 1))
		for (m = ref->tm_mon + 1; m < current->tm_mon && m < 12; m++)
			diff_in_days += month_len[m - 1];

	if (current->tm_mon < (ref->tm_mon - 1)) {
		for (m = 1; m < current->tm_mon && m < 12; m++)
			diff_in_days += month_len[m - 1];

		for (m = ref->tm_mon + 1; m < 12; m++)
			diff_in_days += month_len[m - 1];
	}

	/* Get complete years */
	if (current->tm_year > (ref->tm_year + 1))
		diff_in_days += (current->tm_year - ref->tm_year - 1) * 365;

	/* Particular cases: leap years (one day more) */
	if (diff_in_days > 0) {
		if (current->tm_year == ref->tm_year) {
			if (rtc_is_a_leap_year(current->tm_year) &&
			    ref->tm_mon <= 2 &&
			    current->tm_mon >= 3 && current->tm_mday <= 28)
				diff_in_days++;
		} else {
			uint32_t y = 0;

			/* Ref year is leap */
			if (rtc_is_a_leap_year(ref->tm_year) &&
			    ref->tm_mon <= 2 && ref->tm_mday <= 28)
				diff_in_days++;

			/* Current year is leap */
			if (rtc_is_a_leap_year(current->tm_year) &&
			    current->tm_mon >= 3)
				diff_in_days++;

			/* Interleaved years are leap */
			for (y = ref->tm_year + 1; y < current->tm_year; y++)
				if (rtc_is_a_leap_year(y))
					diff_in_days++;
		}
	}

	return (24 * 60 * 60 * 1000) * (signed long long)diff_in_days;
}

/*
 * Return time diff in milliseconds between current and reference time
 * System will panic if stm32_rtc_calendar "cur" is older than "ref".
 */
unsigned long long stm32_rtc_diff_calendar_ms(struct stm32_rtc_calendar *cur,
					      struct stm32_rtc_calendar *ref)
{
	signed long long diff_in_ms = 0;
	struct optee_rtc_time curr_t = { };
	struct optee_rtc_time ref_t = { };

	stm32_rtc_fill_date(cur, &curr_t);
	stm32_rtc_fill_date(ref, &ref_t);
	stm32_rtc_fill_time(cur, &curr_t);
	stm32_rtc_fill_time(ref, &ref_t);

	diff_in_ms += stm32_rtc_diff_subs_ms(cur, ref);
	diff_in_ms += stm32_rtc_diff_time_ms(&curr_t, &ref_t);
	diff_in_ms += stm32_rtc_diff_date_ms(&curr_t, &ref_t);

	if (diff_in_ms < 0)
		panic("Negative time difference is not allowed");

	return (unsigned long long)diff_in_ms;
}

/*
 * Return time diff in tick count between current and reference time
 * System will panic if stm32_rtc_calendar "cur" is older than "ref".
 */
unsigned long long stm32_rtc_diff_calendar_tick(struct stm32_rtc_calendar *cur,
						struct stm32_rtc_calendar *ref,
						unsigned long tick_rate)
{
	signed long long diff_in_tick = 0;
	struct optee_rtc_time curr_t = { };
	struct optee_rtc_time ref_t = { };

	stm32_rtc_fill_date(cur, &curr_t);
	stm32_rtc_fill_date(ref, &ref_t);
	stm32_rtc_fill_time(cur, &curr_t);
	stm32_rtc_fill_time(ref, &ref_t);

	diff_in_tick += stm32_rtc_diff_subs_tick(cur, ref, tick_rate);
	diff_in_tick += stm32_rtc_diff_time_ms(&curr_t, &ref_t) *
			UDIV_ROUND_NEAREST(tick_rate, MS_PER_SEC);
	diff_in_tick += stm32_rtc_diff_date_ms(&curr_t, &ref_t) *
			UDIV_ROUND_NEAREST(tick_rate, MS_PER_SEC);

	if (diff_in_tick < 0)
		panic("Negative time difference is not allowed");

	return (unsigned long long)diff_in_tick;
}

TEE_Result stm32_rtc_get_timestamp(struct optee_rtc_time *tamp_ts)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	vaddr_t rtc_base = get_base();

	if (!rtc_dev.pclk)
		return TEE_ERROR_GENERIC;

	res = clk_enable(rtc_dev.pclk);
	if (res)
		return res;

	if (io_read32(rtc_base + RTC_SR) & RTC_SR_TSF) {
		/* Timestamp for tamper event */
		stm32_rtc_read_timestamp(tamp_ts);
		io_setbits32(rtc_base + RTC_SCR, RTC_SCR_CTSF);

		/* Overflow detection */
		if (io_read32(rtc_base + RTC_SR) & RTC_SR_TSOVF)
			io_setbits32(rtc_base + RTC_SCR, RTC_SCR_CTSOVF);
	}

	clk_disable(rtc_dev.pclk);

	return TEE_SUCCESS;
}

TEE_Result stm32_rtc_set_tamper_timestamp(void)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	vaddr_t rtc_base = get_base();

	if (!rtc_dev.pclk)
		return TEE_ERROR_GENERIC;

	res = clk_enable(rtc_dev.pclk);
	if (res)
		return res;

	stm32_rtc_write_unprotect();

	/* Enable tamper timestamper */
	io_setbits32(rtc_base + RTC_CR, RTC_CR_TAMPTS);

	/* Secure Timestamp bit */
	if (!rtc_dev.compat.has_seccfgr) {
		io_clrbits32(rtc_base + RTC_SMCR, RTC_SMCR_TS_DPROT);
	} else {
		/* Inverted logic */
		io_setbits32(rtc_base + RTC_SECCFGR, RTC_SECCFGR_TS_SEC);
	}

	stm32_rtc_write_protect();

	clk_disable(rtc_dev.pclk);

	return TEE_SUCCESS;
}

TEE_Result stm32_rtc_is_timestamp_enable(bool *ret)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	if (!rtc_dev.pclk)
		return res;

	res = clk_enable(rtc_dev.pclk);
	if (res)
		return res;

	*ret = io_read32(get_base() + RTC_CR) & RTC_CR_TAMPTS;

	clk_disable(rtc_dev.pclk);

	return TEE_SUCCESS;
}

TEE_Result stm32_rtc_driver_is_initialized(void)
{
	if (rtc_dev.pclk)
		return TEE_SUCCESS;

	return TEE_ERROR_DEFER_DRIVER_INIT;
}

static TEE_Result check_rif_config(void)
{
	uint32_t rxcidcfgr = io_read32(get_base() +
				       RTC_CIDCFGR(RTC_RES_TIMESTAMP));
	uint32_t cid = (rxcidcfgr & RTC_CIDCFGR_SCID_MASK) >>
		       RTC_CIDCFGR_SCID_MASK_SHIFT;

	/* Check if TAMPTS is available for our CID */
	if ((rxcidcfgr & _CIDCFGR_CFEN) && (cid != RTC_CID_1))
		return TEE_ERROR_ACCESS_DENIED;

	return TEE_SUCCESS;
}

static void apply_rif_config(bool is_tdcid)
{
	vaddr_t base = get_base();
	unsigned int shifted_values = 0;
	uint32_t seccfgr = 0;
	uint32_t privcfgr = 0;
	uint32_t access_mask_reg = 0;
	unsigned int i = 0;

	if (!rtc_dev.conf_data)
		return;

	/* Build access mask for RTC_SECCFGR and RTC_PRIVCFGR */
	for (i = 0; i < RTC_NB_RIF_RESOURCES; i++) {
		if (rtc_dev.conf_data->access_mask[0] & BIT(i)) {
			if (i <= RTC_RES_TIMESTAMP)
				access_mask_reg |= BIT(i);
			else
				access_mask_reg |= BIT(i) << RTC_SECCFGR_SHIFT;
		}
	}

	for (i = 0; i < RTC_NB_RIF_RESOURCES; i++) {
		if (!(BIT(i) & rtc_dev.conf_data->access_mask[0]))
			continue;

		/*
		 * When TDCID, OP-TEE should be the one to set the CID filtering
		 * configuration. Clearing previous configuration prevents
		 * undesired events during the only legitimate configuration.
		 */
		if (is_tdcid)
			io_clrbits32(base + RTC_CIDCFGR(i),
				     RTC_CIDCFGR_CONF_MASK);
	}

	/* Security RIF configuration */
	seccfgr = rtc_dev.conf_data->sec_conf[0];

	/* Check if all resources must be secured */
	if (seccfgr == RTC_RIF_FULL_SECURED) {
		io_setbits32(base + RTC_SECCFGR, RTC_SECCFGR_FULL_SEC);
		rtc_dev.is_secured = true;

		if (!(io_read32(base + RTC_SECCFGR) & RTC_SECCFGR_FULL_SEC))
			panic("Bad RTC seccfgr configuration");
	}

	/* Shift some values to align with the register */
	shifted_values = (seccfgr & RTC_SECCFGR_VALUES_TO_SHIFT) <<
			 RTC_SECCFGR_SHIFT;
	seccfgr = (seccfgr & RTC_SECCFGR_VALUES) + shifted_values;

	io_clrsetbits32(base + RTC_SECCFGR,
			RTC_SECCFGR_MASK & access_mask_reg, seccfgr);

	/* Privilege RIF configuration */
	privcfgr = rtc_dev.conf_data->priv_conf[0];

	/* Check if all resources must be privileged */
	if (privcfgr == RTC_RIF_FULL_PRIVILEGED) {
		io_setbits32(base + RTC_PRIVCFGR, RTC_PRIVCFGR_FULL_PRIV);

		if (!(io_read32(base + RTC_PRIVCFGR) & RTC_PRIVCFGR_FULL_PRIV))
			panic("Bad RTC privcfgr configuration");
	}

	/* Shift some values to align with the register */
	shifted_values = (privcfgr & RTC_PRIVCFGR_VALUES_TO_SHIFT) <<
			 RTC_PRIVCFGR_SHIFT;
	privcfgr = (privcfgr & RTC_PRIVCFGR_VALUES) + shifted_values;

	io_clrsetbits32(base + RTC_PRIVCFGR,
			RTC_PRIVCFGR_MASK & access_mask_reg, privcfgr);

	if (!is_tdcid)
		return;

	for (i = 0; i < RTC_NB_RIF_RESOURCES; i++) {
		if (!(BIT(i) & rtc_dev.conf_data->access_mask[0]))
			continue;
		/*
		 * When at least one resource has CID filtering enabled,
		 * the RTC_PRIVCFGR_FULL_PRIV and RTC_SECCFGR_FULL_SEC bits are
		 * cleared.
		 */
		io_clrsetbits32(base + RTC_CIDCFGR(i),
				RTC_CIDCFGR_CONF_MASK,
				rtc_dev.conf_data->cid_confs[i]);
	}
}

static void stm32_rtc_clear_events(uint32_t flags)
{
	io_write32(get_base() + RTC_SCR, flags);
}

static enum itr_return stm32_rtc_it_handler(struct itr_handler *h __unused)
{
	vaddr_t rtc_base = get_base();
	uint32_t status = io_read32(rtc_base + RTC_SR);
	uint32_t cr = io_read32(rtc_base + RTC_CR);

	if ((status & RTC_SR_ALRAF) && (cr & RTC_CR_ALRAIE)) {
		DMSG("Alarm occurred");
		/* Clear event's flags */
		stm32_rtc_clear_events(RTC_SCR_CALRAF);
		/*
		 * Notify the caller of 'stm32_rtc_wait_alarm' to re-schedule
		 * the calling thread
		 */
		notif_send_async(rtc_dev.notif_id);
	}

	return ITRR_HANDLED;
}
DECLARE_KEEP_PAGER(stm32_rtc_it_handler);

static TEE_Result parse_dt(const void *fdt, int node)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct dt_node_info dt_info = { };
	const fdt32_t *cuint = NULL;
	uint32_t rif_conf = 0;
	unsigned int i = 0;
	int lenp = 0;

	fdt_fill_device_info(fdt, &dt_info, node);

	rtc_dev.base.pa = dt_info.reg;
	rtc_dev.flags |= RTC_FLAGS_SECURE;
	io_pa_or_va(&rtc_dev.base, dt_info.reg_size);
	assert(rtc_dev.base.va);

	res = clk_dt_get_by_name(fdt, node, "pclk", &rtc_dev.pclk);
	if (res)
		return res;

	res = clk_dt_get_by_name(fdt, node, "rtc_ck", &rtc_dev.rtc_ck);
	if (!rtc_dev.rtc_ck)
		return res;

	if (IS_ENABLED(CFG_RTC_PTA) && IS_ENABLED(CFG_DRIVERS_RTC)) {
		res = interrupt_dt_get(fdt, node, &rtc_dev.itr_chip,
				       &rtc_dev.itr_num);
		if (res != TEE_SUCCESS && res != TEE_ERROR_ITEM_NOT_FOUND)
			return res;
	}

	if (fdt_getprop(fdt, node, "wakeup-source", NULL)) {
		if (interrupt_can_set_wake(rtc_dev.itr_chip))
			rtc_dev.rtc->is_wakeup_source = true;
		else
			DMSG("RTC wakeup source ignored");
	}

	if (!rtc_dev.compat.has_rif_support)
		return TEE_SUCCESS;

	cuint = fdt_getprop(fdt, node, "st,protreg", &lenp);
	if (!cuint) {
		DMSG("No RIF configuration available");
		return TEE_SUCCESS;
	}

	rtc_dev.conf_data = calloc(1, sizeof(*rtc_dev.conf_data));
	if (!rtc_dev.conf_data)
		panic();

	rtc_dev.nb_res = (unsigned int)(lenp / sizeof(uint32_t));
	assert(rtc_dev.nb_res <= RTC_NB_RIF_RESOURCES);

	rtc_dev.conf_data->cid_confs = calloc(RTC_NB_RIF_RESOURCES,
					      sizeof(uint32_t));
	rtc_dev.conf_data->sec_conf = calloc(1, sizeof(uint32_t));
	rtc_dev.conf_data->priv_conf = calloc(1, sizeof(uint32_t));
	rtc_dev.conf_data->access_mask = calloc(1, sizeof(uint32_t));
	if (!rtc_dev.conf_data->cid_confs ||
	    !rtc_dev.conf_data->sec_conf ||
	    !rtc_dev.conf_data->priv_conf ||
	    !rtc_dev.conf_data->access_mask)
		panic("Not enough memory capacity for RTC RIF config");

	for (i = 0; i < rtc_dev.nb_res; i++) {
		rif_conf = fdt32_to_cpu(cuint[i]);

		stm32_rif_parse_cfg(rif_conf, rtc_dev.conf_data,
				    RTC_NB_RIF_RESOURCES);
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_enter_init_mode(void)
{
	vaddr_t base = get_base();
	uint32_t icsr = io_read32(base + RTC_ICSR);
	uint32_t value = 0;

	if (!(icsr & RTC_ICSR_INITF)) {
		icsr |= RTC_ICSR_INIT;
		io_write32(base + RTC_ICSR, icsr);

		if (IO_READ32_POLL_TIMEOUT(base + RTC_ICSR, value,
					   value & RTC_ICSR_INITF,
					   10, TIMEOUT_US_RTC_GENERIC))
			return TEE_ERROR_BUSY;
	}

	return TEE_SUCCESS;
}

static void stm32_rtc_exit_init_mode(void)
{
	io_clrbits32(get_base() + RTC_ICSR, RTC_ICSR_INIT);
}

static TEE_Result stm32_rtc_wait_sync(void)
{
	vaddr_t base = get_base();
	uint32_t value = 0;

	io_clrbits32(base + RTC_ICSR, RTC_ICSR_RSF);

	if (IO_READ32_POLL_TIMEOUT(base + RTC_ICSR, value,
				   value & RTC_ICSR_RSF, 10,
				   TIMEOUT_US_RTC_GENERIC))
		return TEE_ERROR_BUSY;

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_init(void)
{
	unsigned long rate = clk_get_rate(rtc_dev.rtc_ck);
	vaddr_t base = get_base();
	uint32_t pred_a = 0;
	uint32_t pred_s = 0;
	uint32_t prer = io_read32(base + RTC_PRER);
	uint32_t cr = io_read32(base + RTC_CR);
	uint32_t pred_a_max = RTC_PRER_PREDIV_A_MASK >> RTC_PRER_PREDIV_A_SHIFT;
	uint32_t pred_s_max = RTC_PRER_PREDIV_S_MASK >> RTC_PRER_PREDIV_S_SHIFT;
	TEE_Result res = TEE_ERROR_GENERIC;

	if (rate > (pred_a_max + 1) * (pred_s_max + 1))
		panic("rtc_ck rate is too high");

	/*
	 * Compute the prescaler values whom divides the clock in order to get a
	 * 1 Hz output
	 */
	for (pred_a = 0; pred_a <= pred_a_max; pred_a++) {
		pred_s = (rate / (pred_a + 1)) - 1;
		if (pred_s <= pred_s_max &&
		    ((pred_s + 1) * (pred_a + 1)) == rate)
			break;
	}
	/*
	 * Can't find a 1Hz, so give priority to RTC power consumption
	 * by choosing the higher possible value for prediv_a
	 */
	if (pred_s > pred_s_max || pred_a > pred_a_max) {
		pred_a = pred_a_max;
		pred_s = (rate / (pred_a + 1)) - 1;

		DMSG("rtc_ck is %s",
		     (rate < ((pred_a + 1) * (pred_s + 1))) ? "fast" : "slow");
	}

	prer &= RTC_PRER_PREDIV_S_MASK | RTC_PRER_PREDIV_A_MASK;
	pred_s = (pred_s << RTC_PRER_PREDIV_S_SHIFT) & RTC_PRER_PREDIV_S_MASK;
	pred_a = (pred_a << RTC_PRER_PREDIV_A_SHIFT) & RTC_PRER_PREDIV_A_MASK;
	/* quit if there is nothing to initialize */
	if ((cr & RTC_CR_FMT) == 0 && prer == (pred_s | pred_a))
		return TEE_SUCCESS;

	stm32_rtc_write_unprotect();

	res = stm32_rtc_enter_init_mode();
	if (res) {
		EMSG("Can't enter in init mode. Prescaler config failed");
		stm32_rtc_write_protect();
		return TEE_ERROR_BUSY;
	}

	io_write32(base + RTC_PRER, pred_s);
	io_write32(base + RTC_PRER, pred_a | pred_s);

	/* Force 24h time format */
	cr &= ~RTC_CR_FMT;

	io_write32(base + RTC_CR, cr);

	stm32_rtc_exit_init_mode();

	res = stm32_rtc_wait_sync();
	if (res) {
		EMSG("Can't sync RTC. Prescaler config failed");
		stm32_rtc_write_protect();
		return TEE_ERROR_BUSY;
	}

	stm32_rtc_write_protect();

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_get_time(struct rtc *rtc __unused,
				     struct optee_rtc_time *tm)
{
	struct stm32_rtc_calendar cal = { };
	TEE_Result res = TEE_ERROR_GENERIC;

	res = stm32_rtc_get_calendar(&cal);
	if (res)
		return res;

	stm32_rtc_fill_time(&cal, tm);
	stm32_rtc_fill_date(&cal, tm);

	/*
	 * In our RTC we start :
	 * - year at 0
	 * - month at 1
	 * - day at 1
	 * - weekday at Monday = 1
	 */
	tm->tm_mon -= 1;
	tm->tm_wday %= 7;

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_set_time(struct rtc *rtc, struct optee_rtc_time *tm)
{
	vaddr_t rtc_base = get_base();
	uint32_t tr = 0;
	uint32_t dr = 0;
	TEE_Result res = TEE_ERROR_GENERIC;

	/*
	 * In our RTC we start :
	 * - year at 0
	 * - month at 1
	 * - day at 1
	 * - weekday at Monday = 1
	 */
	tm->tm_year -= rtc->range_min.tm_year;
	tm->tm_mon += 1;
	tm->tm_wday = (!tm->tm_wday) ? 7 : tm->tm_wday;

	tr = ((tm->tm_sec % 10) & RTC_TR_SU_MASK) |
	     (((tm->tm_sec / 10) << RTC_TR_ST_SHIFT) & RTC_TR_ST_MASK) |
	     (((tm->tm_min % 10) << RTC_TR_MNU_SHIFT) & RTC_TR_MNU_MASK) |
	     (((tm->tm_min / 10) << RTC_TR_MNT_SHIFT) & RTC_TR_MNT_MASK) |
	     (((tm->tm_hour % 10) << RTC_TR_HU_SHIFT) & RTC_TR_HU_MASK) |
	     (((tm->tm_hour / 10) << RTC_TR_HT_SHIFT) & RTC_TR_HT_MASK);

	dr = ((tm->tm_mday % 10) & RTC_DR_DU_MASK) |
	     (((tm->tm_mday / 10) << RTC_DR_DT_SHIFT) & RTC_DR_DT_MASK) |
	     (((tm->tm_mon % 10) << RTC_DR_MU_SHIFT) & RTC_DR_MU_MASK) |
	     (((tm->tm_mon / 10) << RTC_DR_MT_SHIFT) & RTC_DR_MT_MASK) |
	     ((tm->tm_wday << RTC_DR_WDU_SHIFT) & RTC_DR_WDU_MASK) |
	     (((tm->tm_year % 10) << RTC_DR_YU_SHIFT) & RTC_DR_YU_MASK) |
	     (((tm->tm_year / 10) << RTC_DR_YT_SHIFT) & RTC_DR_YT_MASK);

	stm32_rtc_write_unprotect();

	res = stm32_rtc_enter_init_mode();
	if (res)
		return res;

	io_write32(rtc_base + RTC_TR, tr);
	io_write32(rtc_base + RTC_DR, dr);

	stm32_rtc_exit_init_mode();

	res = stm32_rtc_wait_sync();
	if (res)
		return res;

	stm32_rtc_write_protect();

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_read_alarm(struct rtc *rtc,
				       struct optee_rtc_alarm *alarm)
{
	struct optee_rtc_time *alarm_tm = NULL;
	TEE_Result res = TEE_ERROR_GENERIC;
	struct optee_rtc_time current_tm = { };
	vaddr_t rtc_base = get_base();
	uint32_t alrmar = io_read32(rtc_base + RTC_ALRMAR);
	uint32_t cr = io_read32(rtc_base + RTC_CR);
	uint32_t status = io_read32(rtc_base + RTC_SR);

	alarm_tm = &alarm->time;

	res = stm32_rtc_get_time(rtc, &current_tm);
	if (res)
		return res;

	alarm_tm->tm_year = current_tm.tm_year;
	alarm_tm->tm_mon = current_tm.tm_mon;
	alarm_tm->tm_mday = ((alrmar & RTC_ALRMXR_DATE_UNITS_MASK) >>
			    RTC_ALRMXR_DATE_UNITS_SHIFT) +
			    ((alrmar & RTC_ALRMXR_DATE_TENS_MASK) >>
			    RTC_ALRMXR_DATE_TENS_SHIFT) * 10;
	alarm_tm->tm_hour = ((alrmar & RTC_ALRMXR_HOUR_UNITS_MASK) >>
			    RTC_ALRMXR_HOUR_UNITS_SHIFT) +
			    ((alrmar & RTC_ALRMXR_HOUR_TENS_MASK) >>
			    RTC_ALRMXR_HOUR_TENS_SHIFT) * 10;
	alarm_tm->tm_min = ((alrmar & RTC_ALRMXR_MIN_UNITS_MASK) >>
			    RTC_ALRMXR_MIN_UNITS_SHIFT) +
			   ((alrmar & RTC_ALRMXR_MIN_TENS_MASK) >>
			    RTC_ALRMXR_MIN_TENS_SHIFT) * 10;
	alarm_tm->tm_sec = ((alrmar & RTC_ALRMXR_MIN_UNITS_MASK) >>
			    RTC_ALRMXR_MIN_UNITS_SHIFT) +
			   ((alrmar & RTC_ALRMXR_MIN_TENS_MASK) >>
			    RTC_ALRMXR_MIN_TENS_SHIFT) * 10;

	if (rtc_timecmp(alarm_tm, &current_tm) < 0) {
		if (current_tm.tm_mon == 11) {
			alarm_tm->tm_mon = 0;
			alarm_tm->tm_year += 1;
		} else {
			alarm_tm->tm_mon += 1;
		}
	}

	alarm->enabled = cr & RTC_CR_ALRAE;
	alarm->pending = status & RTC_SR_ALRAF;

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_enable_alarm(struct rtc *rtc __unused, bool enabled)
{
	vaddr_t rtc_base = get_base();

	stm32_rtc_write_unprotect();

	if (enabled)
		io_setbits32(rtc_base + RTC_CR, RTC_CR_ALRAIE | RTC_CR_ALRAE);
	else
		io_clrbits32(rtc_base + RTC_CR, RTC_CR_ALRAIE | RTC_CR_ALRAE);

	stm32_rtc_clear_events(RTC_SCR_CALRAF);

	stm32_rtc_write_protect();

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_valid_alarm_time(struct optee_rtc_time *tm)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct stm32_rtc_calendar cal = { };
	struct optee_rtc_time max = { };
	uint32_t next_month = 0;
	uint32_t next_year = 0;

	/*
	 * Assuming current date is M-D-Y H:M:S.
	 * RTC alarm can't be set on a specific month and year.
	 * So the valid alarm range is:
	 *	M-D-Y H:M:S < alarm <= (M+1)-D-Y H:M:S
	 */
	res = stm32_rtc_get_calendar(&cal);
	if (res)
		return res;

	stm32_rtc_fill_time(&cal, &max);
	stm32_rtc_fill_date(&cal, &max);
	/*
	 * Hardware month is from 1 to 12
	 * align with tm_mon for comparison with alarm time
	 */
	max.tm_mon -= 1;

	/*
	 * Don't allow alarm to be set in the past.
	 */
	if (rtc_timecmp(&max, tm) > 0)
		return TEE_ERROR_BAD_PARAMETERS;

	/*
	 * Find the next month and the year of the next month.
	 * Note: tm_mon and next_month are from 0 to 11
	 */
	next_month = max.tm_mon + 1;
	if (next_month == 12) {
		next_month = 0;
		next_year = max.tm_year + 1;
	} else {
		next_year = max.tm_year;
	}

	if (max.tm_mday < tm->tm_mday) {
		/*
		 * Alarm is next month
		 * Must be sure it does not exceed the number of days in
		 * the next month.
		 */
		if (tm->tm_mday > rtc_get_month_days(next_month, next_year))
			return TEE_ERROR_BAD_PARAMETERS;
	}

	max.tm_year = next_year;
	max.tm_mon = next_month;

	if (rtc_timecmp(&max, tm) < 0)
		return TEE_ERROR_BAD_PARAMETERS;

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_set_alarm(struct rtc *rtc,
				      struct optee_rtc_alarm *alarm)
{
	struct optee_rtc_time *alarm_time = NULL;
	vaddr_t rtc_base = get_base();
	TEE_Result res = TEE_ERROR_GENERIC;

	uint32_t alrmar = 0;
	uint32_t cr = io_read32(rtc_base + RTC_CR);

	alarm_time = &alarm->time;

	res = stm32_rtc_valid_alarm_time(alarm_time);
	if (res)
		return res;

	alrmar = 0;
	/* tm_year and tm_mon are not used because not supported by RTC */
	alrmar |= ((alarm_time->tm_mday / 10) << RTC_ALRMXR_DATE_TENS_SHIFT) &
		  RTC_ALRMXR_DATE_TENS_MASK;
	alrmar |= ((alarm_time->tm_mday % 10) << RTC_ALRMXR_DATE_UNITS_SHIFT) &
		  RTC_ALRMXR_DATE_UNITS_MASK;
	/* 24-hour format */
	alrmar &= ~RTC_ALRMXR_PM;
	alrmar |= ((alarm_time->tm_hour / 10) << RTC_ALRMXR_HOUR_TENS_SHIFT) &
		  RTC_ALRMXR_HOUR_TENS_MASK;
	alrmar |= ((alarm_time->tm_hour % 10) << RTC_ALRMXR_HOUR_UNITS_SHIFT) &
		  RTC_ALRMXR_HOUR_UNITS_MASK;
	alrmar |= ((alarm_time->tm_min / 10) << RTC_ALRMXR_MIN_TENS_SHIFT) &
		  RTC_ALRMXR_MIN_TENS_MASK;
	alrmar |= ((alarm_time->tm_min % 10) << RTC_ALRMXR_MIN_UNITS_SHIFT) &
		  RTC_ALRMXR_MIN_UNITS_MASK;
	alrmar |= ((alarm_time->tm_sec / 10) << RTC_ALRMXR_SEC_TENS_SHIFT) &
		  RTC_ALRMXR_SEC_TENS_MASK;
	alrmar |= ((alarm_time->tm_sec % 10) << RTC_ALRMXR_SEC_UNITS_SHIFT) &
		  RTC_ALRMXR_SEC_UNITS_MASK;

	stm32_rtc_write_unprotect();

	/* Disable Alarm */
	cr &= ~RTC_CR_ALRAE;
	io_write32(rtc_base + RTC_CR, cr);

	io_write32(rtc_base + RTC_ALRMAR, alrmar);

	stm32_rtc_enable_alarm(rtc, alarm->enabled);

	stm32_rtc_write_protect();

	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_cancel_wait_alarm(struct rtc *rtc __unused)
{
	rtc_dev.wait_alarm_return_status = RTC_WAIT_ALARM_CANCELED;
	notif_send_async(rtc_dev.notif_id);

	return TEE_SUCCESS;
}

static TEE_Result
stm32_rtc_wait_alarm(struct rtc *rtc __unused,
		     enum rtc_wait_alarm_status *return_status)
{
	TEE_Result res = TEE_ERROR_GENERIC;

	rtc_dev.wait_alarm_return_status = RTC_WAIT_ALARM_RESET;

	/* Wait until a notification arrives - blocking */

	res = notif_wait(rtc_dev.notif_id);
	if (res)
		return res;

	if (rtc_dev.wait_alarm_return_status ==
		RTC_WAIT_ALARM_CANCELED) {
		*return_status = RTC_WAIT_ALARM_CANCELED;
		stm32_rtc_enable_alarm(rtc, 0);
	} else {
		*return_status = RTC_WAIT_ALARM_ALARM_OCCURRED;
	}
	return TEE_SUCCESS;
}

static TEE_Result stm32_rtc_set_alarm_wakeup_status(struct rtc *rtc __unused,
						    bool status)
{
	if (!rtc_dev.rtc->is_wakeup_source)
		return TEE_ERROR_NOT_SUPPORTED;

	interrupt_set_wake(rtc_dev.itr_chip, rtc_dev.itr_num, status);

	return TEE_SUCCESS;
}

static const struct rtc_ops stm32_rtc_ops = {
	.get_time = stm32_rtc_get_time,
	.set_time = stm32_rtc_set_time,
	.read_alarm = stm32_rtc_read_alarm,
	.set_alarm = stm32_rtc_set_alarm,
	.enable_alarm = stm32_rtc_enable_alarm,
	.wait_alarm = stm32_rtc_wait_alarm,
	.cancel_wait = stm32_rtc_cancel_wait_alarm,
	.set_alarm_wakeup_status = stm32_rtc_set_alarm_wakeup_status,
};

static struct rtc stm32_rtc = {
	.ops = &stm32_rtc_ops,
	.range_min = { 2000, 1, 1, 0, 0, 0, 0 },
	.range_max = { 2099, 12, 31, 23, 59, 59, 0 },
};

static TEE_Result stm32_rtc_probe(const void *fdt, int node,
				  const void *compat_data)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	bool is_tdcid = false;

	rtc_dev.compat = *(struct rtc_compat *)compat_data;
	rtc_dev.rtc = &stm32_rtc;

	if (rtc_dev.compat.has_rif_support) {
		res = stm32_rifsc_check_tdcid(&is_tdcid);
		if (res)
			return res;
	}

	res = parse_dt(fdt, node);
	if (res) {
		memset(&rtc_dev, 0, sizeof(rtc_dev));
		return res;
	}

	/* Unbalanced clock enable to ensure RTC core clock is always on */
	res = clk_enable(rtc_dev.rtc_ck);
	if (res)
		panic("Couldn't enable RTC clock");

	if (clk_get_rate(rtc_dev.pclk) < (clk_get_rate(rtc_dev.rtc_ck) * 7))
		rtc_dev.flags |= RTC_FLAGS_READ_TWICE;

	if (rtc_dev.compat.has_rif_support) {
		res = clk_enable(rtc_dev.pclk);
		if (res)
			panic("Could not enable RTC bus clock");

		apply_rif_config(is_tdcid);

		/*
		 * Verify if applied RIF config will not disable
		 * other functionnalities of this driver.
		 */
		res = check_rif_config();
		if (res)
			panic("Incompatible RTC RIF configuration");

		clk_disable(rtc_dev.pclk);
	}

	if (IS_ENABLED(CFG_RTC_PTA) && IS_ENABLED(CFG_DRIVERS_RTC) &&
	    IS_ENABLED(CFG_CORE_ASYNC_NOTIF) && rtc_dev.is_secured) {
		res = notif_alloc_async_value(&rtc_dev.notif_id);
		if (res)
			return res;

		if (!rtc_dev.itr_chip)
			return TEE_ERROR_GENERIC;

		res = interrupt_create_handler(rtc_dev.itr_chip,
					       rtc_dev.itr_num,
					       stm32_rtc_it_handler,
					       &rtc_dev, 0,
					       &rtc_dev.itr_handler);
		if (res)
			return res;

		/* Unbalanced clock enable to ensure IRQ interface is alive */
		res = clk_enable(rtc_dev.pclk);
		if (res)
			return res;

		res = stm32_rtc_init();
		if (res)
			return res;

		rtc_register(&stm32_rtc);
		interrupt_enable(rtc_dev.itr_chip, rtc_dev.itr_num);
	}

	return res;
}

static struct rtc_compat mp25_compat = {
	.has_seccfgr = true,
	.has_rif_support = true,
};

static struct rtc_compat mp15_compat = {
	.has_seccfgr = false,
	.has_rif_support = false,
};

static struct rtc_compat mp13_compat = {
	.has_seccfgr = true,
	.has_rif_support = false,
};

static const struct dt_device_match stm32_rtc_match_table[] = {
	{
		.compatible = "st,stm32mp25-rtc",
		.compat_data = &mp25_compat,
	},
	{
		.compatible = "st,stm32mp1-rtc",
		.compat_data = &mp15_compat,
	},
	{
		.compatible = "st,stm32mp13-rtc",
		.compat_data = &mp13_compat,
	},
	{ }
};

DEFINE_DT_DRIVER(stm32_rtc_dt_driver) = {
	.name = "stm32-rtc",
	.match_table = stm32_rtc_match_table,
	.probe = stm32_rtc_probe,
};
