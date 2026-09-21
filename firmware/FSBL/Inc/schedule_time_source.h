/* schedule_time_source.h - the clock behind the dose schedule
 *
 * the technical reference has listed this module as "DESIGNED, NOT YET
 * WRITTEN" for a long time. Nothing needed the time of day until carer mode
 * and scheduled dosing arrived, so it was never built. It is built now, to
 * the design that was already documented:
 *
 *   "the prototype uses a fast, configurable countdown timer standing in for
 *    real wall-clock scheduling... The real RTC-driven daily schedule is the
 *    intended final behavior - implemented as a swappable time source (same
 *    pattern as the OSAL: one clearly isolated point of substitution)."
 *                                          - the project plan
 *
 * THE SUBSTITUTION POINT IS THE DELIVERABLE, not the timer. Everything above
 * this header - state_machine.c, carer_ui.c - asks "what minute of the day is
 * it?" and "how long until HH:MM?" and never learns which of the two backends
 * answered. That is the same bargain ms_osal.h made with FreeRTOS and
 * µT-Kernel, and it is why the demo can compress a whole day into
 * MEDSIGHT_FAST_DAY_SECONDS without a single conditional anywhere else in
 * the firmware.
 *
 * ── The two modes ────────────────────────────────────────────────────────
 *
 * MEDSIGHT_FAST_CLOCK == 0  (default; the real behaviour)
 *     Wall clock straight off the STM32N6's internal RTC, running from LSE
 *     if the board's 32.768 kHz crystal starts and LSI otherwise. The RTC is
 *     in the backup power domain, so the clock and the "has it ever been
 *     set" marker both survive a reset - which is the entire point. A
 *     schedule that forgets itself at every power cycle is not a schedule.
 *
 * MEDSIGHT_FAST_CLOCK == 1  (the demo)
 *     A whole 24-hour day is compressed into MEDSIGHT_FAST_DAY_SECONDS of
 *     real time, so a dose window opening, being met, and a later one being
 *     missed can all be filmed inside one take. The date still comes from
 *     the RTC; only the time-of-day axis is scaled. Nothing else changes:
 *     the same alarm, the same event flag, the same log lines.
 *
 * ── The scheduling axis ──────────────────────────────────────────────────
 *
 * Everything schedule-related in this firmware is expressed as a MINUTE OF
 * DAY, 0..1439. It is small enough to store four of them per patient in a
 * record that has to fit on an SD card, it has no timezone and no DST, and
 * it is exactly the granularity a medication schedule is written in. Seconds
 * appear only where a human reads a clock.
 */
#ifndef SCHEDULE_TIME_SOURCE_H
#define SCHEDULE_TIME_SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Build switch: which backend is behind the interface ─────────────────── */
#ifndef MEDSIGHT_FAST_CLOCK
#define MEDSIGHT_FAST_CLOCK   0
#endif

/* How much real time one compressed "day" takes in demo mode.
 *
 * 1440 s (24 real minutes) is chosen so that **one simulated minute is
 * exactly one real second**. That is not an aesthetic choice, it is the only
 * ratio that makes the feature testable by a person:
 *
 *   - A dose window is DOSE_WINDOW_MINUTES (30) SIMULATED minutes, so it
 *     lasts 30 real seconds - enough to walk to the device and tap through
 *     the instruct / preview / capture / dispense / confirm screens, which
 *     take fifteen to twenty-five seconds of real tapping.
 *   - Setting a dose two simulated minutes ahead means a two second wait.
 *   - Letting one lapse costs half a minute, not half an hour.
 *
 * The first draft used 240 s, which made one simulated minute 1/6 of a real
 * second and a whole dose window FIVE real seconds - physically impossible
 * to complete a dispense inside. The window has to be longer than the flow
 * it is a window for; that is the constraint this number comes from.
 *
 * Nobody ever waits a whole simulated day: you wait until the next dose. So
 * the day length only matters through this ratio, and "minutes on screen =
 * seconds in the room" is also the easiest thing to explain to whoever is
 * holding the camera. */
#ifndef MEDSIGHT_FAST_DAY_SECONDS
#define MEDSIGHT_FAST_DAY_SECONDS   1440u
#endif

#define MINUTES_PER_DAY   1440u

/* ── The one value the rest of the firmware exchanges with this module ──── */
typedef struct {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1..12 */
    uint8_t  day;      /* 1..31 */
    uint8_t  hour;     /* 0..23 */
    uint8_t  minute;   /* 0..59 */
    uint8_t  second;   /* 0..59 */
} ms_datetime_t;

/**
 * @brief Bring up the backing hardware. Call once from main(), before the
 *        scheduler, alongside the other peripheral inits.
 *
 * Safe to call on a board whose clock has never been set: it brings the RTC
 * up and leaves time_source_is_valid() false until a carer sets it. Failure
 * here is reported and survivable - the device keeps dispensing on demand,
 * it just cannot schedule.
 *
 * @return true if the RTC is running.
 */
bool time_source_init(void);

/** @brief Has the clock ever been set on this device?
 *
 * Backed by an RTC backup register, which lives in the always-on domain and
 * is not cleared by a system reset - the same property
 * the cold-boot lesson is about, used deliberately here
 * rather than tripped over. Only removing VDD clears it. */
bool time_source_is_valid(void);

/** @brief Read the current date and time. False if the RTC is not running. */
bool time_source_get(ms_datetime_t *out);

/** @brief Set the clock, and mark it as having been set. Carer mode only. */
bool time_source_set(const ms_datetime_t *dt);

/** @brief The scheduling axis: 0..1439. In demo mode this is the compressed
 *         day, so it runs through all 1440 values every
 *         MEDSIGHT_FAST_DAY_SECONDS. Returns 0 if the clock is not set -
 *         callers must check time_source_is_valid() before scheduling. */
uint16_t time_source_minute_of_day(void);

/** @brief Real milliseconds from now until `target` minute-of-day next
 *         occurs. Never returns 0 for "right now" - a target equal to the
 *         current minute means the next occurrence, a whole day away, which
 *         is what makes a re-arm after firing land on tomorrow rather than
 *         immediately re-firing. */
uint32_t time_source_ms_until(uint16_t target_minute_of_day);

/** @brief Real milliseconds that one scheduled minute lasts (60000 in real
 *         mode, MEDSIGHT_FAST_DAY_SECONDS*1000/1440 in demo mode). Lets a
 *         caller express a window length in schedule minutes and get a
 *         timeout in the units the RTOS wants. */
uint32_t time_source_minute_ms(void);

/** @brief "RTC (wall clock)" or "DEMO (1 day = Ns)", for the UI and the log. */
const char *time_source_mode(void);

/** @brief "10 Sep 2026  14:32" into buf. Always writes something. */
void time_source_format_now(char *buf, size_t n);

/** @brief "14:32" for an arbitrary minute-of-day. Always writes something. */
void time_source_format_hhmm(char *buf, size_t n, uint16_t minute_of_day);

/**
 * @brief Sortable log stamp: "2026-09-10 16:39:02 ", or an EMPTY string if
 *        the clock has never been set.
 *
 * The empty-string case is the point, not an edge case. Before a carer sets
 * the clock the device genuinely does not know what time it is, and a log
 * line stamped "2000-01-01 00:04" would be worse than an unstamped one - it
 * looks like data. So the stamp appears exactly when it means something, and
 * a reader can tell the two eras apart at a glance.
 *
 * ISO order (year-month-day) rather than the human "10 Sep 2026" the UI
 * shows, because this one is read in a text file where sorting and grepping
 * matter more than reading aloud.
 *
 * In demo mode the time-of-day comes from the COMPRESSED axis, so the log
 * agrees with what is on the screen. A demo build's stamps are not wall-clock
 * times and are not meant to be.
 */
void time_source_format_stamp(char *buf, size_t n);

/**
 * @brief Create the RTC access mutex. Call once from main(), alongside the
 *        other osal_*_create() calls, BEFORE osal_scheduler_start().
 *
 * WHY THIS EXISTS. Reading the RTC calendar is not one register read: the
 * ST HAL requires HAL_RTC_GetTime() before HAL_RTC_GetDate(), because the
 * first locks the calendar shadow registers and the second unlocks them.
 * Two tasks interleaving that pair can hand one of them a date that belongs
 * to the other's read.
 *
 * That was theoretical until the RTC got a second reader: the
 * logger task (priority 2) now stamps every line, while the UI task
 * (priority 4) reads the clock to draw it and to schedule. The UI preempts
 * the logger by construction, so the interleaving is not unlikely - it is
 * the normal case.
 *
 * Worth noting for the record: this is the FIRST genuine consumer of the
 * OSAL's mutex primitive. It existed from the start and
 * nothing in the codebase used it until now. It has
 * something to protect now.
 */
void time_source_service_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULE_TIME_SOURCE_H */
