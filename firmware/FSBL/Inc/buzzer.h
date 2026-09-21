/* buzzer.h - MedSight audible alerts
 *
 * THE PROGRAM PLAN'S FOURTH BOX. the Program Plan section 5 lists an
 * "Alert / Feedback Module: Buzzer, LED, optional small display output", and
 * section 6 shows the Alert Task as the fourth stage of Camera -> Inference ->
 * Validation -> Alert, driving "a buzzer and LED (green = correct, red =
 * incorrect/missed)". The device has had no audible feedback at all until
 * now, so this closes a commitment from our own submitted plan rather than
 * adding a new idea.
 *
 * AN ACTIVE PIEZO BUZZER, not a passive one. The plan
 * recommended passive on a timer PWM channel because different pitches let a
 * confirmation chirp sound unlike a missed-dose alert. The project owner has
 * an active piezo, so the patterns below distinguish themselves by RHYTHM
 * instead - which costs nothing that matters here and arguably reads better
 * for the users this device is for: a long insistent pulse train is easier to
 * learn than a semitone. The saving is real: one GPIO, no timer, and TIM16
 * stays free.
 *
 * WHO EACH SOUND IS FOR. Settled and not re-litigated here:
 *   - BUZZ_DOSE_REMINDER is for the PATIENT, at the opening edge of a window.
 *   - BUZZ_DOSE_MISSED is for a CARER, at the closing edge. A missed window
 *     is by definition the case where the patient did not respond to the
 *     on-screen reminder, so beeping harder at them is nagging. This one
 *     fetches somebody else, who opens carer mode -> DOSE HISTORY.
 * That is why BUZZ_DOSE_MISSED is the only pattern built from long tones.
 */

#ifndef BUZZER_H
#define BUZZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Set to 0 to build a silent device. The whole module compiles away to
 * no-ops; no caller needs an #if. */
#ifndef MEDSIGHT_BUZZER
#define MEDSIGHT_BUZZER 1
#endif

/* The on-screen keyboard, OFF by default and deliberately so.
 *
 * Entering a patient name is on the order of a hundred taps, and the project
 * owner's instruction was explicit: better silent than irritating. An active
 * buzzer makes that MORE true, not less - it has exactly one pitch, so a
 * hundred of them are a hundred identical chirps. Flip this to 1 on the bench
 * if you disagree; nothing else has to change. */
#ifndef MEDSIGHT_BUZZER_KEYBOARD_CLICK
#define MEDSIGHT_BUZZER_KEYBOARD_CLICK 0
#endif

typedef enum {
    BUZZ_TICK = 0,      /* one 10 ms blip - a UI tap. Barely a sound.       */
    BUZZ_DISPENSE_OK,   /* two short beeps - the dose came out              */
    BUZZ_DISPENSE_FAIL, /* four rapid beeps - jam or short count            */
    BUZZ_DOSE_REMINDER, /* 3x (short-short) - patient-facing, polite        */
    BUZZ_DOSE_MISSED,   /* 4x long - carer-facing, insistent, unmistakable  */
    BUZZ_PATTERN_COUNT
} buzz_pattern_t;

/**
 * @brief  Configure the buzzer GPIO. Call from main() before the scheduler.
 *         Leaves the pin LOW - silent - on every path.
 */
void buzzer_init(void);

/**
 * @brief  Create the Alert task's event flag. Call from main() alongside the
 *         other osal_*_create() calls, before the scheduler starts.
 */
void buzzer_service_init(void);

/**
 * @brief  The Alert task body. Blocks on its event flag and plays whatever
 *         pattern was requested. Never returns.
 */
void buzzer_task_fn(void *arg);

/**
 * @brief  Request a pattern. NON-BLOCKING and safe to call from any task -
 *         it sets one event-flag bit and returns.
 *
 *         state_machine_update()'s 10 ms poll is what makes touch feel
 *         immediate, so nothing in the UI may ever wait on a tone finishing
 *         This is the reason the Alert task exists
 *         at all: a tone has to be able to outlive the call that triggered it.
 *
 *         A request made while a pattern is already playing replaces it if
 *         the new one is more important, and is dropped if it is not - a tap
 *         tick must never interrupt a missed-dose alert.
 *
 *         NOT safe from handler context. The schedule alarm handler sets its
 *         own flag and returns; the buzzer is rung from schedule_service(),
 *         which is a task.
 */
void buzzer_play(buzz_pattern_t pattern);

/** @brief  Convenience for the UI's tap feedback - BUZZ_TICK, or nothing at
 *          all when the caller is the on-screen keyboard and
 *          MEDSIGHT_BUZZER_KEYBOARD_CLICK is 0. */
void buzzer_tick(bool is_keyboard);

/** @brief  Silence immediately and abandon any pattern in progress. */
void buzzer_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
