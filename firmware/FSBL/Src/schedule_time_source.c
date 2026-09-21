/* schedule_time_source.c - the clock behind the dose schedule.
 * See schedule_time_source.h for the design and the two modes.
 *
 * PERIPHERAL BRING-UP NOTE. This project has
 * no.ioc and STM32CubeMX cannot be used, so the RTC is configured by hand
 * here. Three things about the RTC's backup domain that look exactly like a
 * dead peripheral if you get them wrong, in the order they bite:
 *
 *   1. The backup domain is WRITE PROTECTED after reset. Every register in
 *      it - the RTC itself, the backup registers, the LSE control bits -
 *      ignores writes until HAL_PWR_EnableBkUpAccess() is called. There is
 *      no error; the writes simply do not land.
 *   2. The RTC needs a clock SOURCE selected through the RCC extended
 *      peripheral-clock API, and that selection is itself in the backup
 *      domain, so it has to happen after (1).
 *   3. LSE is a crystal. It can take a second to start and it can fail to
 *      start at all on a board where the crystal is not fitted or is
 *      marginal. HAL_RCC_OscConfig() blocks on it and then returns an error,
 *      and the natural reaction is to treat the RTC as broken. It is not -
 *      LSI is right there. This file tries LSE, and falls back to LSI with a
 *      line in the log saying which one it got, because "which oscillator is
 *      behind the clock" is exactly the kind of fact that should never have
 *      to be re-derived on a bench at midnight.
 *
 * This is the same lesson as the VddIO finding, one layer
 * out: a power/enable precondition that produces silence rather than an
 * error.
 */

#include "schedule_time_source.h"
#include "ms_osal.h"          /* The RTC read mutex - see below */
#include "stm32n6xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ── The marker that says "a human has set this clock" ────────────────────
 *
 * Backup register 0 holds a magic word. It is in the always-on domain, so it
 * survives every reset and only a genuine power removal clears it. That is
 * precisely the property that made the cold-boot display bug invisible for
 * a long time; here the same property is the
 * feature - a carer sets the clock once and the device remembers across
 * reboots.
 *
 * The value is arbitrary but must not be 0 or 0xFFFFFFFF, the two values an
 * uninitialised or erased backup register is most likely to hold. */
#define TIME_SET_MARKER_REG   RTC_BKP_DR0
#define TIME_SET_MARKER       0x4D533135u   /* "MS15" */

static RTC_HandleTypeDef s_hrtc;
static bool              s_rtc_up      = false;
static const char       *s_osc_name    = "none";
static char              s_mode_str[40];

/* ── Serialising access to the calendar ───────────────────────────────────
 *
 * Reading the RTC calendar is a PAIR of calls, not one: HAL_RTC_GetTime()
 * locks the shadow registers and HAL_RTC_GetDate() unlocks them. Two tasks
 * interleaving that pair can hand one of them a date belonging to the
 * other's read.
 *
 * The RTC used to have exactly one reader, and this was theoretical.
 * It is not any more: the logger task (priority 2) stamps every log line
 * while the UI task (priority 4) reads the clock to draw it and to schedule.
 * The UI preempts the logger by construction, so the interleaving is the
 * normal case rather than an unlucky one.
 *
 * This is also the OSAL's mutex primitive finding its first real consumer.
 * It has been in ms_osal.h from the start, and nothing in the codebase
 * used it until now.
 *
 * The timeout is short and failure is non-fatal: a log line with a slightly
 * odd timestamp is a far better outcome than a logger task blocked behind a
 * UI redraw. */
static osal_mutex_handle_t s_rtc_mtx = NULL;

#if MEDSIGHT_FAST_CLOCK
/* ── The demo clock's anchor ──────────────────────────────────────────────
 *
 * The compressed day runs FORWARD FROM the last time the clock was set,
 * rather than from an arbitrary point in the tick counter. Without this a
 * carer sets 04:00, the screen carries on reading whatever the free-running
 * axis happened to be, and the setting appears to have been ignored - which
 * is exactly what was reported from the bench.
 *
 * s_demo_anchor_min is the minute-of-day that was set; s_demo_anchor_tick is
 * HAL_GetTick() at that instant. Everything else is arithmetic. */
static uint16_t s_demo_anchor_min  = 0u;
static uint32_t s_demo_anchor_tick = 0u;

/* Simulated seconds since midnight, 0..86399. One place, so the minute the
 * scheduler acts on and the seconds the log stamps can never disagree. */
static uint32_t demo_second_of_day(void)
{
    uint32_t elapsed_ms  = HAL_GetTick() - s_demo_anchor_tick;
    uint32_t day_ms      = MEDSIGHT_FAST_DAY_SECONDS * 1000u;
    /* Real ms -> simulated seconds: a whole simulated day (86400 s) passes
     * in day_ms of real time. */
    uint32_t elapsed_sim = (uint32_t)(((uint64_t)elapsed_ms * 86400u) / day_ms);
    return (uint32_t)((((uint32_t)s_demo_anchor_min * 60u) + elapsed_sim) % 86400u);
}
#endif

void time_source_service_init(void)
{
    s_rtc_mtx = osal_mutex_create();
}

/* Returns whether the lock was actually taken, so the unlock can match.
 * Deliberately tolerant of a NULL handle: time_source_init() and the very
 * first reads happen in main(), before the scheduler exists, where there is
 * nothing to serialise against. */
static bool rtc_lock(void)
{
    if (s_rtc_mtx == NULL) return false;
    return osal_mutex_lock(s_rtc_mtx, 50u);
}
static void rtc_unlock(bool held)
{
    if (held) osal_mutex_unlock(s_rtc_mtx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Bring-up
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Try LSE, fall back to LSI. Returns the RCC_RTCCLKSOURCE_* actually
 * obtained, or 0 if neither oscillator would start. */
static uint32_t select_rtc_clock_source(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    /* --- attempt 1: LSE, the 32.768 kHz crystal on the DK ---------------- */
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState       = RCC_LSE_ON;
    if (HAL_RCC_OscConfig(&osc) == HAL_OK) {
        pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        pclk.RTCClockSelection    = RCC_RTCCLKSOURCE_LSE;
        if (HAL_RCCEx_PeriphCLKConfig(&pclk) == HAL_OK) {
            s_osc_name = "LSE (32.768 kHz crystal)";
            return RCC_RTCCLKSOURCE_LSE;
        }
    }

    /* --- attempt 2: LSI, always present, less accurate -------------------
     * Drifts by a percent or so, which over a day is minutes. For a dose
     * REMINDER that is immaterial; it would not be for anything that had to
     * agree with an outside clock. Recorded in the technical reference sibling
     * discussion rather than silently accepted. */
    memset(&osc, 0, sizeof(osc));
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
    osc.LSIState       = RCC_LSI_ON;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return 0u;
    }
    memset(&pclk, 0, sizeof(pclk));
    pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    pclk.RTCClockSelection    = RCC_RTCCLKSOURCE_LSI;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return 0u;
    }
    s_osc_name = "LSI (internal, +/- ~1%)";
    return RCC_RTCCLKSOURCE_LSI;
}

bool time_source_init(void)
{
    if (s_rtc_up) {
        return true;
    }

    /* (1) Unlock the backup domain. Nothing below this line would take
     *     effect without it, and nothing would report an error either. */
    HAL_PWR_EnableBkUpAccess();

    /* (2) Pick and enable an oscillator, and route it to the RTC. */
    uint32_t src = select_rtc_clock_source();
    if (src == 0u) {
        printf("time_source: no low-speed oscillator would start - "
               "scheduling is UNAVAILABLE, on-demand dispensing still works.\r\n");
        return false;
    }

    __HAL_RCC_RTCAPB_CLK_ENABLE();   /* CPU-side register interface */
    __HAL_RCC_RTC_ENABLE();          /* the counter itself          */

    /* (3) Prescalers. Both oscillators are nominally 32 kHz-ish, and the
     *     product (async+1)*(sync+1) must equal the input frequency for the
     *     calendar to tick at 1 Hz:
     *         LSE 32768 = 128 * 256   -> 127 / 255
     *         LSI 32000 = 128 * 250   -> 127 / 249
     *     Getting this wrong does not fail to build or init; the clock just
     *     runs at the wrong speed, which is a genuinely unpleasant bug to
     *     find from a schedule that fires early. */
    s_hrtc.Instance            = RTC;
    s_hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
    s_hrtc.Init.AsynchPrediv   = 127u;
    s_hrtc.Init.SynchPrediv    = (src == RCC_RTCCLKSOURCE_LSE) ? 255u : 249u;
    s_hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
    s_hrtc.Init.OutPutRemap    = RTC_OUTPUT_REMAP_NONE;
    s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    s_hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
    s_hrtc.Init.OutPutPullUp   = RTC_OUTPUT_PULLUP_NONE;
    s_hrtc.Init.BinMode        = RTC_BINARY_NONE;

    if (HAL_RTC_Init(&s_hrtc) != HAL_OK) {
        printf("time_source: HAL_RTC_Init FAILED - scheduling is UNAVAILABLE.\r\n");
        return false;
    }

    s_rtc_up = true;

#if MEDSIGHT_FAST_CLOCK
    /* On a board whose clock was already set in a previous run, start the
     * compressed day from the RTC's wall-clock time rather than from zero -
     * so a reboot does not silently move the demo clock. */
    {
        ms_datetime_t boot;
        if (time_source_get(&boot)) {
            s_demo_anchor_min = (uint16_t)(((uint32_t)boot.hour * 60u) + boot.minute);
        }
        s_demo_anchor_tick = HAL_GetTick();
    }
#endif

    snprintf(s_mode_str, sizeof(s_mode_str),
#if MEDSIGHT_FAST_CLOCK
             "DEMO (1 day = %us)", (unsigned)MEDSIGHT_FAST_DAY_SECONDS
#else
             "RTC (wall clock)"
#endif
             );

    char now[40];
    time_source_format_now(now, sizeof(now));
    printf("time_source: RTC up on %s, mode %s, clock %s (%s).\r\n",
           s_osc_name, s_mode_str,
           time_source_is_valid() ? "SET" : "NOT SET - carer must set it",
           now);
    return true;
}

bool time_source_is_valid(void)
{
    if (!s_rtc_up) {
        return false;
    }
    return (HAL_RTCEx_BKUPRead(&s_hrtc, TIME_SET_MARKER_REG) == TIME_SET_MARKER);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Reading and setting
 * ═══════════════════════════════════════════════════════════════════════════ */

bool time_source_get(ms_datetime_t *out)
{
    if (!s_rtc_up || (out == NULL)) {
        return false;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    /* HAL_RTC_GetTime MUST be called before HAL_RTC_GetDate: reading the
     * time register locks the calendar shadow registers, and reading the
     * date is what unlocks them. Reversing the order returns a date that can
     * be one day stale across midnight. This is an ST HAL contract, not a
     * style preference - and it is exactly why the pair is serialised. */
    bool held = rtc_lock();

    if (HAL_RTC_GetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        rtc_unlock(held);
        return false;
    }
    if (HAL_RTC_GetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        rtc_unlock(held);
        return false;
    }
    rtc_unlock(held);

    out->year   = (uint16_t)(2000u + d.Year);
    out->month  = d.Month;
    out->day    = d.Date;
    out->hour   = t.Hours;
    out->minute = t.Minutes;
    out->second = t.Seconds;
    return true;
}

bool time_source_set(const ms_datetime_t *dt)
{
    if (!s_rtc_up || (dt == NULL)) {
        return false;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    t.Hours          = dt->hour;
    t.Minutes        = dt->minute;
    t.Seconds        = dt->second;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    d.Year  = (uint8_t)((dt->year >= 2000u) ? (dt->year - 2000u) : 0u);
    d.Month = dt->month;
    d.Date  = dt->day;
    /* WeekDay is required by the HAL but this firmware never uses it - the
     * schedule is the same every day. RTC_WEEKDAY_MONDAY is a valid value,
     * not a claim about the actual day. */
    d.WeekDay = RTC_WEEKDAY_MONDAY;

    bool held = rtc_lock();

    if (HAL_RTC_SetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        rtc_unlock(held);
        return false;
    }
    if (HAL_RTC_SetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        rtc_unlock(held);
        return false;
    }

    HAL_RTCEx_BKUPWrite(&s_hrtc, TIME_SET_MARKER_REG, TIME_SET_MARKER);
    rtc_unlock(held);

#if MEDSIGHT_FAST_CLOCK
    /* Re-anchor the compressed day on what was just set, so the demo clock
     * reads the carer's time and then runs fast from it. */
    s_demo_anchor_min  = (uint16_t)(((uint32_t)dt->hour * 60u) + dt->minute);
    s_demo_anchor_tick = HAL_GetTick();
#endif

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * The scheduling axis - the ONE place the two modes differ
 * ═══════════════════════════════════════════════════════════════════════════ */

uint16_t time_source_minute_of_day(void)
{
#if MEDSIGHT_FAST_CLOCK
    /* Driven from the millisecond tick rather than the RTC, because the
     * RTC's resolution is one second and one real second is a whole
     * simulated minute here - reading the RTC would make the schedule move
     * in one-minute jumps with nothing in between for the UI to show.
     *
     * Anchored on the last time the clock was SET (see demo_second_of_day),
     * so a carer's chosen time is honoured rather than ignored.
     *
     * HAL_GetTick() wraps after 49 days; in a demo build that costs at most
     * one mistimed window. Stated rather than fixed, because the real build
     * does not have this property at all. */
    return (uint16_t)(demo_second_of_day() / 60u);
#else
    ms_datetime_t now;
    if (!time_source_get(&now)) {
        return 0u;
    }
    return (uint16_t)((uint32_t)now.hour * 60u + now.minute);
#endif
}

uint32_t time_source_minute_ms(void)
{
#if MEDSIGHT_FAST_CLOCK
    return (MEDSIGHT_FAST_DAY_SECONDS * 1000u) / MINUTES_PER_DAY;
#else
    return 60000u;
#endif
}

uint32_t time_source_ms_until(uint16_t target_minute_of_day)
{
    uint16_t now    = time_source_minute_of_day();
    uint16_t target = (uint16_t)(target_minute_of_day % MINUTES_PER_DAY);

    /* Minutes forward to the next occurrence. A target equal to "now" means
     * a whole day, not zero - see the header. Without that, re-arming the
     * alarm the instant a window fires would fire it again immediately and
     * spin. */
    uint32_t minutes = (target > now)
                       ? (uint32_t)(target - now)
                       : (uint32_t)(MINUTES_PER_DAY - (now - target));

    uint32_t ms = minutes * time_source_minute_ms();
    /* Never hand a zero delay to an alarm: µT-Kernel's tk_sta_alm() treats
     * 0 as "fire on the next tick", which is legal but turns a scheduling
     * bug into a busy loop through the handler. One minute is the smallest
     * meaningful unit on this axis. */
    if (ms == 0u) {
        ms = time_source_minute_ms();
    }
    return ms;
}

const char *time_source_mode(void)
{
    return (s_mode_str[0] != '\0') ? s_mode_str : "uninitialised";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Formatting
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *const MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

void time_source_format_now(char *buf, size_t n)
{
    if ((buf == NULL) || (n == 0u)) {
        return;
    }
    ms_datetime_t now;
    if (!time_source_get(&now)) {
        snprintf(buf, n, "--- clock not running ---");
        return;
    }
    const char *mon = (now.month >= 1u && now.month <= 12u)
                      ? MONTHS[now.month - 1u] : "???";
#if MEDSIGHT_FAST_CLOCK
    /* In demo mode the hours/minutes shown come from the compressed axis,
     * not the RTC, so what is on screen matches what the schedule is acting
     * on. Showing the real RTC time here would be a second clock disagreeing
     * with the one the device is using, which is worse than a fast one. */
    uint16_t mod = time_source_minute_of_day();
    snprintf(buf, n, "%u %s %u  %02u:%02u",
             (unsigned)now.day, mon, (unsigned)now.year,
             (unsigned)(mod / 60u), (unsigned)(mod % 60u));
#else
    snprintf(buf, n, "%u %s %u  %02u:%02u",
             (unsigned)now.day, mon, (unsigned)now.year,
             (unsigned)now.hour, (unsigned)now.minute);
#endif
}

void time_source_format_hhmm(char *buf, size_t n, uint16_t minute_of_day)
{
    if ((buf == NULL) || (n == 0u)) {
        return;
    }
    uint16_t m = (uint16_t)(minute_of_day % MINUTES_PER_DAY);
    snprintf(buf, n, "%02u:%02u", (unsigned)(m / 60u), (unsigned)(m % 60u));
}

void time_source_format_stamp(char *buf, size_t n)
{
    if ((buf == NULL) || (n == 0u)) {
        return;
    }
    buf[0] = '\0';

    /* No clock, no stamp. See the header: an unstamped line is honest, and
     * a line stamped 2000-01-01 looks like data. */
    if (!time_source_is_valid()) {
        return;
    }

    ms_datetime_t now;
    if (!time_source_get(&now)) {
        return;
    }

#if MEDSIGHT_FAST_CLOCK
    /* Demo builds take the time of day from the compressed axis, so the log
     * and the screen tell the same story. Same single source as the minute
     * the scheduler acts on, so the two can never disagree. */
    uint32_t sim_sec = demo_second_of_day();
    snprintf(buf, n, "%04u-%02u-%02u %02u:%02u:%02u ",
             (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
             (unsigned)(sim_sec / 3600u),
             (unsigned)((sim_sec / 60u) % 60u),
             (unsigned)(sim_sec % 60u));
#else
    snprintf(buf, n, "%04u-%02u-%02u %02u:%02u:%02u ",
             (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
             (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);
#endif
}
