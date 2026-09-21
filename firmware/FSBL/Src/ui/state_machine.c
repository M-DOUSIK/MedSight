/* state_machine.c - MedSight application state machine
 *
 * Every screen in the device is a state here, and every transition between
 * them goes through state_machine_update(), which the UI task calls every
 * 10 ms. Nothing in this file blocks: a state that has to wait decides once,
 * then polls HAL_GetTick() until its deadline, so touch and the physical
 * USER1 button stay alive throughout.
 *
 * The two face-capture states are the reason for that rule. Inference runs
 * in its own µT-Kernel task (ai_vision.c, priority 3, below this one), and
 * each capture state is a small non-blocking phase machine: a 1.5 s preview
 * window, three attempts 500 ms apart, camera_stop() sequencing.
 *
 * FRAME-BUFFER OWNERSHIP. Between ai_vision_capture_request() and the
 * matching ai_vision_capture_wait() success, the AI task owns
 * BUFFER_ADDRESS: both NPU networks' activation scratch overlaps it (see
 * the memory-hazard note at the top of ai_vision.c). This file must draw
 * nothing at all across that window - which is why a USER1 press during a
 * capture is recorded as s_cancel_pending and acted on only once the
 * capture completes, instead of transitioning home and redrawing straight
 * into memory the NPU is still using.
 *
 * Gallery-full is refused up front, three failed capture attempts offer
 * TRY AGAIN / CANCEL rather than dead-ending home, and an unavailable SD
 * card is stated on the home screen instead of failing silently.
 *
 * pill_count is the DOSE, not a stock level: nothing here decrements it.
 * See ai_vision.h.
 */

#include "ui/state_machine.h"
#include "ui/touch_driver.h"
#include "ui/gui_draw.h"
#include "ui/anime_ui.h"
#include "ui/registration_ui.h" /* Keyboard/pillcount/confirm screens */
#include "ui/carer_ui.h"        /* Passcode gate + carer mode      */
#include "schedule_time_source.h" /* The clock behind the schedule */
#include "ms_memtest.h"         /* The AI arena self-test                     */
#include "ms_osal.h"      /* OSAL delay instead of HAL_Delay */
#include "sd_logger.h"    /* Async log calls */
#include "ai_vision.h"    /* Real face detect + embed + gallery match */
#include "ai/intake.h"    /* Action recognition (corroboration only) */
#include "dispenser.h"    /* The real hopper, behind a build switch */
#include "buzzer.h"       /* The Program Plan's Alert Task          */
#include "ui/ai_overlay.h" /* Draw what the models see */
#include "stm32n6xx_hal.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern LTDC_HandleTypeDef   hltdc;
extern DCMIPP_HandleTypeDef hdcmipp;

/* Routine state-trace and bookkeeping output goes behind
 * MEDSIGHT_DEBUG (default off), same pattern as sd_diskio.c's
 * MEDSIGHT_DEBUG_DISKIO. Fault-reporting stays on printf() directly and
 * unconditional: the NPU-init-timeout warning in state_machine_init() and
 * the NPU latency measurement in poll_capture() are NOT touched by this
 * macro on purpose. */
#ifndef MEDSIGHT_DEBUG
#define MEDSIGHT_DEBUG 0
#endif
#if MEDSIGHT_DEBUG
#define MS_DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define MS_DBG_PRINTF(...) do { } while (0)
#endif

/* How long STATE_CONFIRM_TAKEN waits for a tap ("I Took It" or
 * "Skip") before treating it as a missed confirmation. No specific value
 * for this timeout is given anywhere; 30 s was chosen as generous
 * for an elderly user
 * reading a large on-screen button, without leaving the device stuck
 * mid-flow indefinitely. */
#define CONFIRM_TAKEN_TIMEOUT_MS  30000u

/* Set around ai_vision_run_pipeline() calls. The actual hardware freeze
 * is camera_stop() below (DCMIPP DMA into BUFFER_ADDRESS must really
 * stop, not just be flagged) - this flag is bookkeeping and diagnostic
 * on top of that. */
static volatile bool g_isp_suspend = false;

/* Identification result for the current STATE_CAMERA_DISPENSE
 * pass, decided once up front and then just displayed/timed out. */
static bool     s_dispense_identified = false;
static char     s_dispense_patient_name[PATIENT_NAME_MAX];
/* Gallery slot of the matched patient, and their dose - the number of pills
 * shown on STATE_DISPENSING and recorded in the audit log.
 *
 * This used to read (and decrement) a `pills_remaining`
 * stock counter. `pill_count` is the DOSE, fixed per patient; nothing about
 * dispensing changes it. See the comment on PatientRecord in ai_vision.h. */
static int      s_dispense_slot       = -1;
static uint8_t  s_dispense_pill_count = 0;
/* True while STATE_CONFIRM_TAKEN is holding its post-tap
 * acknowledgement ("Thank You!") on screen before returning home. */
static bool     s_taken_ack_shown      = false;

/* Earlier builds each kept a "hold the error dialog on screen for
 * 2.5 s, then go home regardless" flag here (s_register_face_failed /
 * s_dispense_error_shown). Both are gone: a failed capture now goes to
 * STATE_FACE_RETRY, which waits for the user instead of deciding for them. */

/* Set once gallery_add_patient()/the "gallery full" message has
 * been shown on STATE_CONFIRM_REGISTER, so that screen can hold for a
 * couple of seconds before returning home. */
static bool     s_confirm_done = false;

/* ── Non-blocking face capture ──────────────────────────────── */

/* How long the live preview runs before the camera is frozen for the NPU
 * pass - unchanged from Earlier builds, just no longer an osal_delay_ms(). */
#define CAPTURE_PREVIEW_MS        2500u

/* How long to let the camera write BUFFER_ADDRESS again after the live
 * preview closes, before asking the NPU to analyse it.
 *
 * The preview streams to PSRAM so the UI can own the screen (ai_overlay.c).
 * ai_vision_run_pipeline() then crops its input out of BUFFER_ADDRESS -
 * which, for the whole of the preview, no camera has been writing. Request
 * a capture without this gap and the NPU is handed the last UI screen
 * instead of a face. At 30 fps one frame is 33 ms; 150 ms is four, which
 * also covers the ISP re-converging its exposure after the pipe restart. */
#define PREVIEW_SETTLE_MS          150u
/* One-shot warning if the AI task has not answered after this long. The wait
 * itself is deliberately NOT abandoned: the AI task still owns
 * BUFFER_ADDRESS, so pressing on would mean drawing into memory the NPU is
 * using. Earlier builds had exactly the same unbounded behaviour (the
 * pipeline call simply blocked this task), so waiting is a non-regression;
 * the warning just makes a wedged NPU visible on the console instead of
 * looking like a UI freeze. */
#define CAPTURE_STUCK_WARN_MS    15000u

typedef enum {
    CAP_PREVIEW = 0,   /* live preview from PSRAM, UI owns the screen        */
    CAP_SETTLE,        /* preview closed, waiting for a real frame in the FB */
    CAP_WAITING        /* camera stopped, AI task owns BUFFER_ADDRESS        */
} capture_phase_t;

static capture_phase_t s_cap_phase       = CAP_PREVIEW;
static bool            s_cap_warned      = false;
/* True from ai_vision_capture_request() until its result has been consumed.
 * While set, nothing in this file may draw - see the header comment. */
static bool            s_cap_in_flight   = false;
/* USER1 pressed during a capture: honoured once the capture completes. */
static bool            s_cancel_pending  = false;

/* ── HOW LONG THE MASCOT STAYS SAD AFTER A MISSED DOSE ───────────────────
 *
 * It used to be FOREVER. MASCOT_ERROR was set when a dose window closed
 * unserved and nothing cleared it: the device kept crying until the next
 * window opened, or someone pressed USER1, or an unrelated flow happened to
 * reset it. In DEMO mode that is about twelve minutes; on a real clock it is
 * hours.
 *
 * That is worse than it sounds, because an indefinite signal stops carrying
 * information. A device that has been crying for an hour cannot tell a carer
 * whether the miss was a minute ago or this morning - and the permanent
 * record of the miss is the dose log, which is where it belongs. The mascot
 * is an AMBIENT CUE, and an ambient cue that never clears is just a broken
 * looking device.
 *
 * 60 seconds of real time: long enough for someone in the room to notice and
 * come over, short enough that the device does not look stuck. Real seconds
 * rather than demo-clock minutes on purpose - this is feedback for a human
 * standing there, not a scheduled event. */
#define MISSED_SAD_HOLD_MS   60000u

/* When the sad face should end, or 0 when it is not showing for a miss.
 * Every other path that sets a mascot state disarms this, so a stale timer
 * can never reach in and override a face that something else chose. */
static uint32_t        s_sad_until = 0u;

/* ── THE DOSE WE LAST DECLARED MISSED ────────────────────────────────────
 *
 * A dose window closes, MISSED is written, and then the patient taps "I Took
 * It" a moment later - because they were mid-gesture, or the watch timed out
 * just as they finished. On hardware:
 *
 *     03:30:49  MISSED: DOUSIK did not take the 03:00 dose
 *     03:32:50  CONFIRMED: DOUSIK took pills (hand reached mouth)
 *
 * Both lines are individually true and together they are a contradiction. A
 * carer reading that log cannot tell what happened, and this log is the whole
 * point of the feature: missed-dose recording exists so the device
 * could record what did NOT happen, and a record that contradicts itself is
 * worse than one that says less.
 *
 * Note the wording degrades too: the in-window line names the dose ("took the
 * 02:00 dose (on time...)"), the out-of-window one falls back to "took pills"
 * and drops the connection entirely - so the two entries do not even
 * obviously refer to the same dose.
 *
 * The fix is not to suppress the MISSED line. It was true when written and an
 * append-only audit log must not rewrite history. Instead the late
 * confirmation names the window it belongs to and says it supersedes the
 * earlier entry, so the two lines read as one story. */
static int             s_missed_slot   = -1;
static uint16_t        s_missed_minute = 0u;

/* The capture result screen ("what the camera saw" + NEXT).
 * It is a real state rather than a blocking dwell, so it needs somewhere to
 * remember its title and where the flow resumes when NEXT is tapped. */
static char            s_capres_title[64];
static AppState_t      s_capres_next     = STATE_HOME;

/* Which flow STATE_FACE_RETRY should resume - capture failures can come from
 * either registration or dispensing and TRY AGAIN must go back to the right
 * one - and what to say, since "no face at all" and "a face I do not know"
 * call for different advice. */
#define NO_FACE_MESSAGE  "I could not see a face.\n\n" \
                         "Please sit in front of the\n" \
                         "camera in good light."
/* Neither screen may sit on the display forever if the user walks away. */
#define FACE_RETRY_TIMEOUT_MS   30000u
#define ALERT_TIMEOUT_MS        30000u

static AppState_t      s_retry_target    = STATE_HOME;
static const char     *s_retry_message   = NO_FACE_MESSAGE;

/* STATE_ALERT context: what to show, and where to go when it is dismissed. */
static const char     *s_alert_title     = "";
static const char     *s_alert_message   = "";
static uint16_t        s_alert_accent    = COLOR_WARN;
static AppState_t      s_alert_next      = STATE_HOME;

static void show_alert(const char *title, const char *message,
                        uint16_t accent, AppState_t next_state);

/* ═══════════════════════════════════════════════════════════════════════════
 * The scheduled-dose engine
 *
 * The Program Plan submitted in March named "Schedule validation" as a core
 * function and described the camera task waking "on a scheduled uT-Kernel
 * alarm (aligned to dose times)". Nothing in this device has ever known the
 * time of day. This is that, built.
 *
 * ── Why an alarm handler and not a polling task ──────────────────────────
 *
 * A dose window is a one-shot deadline at an absolute time of day, three or
 * four times a day. A task that woke every ten seconds to compare the clock
 * against a schedule would wake 8,640 times a day in order to act four
 * times, and it would do that against the one number this project has been
 * measuring throughout - the ~89% of wall-clock time the Cortex-M55
 * spends asleep. uT-Kernel's tk_cre_alm/tk_sta_alm is an object that costs
 * exactly nothing until it fires. The "real-time performance" criterion and
 * "power saving" are answered here by the same mechanism.
 *
 * ── The two edges, and why one alarm serves both ─────────────────────────
 *
 * A dose window has an opening and a closing edge, and both matter:
 *   OPEN    the window starts  -> remind, and start counting
 *   CLOSE   the window ends    -> if nobody dispensed, this is a MISSED dose
 * The missed-dose line is the single most valuable entry in the whole audit
 * trail and this device has never been able to write it. So the alarm is
 * re-armed for the close the moment it fires for the open, and re-armed for
 * the next dose the moment it fires for the close. One object, one pending
 * expiry at a time, and no polling anywhere.
 *
 * ── Handler context ──────────────────────────────────────────────────────
 *
 * schedule_alarm_handler() runs in uT-Kernel's timer context. It may not
 * block, may not printf, may not call anything unbounded - see ms_osal.h's
 * warning about what a slow handler path
 * costs on this hardware. It sets one bit in an event flag and returns. The
 * UI task does every piece of actual work: reading the gallery, drawing,
 * logging. That is the same division of labour used for
 * the AI task, applied to time instead of to inference.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* How long a dose window stays open, in SCHEDULE minutes - so in demo mode
 * (MEDSIGHT_FAST_CLOCK) it compresses along with everything else and a
 * missed dose becomes filmable in seconds instead of half an hour.
 *
 * 30 minutes is a number a pharmacist would recognise: wide enough that "I
 * was making tea" is not a missed dose, narrow enough that a dose taken
 * inside it genuinely happened at the scheduled time. A decision, not a
 * default. */
#define DOSE_WINDOW_MINUTES   30u

#define SCHED_FLAG_OPEN    (1u << 0)   /* a dose window has just opened      */
#define SCHED_FLAG_CLOSE   (1u << 1)   /* the open window has just expired   */

typedef enum {
    SCHED_IDLE = 0,     /* alarm armed for the next window OPEN, or disarmed */
    SCHED_WINDOW_OPEN   /* alarm armed for that window's CLOSE               */
} sched_phase_t;

static osal_flag_handle_t  s_sched_flag  = NULL;
static osal_alarm_handle_t s_sched_alarm = NULL;
static volatile sched_phase_t s_sched_phase = SCHED_IDLE;

/* The dose currently due, while a window is open. -1 = none. */
static int      s_due_slot   = -1;
static uint16_t s_due_minute = 0u;
/* Set when a dose is confirmed inside an open window, so the closing edge
 * knows whether to write a MISSED line. */
static bool     s_due_served = false;

/* Runs in HANDLER context. Three lines, deliberately. */
static void schedule_alarm_handler(void *arg)
{
    (void)arg;
    osal_flag_set(s_sched_flag,
                  (s_sched_phase == SCHED_WINDOW_OPEN) ? SCHED_FLAG_CLOSE
                                                       : SCHED_FLAG_OPEN);
}

void state_machine_service_init(void)
{
    s_sched_flag  = osal_flag_create();
    s_sched_alarm = osal_alarm_create(schedule_alarm_handler, NULL);
}

/* Find the dose that comes soonest, across every enrolled patient. Returns
 * the gallery slot and writes its minute-of-day, or -1 if nothing in the
 * gallery has a schedule at all. */
static int schedule_find_next(uint16_t *out_minute)
{
    uint16_t now       = time_source_minute_of_day();
    int      best_slot = -1;
    uint32_t best_gap  = 0xFFFFFFFFu;
    uint16_t best_min  = 0u;

    for (int i = 0; i < MAX_PATIENTS; i++) {
        const PatientRecord *p = &patient_gallery[i];
        if (!p->valid) continue;
        for (uint8_t k = 0; k < p->dose_time_count && k < MAX_DOSE_TIMES; k++) {
            uint16_t t = p->dose_time[k];
            /* Forward distance to the next occurrence, wrapping midnight. A
             * dose at exactly "now" counts as a whole day away, which is
             * what stops a window that has just been served from firing
             * again immediately - the same reasoning as
             * time_source_ms_until(). */
            uint32_t gap = (t > now) ? (uint32_t)(t - now)
                                     : (uint32_t)(MINUTES_PER_DAY - (now - t));
            if (gap < best_gap) {
                best_gap  = gap;
                best_slot = i;
                best_min  = t;
            }
        }
    }
    if ((best_slot >= 0) && (out_minute != NULL)) {
        *out_minute = best_min;
    }
    return best_slot;
}

/* Arm the alarm for the next dose window to OPEN. Safe to call at any time;
 * disarms and stays disarmed when there is nothing to wait for. Called at
 * start-up, after every window closes, and after any carer edit that could
 * have changed what comes next. */
static void schedule_arm_next(void)
{
    s_sched_phase = SCHED_IDLE;
    s_due_slot    = -1;
    s_due_served  = false;

    osal_alarm_stop(s_sched_alarm);
    osal_flag_clear(s_sched_flag, SCHED_FLAG_OPEN | SCHED_FLAG_CLOSE);

    if (!time_source_is_valid()) {
        /* No clock, no schedule. Deliberately not an error state - the
         * device keeps dispensing on demand, it simply cannot remind anyone.
         * The home screen says so, so this is visible rather than silent. */
        MS_DBG_PRINTF("schedule: clock not set - no dose windows armed.\n");
        return;
    }

    uint16_t minute = 0u;
    int slot = schedule_find_next(&minute);
    if (slot < 0) {
        MS_DBG_PRINTF("schedule: nobody has a dose schedule - nothing armed.\n");
        return;
    }

    uint32_t ms = time_source_ms_until(minute);
    if (!osal_alarm_start(s_sched_alarm, ms)) {
        /* Unconditional: "reminders are silently off" is the worst failure
         * this feature has, because nothing happens and nothing says so. */
        printf("schedule: WARNING could not arm the dose alarm - "
               "reminders are OFF.\r\n");
        return;
    }

    char hhmm[8];
    time_source_format_hhmm(hhmm, sizeof(hhmm), minute);
    MS_DBG_PRINTF("schedule: next dose %s for '%s' in %lums.\n",
                  hhmm, patient_gallery[slot].name, (unsigned long)ms);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * The hidden entry gesture
 *
 * Carer mode has to be findable by a carer who was told one sentence, and
 * not by a patient exploring the screen. That is the phone
 * developer-settings pattern, and the right target here is the title bar: it
 * is the one thing on the home screen that is unmistakably not a button, so
 * tapping it repeatedly is not something anyone does by accident, and it is
 * large enough to hit reliably with an unsteady hand.
 *
 * Five taps inside three seconds. Five, because one or two are plausible
 * accidents and ten is a chore to explain over the phone. Three seconds,
 * because it is comfortably long for a deliberate five taps by an older hand
 * and far too short for five accidental ones spread over a minute to
 * accumulate. The count resets when the window lapses or any other control
 * is touched.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define TITLE_TAP_X   240u
#define TITLE_TAP_Y    24u
#define TITLE_TAP_W   320u
#define TITLE_TAP_H    68u

static uint8_t  s_title_taps      = 0u;
static uint32_t s_title_tap_first = 0u;

/* Where STATE_PASSWORD goes when the code is accepted. The gate is shared;
 * this is the only thing that differs between its two callers. */
static AppState_t s_pw_next = STATE_CARER_MENU;


/* ── Camera helpers ─────────────────────────────────────────────────────── */
static void camera_stop(void)
{
    HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0);
}
static void camera_start(void)
{
    HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE1,
                               DCMIPP_VIRTUAL_CHANNEL0,
                               BUFFER_ADDRESS, DCMIPP_MODE_CONTINUOUS);
}
static void switch_ltdc_buffer(uint32_t addr)
{
    HAL_LTDC_SetAddress(&hltdc, addr, LTDC_LAYER_1);
}

/* ── Hitbox test ─────────────────────────────────────────────────────────── */
static bool check_hit(uint32_t tx, uint32_t ty,
                       uint32_t x,  uint32_t y,
                       uint32_t w,  uint32_t h)
{
    return (tx >= x && tx <= (x+w) && ty >= y && ty <= (y+h));
}

/* ── State ───────────────────────────────────────────────────────────────── */
static AppState_t current_state   = STATE_HOME;
static bool       state_init_done  = false;
static uint32_t   state_entry_time = 0;

/*
 * TOUCH EDGE DETECTION
 * ────────────────────
 * was_touching tracks whether the finger was down on the PREVIOUS call.
 * We only act on a NEW touch (rising edge: !was_touching && touched now).
 * This prevents the same finger-down from cascading through multiple states.
 */
static bool was_touching = false;

/* ── Helpers ──────────────────────────────────────────────────── */

/* Queue a one-button alert screen. Safe to call from any state; the screen
 * itself is drawn on STATE_ALERT's entry, never from here, so this can be
 * called at a point where the framebuffer is not ours to draw into. */
static void show_alert(const char *title, const char *message,
                        uint16_t accent, AppState_t next_state)
{
    s_alert_title   = title;
    s_alert_message = message;
    s_alert_accent  = accent;
    s_alert_next    = next_state;
    current_state   = STATE_ALERT;
    state_init_done = false;
}

#if MEDSIGHT_PHYSICAL_DISPENSER
/* One pill counted through the IR beam, one step of the bar.
 *
 * dispenser.c invokes this from the dispense loop, which runs on the UI task,
 * so drawing from here is safe - it is the same context that drew the screen
 * this bar sits on. It is never called from the EXTI handler.
 *
 * The bar now reports a physical event rather than the passage of time: at
 * pill 1 of 2 it is exactly half full and it STAYS there until a second pill
 * actually breaks the beam. If the hopper is empty it does not move at all,
 * which is the honest picture. */
static void dispensing_progress_cb(uint8_t counted, uint8_t requested)
{
    uint32_t total = (requested > 0u) ? requested : 1u;
    gui_draw_dispensing_progress((uint16_t)((uint32_t)counted * 100u / total),
                                 counted);
}
#endif

/* ── The second framebuffer does not exist ────────────
 *
 * GUI_BUFFER_ADDRESS (0x342BB800) was going to be a second framebuffer so
 * the UI could keep drawing while the NPU ran. A canary check written for
 * exactly this question answered it on hardware, on the first capture:
 *
 *   CANARY FAIL: GUI_BUFFER_ADDRESS (0x342BB800-0x34376FFF) was overwritten
 *   during capture (first=3B185504 last=3E2699ED, expected C0FFEEA5)
 *
 * Both ends of the 750 KB region were clobbered, with what are plainly
 * float bit patterns - NPU activation data. So the activation scratch does
 * not merely start at BUFFER_ADDRESS, it runs past the whole of the second
 * framebuffer: at least 0x34200000-0x34376FFF, over 1.5 MB.
 *
 * The rule for this case was set in advance: if it does not fit,
 * say so and stop - do not shrink the framebuffer or move the NPU's pools
 * to make it fit. So this file keeps the single-buffer
 * behaviour, and the capture window is handled by blanking the display
 * instead (see display_blank()).
 * ──────────────────────────────────────────────────────────────────────── */

/* Blank the panel while the NPU owns BUFFER_ADDRESS, rather than leaving a
 * frozen camera frame on screen to be visibly scribbled over for ~750 ms.
 * Disabling the layer makes the LTDC show its background colour (set to the
 * theme blush in main.c's LCD_Init), which reads as an intentional pause.
 *
 * Gated so it can be turned off without unpicking anything if it ever
 * misbehaves on hardware: set to 0 and the old behaviour (a
 * corrupting frozen frame) comes back. */
#ifndef MS_BLANK_DURING_CAPTURE
#define MS_BLANK_DURING_CAPTURE 1
#endif

static bool s_display_blanked = false;

static void display_blank(bool on)
{
#if MS_BLANK_DURING_CAPTURE
    if (on == s_display_blanked) return;
    if (on) __HAL_LTDC_LAYER_DISABLE(&hltdc, LTDC_LAYER_1);
    else    __HAL_LTDC_LAYER_ENABLE(&hltdc, LTDC_LAYER_1);
    __HAL_LTDC_RELOAD_IMMEDIATE_CONFIG(&hltdc);
    s_display_blanked = on;
#else
    (void)on;
#endif
}

/* Begin a face capture: freeze the camera and hand BUFFER_ADDRESS to the AI
 * task. Nothing in this file may draw until poll_capture() reports a result
 * other than "still running" - the NPU owns the one framebuffer we have. */
static void begin_capture(void)
{
    g_isp_suspend = true;
    camera_stop();
    s_cap_in_flight = true;
    s_cap_warned    = false;

    display_blank(true);

    ai_vision_capture_request();
    s_cap_phase = CAP_WAITING;
}

/* Poll the AI task without blocking. Returns AI_CAPTURE_TIMEOUT while the
 * capture is still running (the caller must simply come back next tick), and
 * clears the in-flight/ownership flag on any other result. */
static ai_capture_result_t poll_capture(int8_t *out_embedding, uint32_t since_ms)
{
    ai_capture_result_t r = ai_vision_capture_wait(out_embedding, OSAL_NO_WAIT);
    if (r == AI_CAPTURE_TIMEOUT) {
        if (!s_cap_warned && (since_ms > CAPTURE_STUCK_WARN_MS)) {
            s_cap_warned = true;
            MS_DBG_PRINTF("state_machine: WARNING AI capture still running after %ums.\n",
                   (unsigned)since_ms);
        }
        return r;
    }
    /* NPU inference latency, wall-clock from
     * begin_capture()'s ai_vision_capture_request() to this result -
     * the technical reference asked for a measured number and never had one.
     * since_ms already IS that interval (state_entry_time is reset right
     * after begin_capture() is called). One line, not a hot path, left
     * unconditional like the sleep-clocks readback - this is exactly the
     * number this project needs recorded from real hardware. */
    printf("AI capture: result after %ums (detector+embedder wall time, "
           "includes AI-task IPC/scheduling).\r\n", (unsigned)since_ms);

    g_isp_suspend   = false;
    s_cap_in_flight = false;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* How long state_machine_init() will wait for the NPU to finish coming up
 * before drawing anyway. ai_vision_init() takes roughly 0.5-1 s on this
 * board; 20 s is far beyond any legitimate value, so reaching it means the
 * AI task is wedged, not slow. In that case the UI comes up regardless -
 * losing face recognition is bad, but a blank screen is worse, and the
 * console line below says exactly which of the two happened. */
#define AI_INIT_WAIT_MS   20000u

void state_machine_init(void)
{
    /* Touch first: gt911_hardware_reset() spends 140 ms in osal_delay_ms(),
     * which is 140 ms of NPU bring-up the wait below will not have to pay
     * for. Neither call touches the framebuffer. */
    touch_driver_init();
    user_button_init();
    was_touching = false;

    camera_stop();

    /* NOTHING above this line writes to BUFFER_ADDRESS; nothing below it may
     * run until the NPU has stopped writing there. See the long comment on
     * ai_vision_wait_init() in ai_vision.h - this one line is the whole fix
     * for the cold-boot "screen draws, glitches, then greys out" fault. */
    if (!ai_vision_wait_init(AI_INIT_WAIT_MS)) {
        printf("state_machine: NPU init did not finish in %ums - "
               "drawing anyway, face recognition may be unavailable.\n",
               AI_INIT_WAIT_MS);
    }

    /* The AI arena self-test runs HERE and nowhere else.
     *
     * The arena is at 0x34388000, in AXISRAM6 - and AXISRAM5/6 are NOT
     * powered at reset. FSBL's stm32n6xx_hal_msp.c brings up AXISRAM3 and
     * AXISRAM4 only; the other two are enabled by SystemInit_POST() inside
     * npu_init.c, which runs from aiPreInitialize() on the AI task. So the
     * one line above - ai_vision_wait_init() - is also the precondition for
     * this test being meaningful. Run it any earlier and it reads back
     * whatever an unclocked bank returns, which would pass or fail for
     * reasons that have nothing to do with the arena.
     *
     * It is also before gui_draw_init(), because the arena is disjoint from
     * the framebuffer and this is the last moment when nothing is on screen
     * to be disturbed if that assumption is ever wrong.
     *
     * IT MOVED, and only for builds where the arena has an
     * occupant. The self-test fills all 225,280 bytes with three patterns;
     * the pill detector's activations now occupy 208,000 of them from
     * 0x34388000. Running the test after that network has initialised means
     * writing over its arena.
     *
     * Today that is harmless - the generated pool declares
     * `use4initializers=NO`, so nothing durable lives there and activations
     * are rewritten on every stai_pill_run(). But "harmless because of a flag
     * in a generated comment" is not a property to rely on across a
     * regeneration, and a self-test that claims to prove memory is FREE
     * should not be run over memory that is now SPOKEN FOR.
     *
     * So with action recognition on, the test runs inside task_ai_fn in the
     * window between ai_vision_init() (which powers AXISRAM5/6) and
     * intake_detect_init() (which claims the arena) - full coverage, no
     * conflict. With it off, nothing occupies the arena and the test stays
     * exactly where it is. */
#if !MEDSIGHT_ACTION_RECOGNITION
    (void)ms_memtest_arena();
#endif

    /* One framebuffer. GUI_BUFFER_ADDRESS was proven unusable on hardware -
     * see the Part A note above begin_capture(). */
    gui_draw_init(BUFFER_ADDRESS, FRAME_WIDTH, FRAME_HEIGHT);
    switch_ltdc_buffer(BUFFER_ADDRESS);

    anime_ui_set_dest_buffer(BUFFER_ADDRESS);
    anime_ui_set_bg_color(COLOR_BG);

    gui_draw_home_screen();

    /* The passcode store, then the schedule.
     *
     * Order matters and it is one-way: carer_pass_init() reads carer.cfg
     * from the card, and schedule_arm_next() reads the gallery that
     * ai_vision_init() already loaded plus the clock that main() already
     * started. Neither can run before this point, and nothing above them
     * depends on either. */
    carer_pass_init();
    if (carer_pass_is_default()) {
        /* Said out loud on every boot rather than buried in a document. A
         * device still on its shipped passcode is one a visitor can open. */
        printf("carer: the carer passcode is still the build-time default - "
               "change it from carer mode.\r\n");
    }
    schedule_arm_next();

    current_state  = STATE_HOME;
    state_init_done = true;
}

/* ── Acting on the schedule alarm, from TASK context ──────────
 *
 * Called once per UI tick. The wait is OSAL_NO_WAIT - a poll of an event
 * flag, not a poll of the clock. The distinction is the whole design: this
 * costs one non-blocking flag read per 10 ms tick and the flag is only ever
 * set by an alarm that fires three or four times a day. Nothing here reads
 * the RTC on a loop. */
static void schedule_service(void)
{
    if (s_sched_flag == NULL) {
        return;
    }

    uint32_t got = 0u;
    if (!osal_flag_wait(s_sched_flag, SCHED_FLAG_OPEN | SCHED_FLAG_CLOSE,
                        OSAL_FLAG_WAIT_OR | OSAL_FLAG_WAIT_CLEAR,
                        &got, OSAL_NO_WAIT)) {
        return;   /* nothing fired; the common case, and it is cheap */
    }

    char line[SD_LOG_MSG_MAX_LEN];

    if ((got & SCHED_FLAG_OPEN) != 0u) {
        uint16_t minute = 0u;
        int slot = schedule_find_next(&minute);
        /* schedule_find_next() treats "exactly now" as a whole day away, so
         * at the instant the alarm fires the dose that just came due is the
         * FURTHEST one, not the nearest. Recover it from the clock instead:
         * the window that just opened is the one whose time is the current
         * minute. */
        uint16_t now = time_source_minute_of_day();
        int      due = -1;
        for (int i = 0; (i < MAX_PATIENTS) && (due < 0); i++) {
            const PatientRecord *pr = &patient_gallery[i];
            if (!pr->valid) continue;
            for (uint8_t k = 0; k < pr->dose_time_count && k < MAX_DOSE_TIMES; k++) {
                /* A minute either side, because the alarm's expiry and the
                 * clock's minute boundary are two different clocks and there
                 * is no reason for them to agree to the millisecond. */
                uint32_t d = (pr->dose_time[k] > now)
                             ? (uint32_t)(pr->dose_time[k] - now)
                             : (uint32_t)(now - pr->dose_time[k]);
                if (d <= 1u) { due = i; minute = pr->dose_time[k]; break; }
            }
        }
        if (due < 0) { due = slot; }   /* clocks disagreed; take the best guess */

        if (due < 0) {
            schedule_arm_next();
            return;
        }

        s_due_slot    = due;
        s_due_minute  = minute;
        s_due_served  = false;
        s_sched_phase = SCHED_WINDOW_OPEN;

        char hhmm[8];
        time_source_format_hhmm(hhmm, sizeof(hhmm), minute);
        snprintf(line, sizeof(line), "SCHEDULE: dose due %s for %s",
                 hhmm, patient_gallery[due].name);
        SD_Log_Event_Async(line);
        printf("schedule: dose window OPEN - %s for '%s'.\r\n",
               hhmm, patient_gallery[due].name);

        /* Arm the closing edge on the same alarm object. */
        (void)osal_alarm_start(s_sched_alarm,
                               DOSE_WINDOW_MINUTES * time_source_minute_ms());

        /* The reminder itself, audible as well as
         * visible: three polite double-pulses, patient-facing.
         *
         * The SAI codec is still unbuilt and still deferred: a buzzer is not
         * audio playback, and the Program Plan asked for a buzzer.
         */
        anime_ui_set_state(MASCOT_ACTIVE);
        buzzer_play(BUZZ_DOSE_REMINDER);
        s_sad_until   = 0u;   /* a new dose is due; that supersedes the last miss */
        s_missed_slot = -1;   /* and so does its late-confirmation window */
        if (current_state == STATE_HOME) {
            state_init_done = false;   /* redraw the home screen with the banner */
        }
    }

    if ((got & SCHED_FLAG_CLOSE) != 0u) {
        if ((s_due_slot >= 0) && !s_due_served) {
            /* THE line this whole feature exists to write. Before it,
             * this device could record that a dose was given; it could not
             * record that one was not. */
            char hhmm[8];
            time_source_format_hhmm(hhmm, sizeof(hhmm), s_due_minute);
            snprintf(line, sizeof(line), "MISSED: %s did not take the %s dose",
                     patient_gallery[s_due_slot].name, hhmm);
            SD_Log_Event_Async(line);
            printf("schedule: dose window CLOSED - MISSED (%s, %s).\r\n",
                   patient_gallery[s_due_slot].name, hhmm);
            anime_ui_set_state(MASCOT_ERROR);
            /* THE sound this feature exists for. Four
             * long pulses, twice - the only pattern in the set built from
             * long tones, because it is the only one that has to carry to a
             * carer in another room. This alert is
             * for a carer and not for the patient: a missed window is by
             * definition the case where the patient did not respond to the
             * on-screen reminder, so beeping harder at them is nagging.
             * Fires exactly once per missed window, on this edge. */
            buzzer_play(BUZZ_DOSE_MISSED);
            s_missed_slot   = s_due_slot;
            s_missed_minute = s_due_minute;
            s_sad_until = HAL_GetTick() + MISSED_SAD_HOLD_MS;
            if (s_sad_until == 0u) {
                s_sad_until = 1u;      /* 0 means "not armed" */
            }
        } else {
            anime_ui_set_state(MASCOT_IDLE);
            s_sad_until = 0u;
        }

        bool was_home = (current_state == STATE_HOME);
        schedule_arm_next();
        if (was_home) {
            state_init_done = false;   /* drop the banner */
        }
    }
}

void state_machine_update(void)
{
    /* Act on any dose window that opened or closed since the
     * last tick. Deliberately first - a reminder that arrives while the user
     * is mid-flow must not be lost, and the home screen must be able to
     * redraw with the banner on the very next pass. */
    schedule_service();

    /* Retire the missed-dose face once its hold has run out. Checked here
     * rather than inside schedule_service() because that only runs on a
     * schedule EVENT, and this has to happen on an ordinary tick when
     * nothing at all is going on - which is exactly the situation a missed
     * dose leaves the device in. */
    if ((s_sad_until != 0u) &&
        ((int32_t)(HAL_GetTick() - s_sad_until) >= 0)) {
        s_sad_until = 0u;
        anime_ui_set_state(MASCOT_IDLE);
        if (current_state == STATE_HOME) {
            state_init_done = false;   /* redraw without the sad face */
        }
        MS_DBG_PRINTF("schedule: missed-dose face retired after %ums.\n",
                      (unsigned)MISSED_SAD_HOLD_MS);
    }

    uint32_t tx = 0, ty = 0;
    bool touched   = touch_driver_get_touch(&tx, &ty);
    bool new_touch = touched && !was_touching;   /* rising-edge only */
    was_touching   = touched;

    /* One tick per tap, from the ONE place in this file
     * that detects a new touch - so every button in the device gets it and no
     * screen can forget to. The on-screen keyboard is excluded by default:
     * entering a name is about a hundred taps, and one fixed-pitch chirp a
     * hundred times is the definition of irritating. MEDSIGHT_BUZZER_KEYBOARD_CLICK
     * flips that on the bench without touching this line. */
    if (new_touch) {
        buzzer_tick(current_state == STATE_KEYBOARD_REGISTER);
    }

    bool btn_pressed = user_button_is_pressed();

    /* Physical USER1 button -> back to home.
     *
     * This now works DURING a face capture, which it could not
     * before (the UI task was blocked inside the pipeline and never reached
     * this line). It must not act immediately though: the AI task owns
     * BUFFER_ADDRESS until its capture finishes, and going home here would
     * redraw the home screen into memory the NPU is still writing. Record
     * the intent and let the capture state honour it on completion. */
    if (btn_pressed && current_state != STATE_HOME)
    {
        if (s_cap_in_flight)
        {
            if (!s_cancel_pending)
            {
                s_cancel_pending = true;
                MS_DBG_PRINTF("state_machine: USER1 during capture - cancel pending.\n");
            }
        }
        else
        {
            /* Close the live preview FIRST if one is open. It has the
             * DCMIPP pointed at PSRAM, and intake_camera_start() is a
             * no-op while it believes the pipe is already running - so
             * cancelling out of a preview without this leaves the next
             * face scan showing a frozen viewport, with nothing in any
             * log to say why. */
            ai_overlay_preview_end();
            camera_stop();
            anime_ui_set_state(MASCOT_IDLE);
            s_sad_until     = 0u;
            current_state   = STATE_HOME;
            state_init_done = false;
            was_touching    = false;
            /* S_confirm_done is a two-second "the screen is
             * showing an acknowledgement" latch, shared by the registration
             * confirm screen and by four carer screens. Leaving USER1 as the
             * one exit that does not clear it means the NEXT screen to use it
             * starts latched and ignores its own first touch. Cheap to clear
             * here; expensive to find on a bench. */
            s_confirm_done  = false;
            s_title_taps    = 0u;
            /* Use OSAL delay instead of HAL_Delay - yields CPU */
            osal_delay_ms(200);
            return;
        }
    }

    /* A screen change retires the previous screen's live clock
     * strip. Screens that want one re-arm it in their own entry block, so
     * this cannot go stale and no screen can inherit another's. */
    if (!state_init_done)
    {
        carer_ui_clock_strip_hide();
    }

    switch (current_state)
    {
        /* ── HOME ─────────────────────────────────────────────────────── */
        case STATE_HOME:
            if (!state_init_done)
            {
                camera_stop();
                switch_ltdc_buffer(BUFFER_ADDRESS);
                gui_draw_init(BUFFER_ADDRESS, FRAME_WIDTH, FRAME_HEIGHT);
                anime_ui_set_dest_buffer(BUFFER_ADDRESS);
                anime_ui_set_bg_color(COLOR_BG);

                /* Found on hardware: a missed dose selects
                 * MASCOT_ERROR, and the crying frames used to be hard-coded
                 * to the TWO-CHOICE screen's mascot box at (56,112) - which
                 * on the home screen sits on top of the REGISTER and
                 * DISPENSE buttons and half-covered both. Point the error
                 * pose at the home screen's own mascot box instead. */
                anime_ui_set_error_box(MASCOT_BG_X, MASCOT_BG_Y,
                                       MASCOT_BG_W, MASCOT_BG_H);

                gui_draw_home_screen();

                /* The clock, live, on the home screen too - it
                 * is the screen the device spends its life on, and in demo
                 * mode it is the only place you can watch the compressed day
                 * run. Drawn after the screen so it lands on top of it. */
                carer_ui_draw_clock_strip(SUBTEXT_Y);

                /* An unusable SD card is stated, not hidden. The
                 * device deliberately keeps working without one - dispensing
                 * a dose matters more than recording it - but the operator
                 * has to be able to see that the audit log is not being
                 * written. Drawn over the home screen's dialog box, so it
                 * costs no layout and disappears by itself once the card is
                 * back and the screen is next redrawn. */
                if (!SD_Logger_Is_Available())
                {
                    gui_draw_dialog_text(
                        "No SD card - doses are not being recorded.\n"
                        "The device still works normally.");
                }

                /* The scheduled-dose reminder. This is the
                 * local alert the project plan describes - on the
                 * device's own screen, to the person standing in front of
                 * it, and recorded to the card. Nothing is pushed anywhere;
                 * there is no radio in this design and there never will be.
                 *
                 * Drawn LAST so it wins the message panel over the SD
                 * warning if both apply. That ordering is deliberate: a dose
                 * being due is time-critical and the SD warning is not, and
                 * the SD warning is on the console as well. */
                if (s_due_slot >= 0)
                {
                    char hhmm[8];
                    time_source_format_hhmm(hhmm, sizeof(hhmm), s_due_minute);
                    char banner[96];
                    snprintf(banner, sizeof(banner),
                             "%s - your %s pills are due now.\n"
                             "Tap DISPENSE MEDICINE.",
                             patient_gallery[s_due_slot].name, hhmm);
                    gui_draw_dialog_text(banner);
                }
                else if (!time_source_is_valid())
                {
                    /* A device that cannot tell the time cannot remind
                     * anyone, and saying so on the home screen is the only
                     * way a carer finds out before a dose is missed. */
                    gui_draw_dialog_text(
                        "The clock is not set - reminders are off.\n"
                        "A carer can set it in carer mode.");
                }

                state_init_done = true;
                MS_DBG_PRINTF("STATE_HOME\n");
                /* Async log via queue - does not block the UI task */
                SD_Log_Event_Async("STATE: HOME");
            }

            anime_ui_update(HAL_GetTick());

            if (new_touch)
            {
                /* The hidden carer-mode gesture. Checked
                 * before the buttons because the title bar does not overlap
                 * either of them - the ordering is for readability, not for
                 * correctness. Any tap that is NOT on the title bar resets
                 * the count, which is what stops five taps spread across a
                 * session of ordinary use from ever adding up. */
                if (check_hit(tx, ty, TITLE_TAP_X, TITLE_TAP_Y,
                              TITLE_TAP_W, TITLE_TAP_H))
                {
                    uint32_t now_ms = HAL_GetTick();
                    if ((s_title_taps == 0u) ||
                        ((now_ms - s_title_tap_first) > CARER_GESTURE_WINDOW_MS))
                    {
                        s_title_taps      = 1u;
                        s_title_tap_first = now_ms;
                    }
                    else
                    {
                        s_title_taps++;
                    }

                    if (s_title_taps >= CARER_GESTURE_TAPS)
                    {
                        s_title_taps = 0u;
                        MS_DBG_PRINTF("carer: entry gesture recognised.\n");
                        carer_ui_prompt_begin(CARER_PROMPT_CARER_MODE);
                        s_pw_next       = STATE_CARER_MENU;
                        current_state   = STATE_PASSWORD;
                        state_init_done = false;
                    }
                    break;
                }
                s_title_taps = 0u;

                if (check_hit(tx, ty, REG_BTN_X, REG_BTN_Y, REG_BTN_W, REG_BTN_H))
                {
                    /* Refuse a full gallery HERE, before asking
                     * the patient to stand in front of the camera and run a
                     * capture whose result can only be thrown away. This used to be
                     * checked at the very end of the flow, after the face
                     * capture, the keyboard and the pill count. */
                    if (gallery_count() >= MAX_PATIENTS)
                    {
                        SD_Log_Event_Async("EVENT: Registration - refused, gallery full");
                        show_alert("GALLERY FULL",
                                   "This device already holds\n"
                                   "10 patients, the maximum.\n\n"
                                   "Please contact your administrator.",
                                   COLOR_WARN, STATE_HOME);
                        break;
                    }
                    /* ── B2a: registration is authorised, or it does not
                     * happen ─────────────────────────────────────────────
                     *
                     * Until now anyone could walk up, tap this
                     * button, enrol their own face and their own dose size,
                     * and then have the machine hand them medication - with
                     * the audit log faithfully recording a legitimate,
                     * face-matched dispense to a registered patient, because
                     * that is exactly what it was. Face recognition was
                     * working perfectly. The gallery it matched against
                     * accepted anyone who asked.
                     *
                     * The gate is HERE, at the start, and not at the end of
                     * the flow. Asking afterwards would waste the user's
                     * time before refusing them, would mean a face and a
                     * name had already been taken from someone never
                     * authorised to give them, and would leave half-written
                     * state to unwind. Ask first and everything downstream
                     * is already authorised.
                     *
                     * The gallery-full check above still runs first: being
                     * told the device is full is more useful than being
                     * asked for a code and only then told. */
                    carer_ui_prompt_begin(CARER_PROMPT_REGISTRATION);
                    s_pw_next       = STATE_INSTRUCT_REGISTER;
                    current_state   = STATE_PASSWORD;
                    state_init_done = false;
                }
                else if (check_hit(tx, ty, DISP_BTN_X, DISP_BTN_Y, DISP_BTN_W, DISP_BTN_H))
                {
                    current_state   = STATE_INSTRUCT_DISPENSE;
                    state_init_done = false;
                }
            }
            break;

        /* ── REGISTER instruction screen ─────────────────────────────── */
        case STATE_INSTRUCT_REGISTER:
            if (!state_init_done)
            {
                gui_draw_ready_screen(
                    "REGISTER PATIENT", ACCENT_NEUTRAL,
                    /* Name entry comes first, and this screen
                     * still said "Face the camera" - which was simply
                     * untrue: the next screen is a keyboard. Screens that
                     * describe the wrong next step are worse than screens
                     * that describe nothing. */
                    "Let's get you set up!\n"
                    "First, type the patient's name."
                );
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;   /* force release before new touch */
                MS_DBG_PRINTF("STATE_INSTRUCT_REGISTER\n");
                SD_Log_Event_Async("STATE: INSTRUCT_REGISTER");
            }

            anime_ui_update(HAL_GetTick());

            /*
             * TOUCH GUARD - two conditions BOTH must be true:
             *  1. Rising-edge touch (new_touch) - finger was up, now down
             *  2. At least 500 ms in this state - prevents bleed from the
             *     button tap that CAUSED the state transition.
             */
            if (new_touch &&
                (HAL_GetTick() - state_entry_time > 500u) &&
                check_hit(tx, ty, READY_BTN_X, READY_BTN_Y, READY_BTN_W, READY_BTN_H))
            {
                /* Enrolment order: NAME FIRST, then the face.
                 * Two reasons, and the second is the real one.
                 *
                 * The small reason is that a name typed before the scan is a
                 * name the camera screen can use, so the device can say
                 * "HOLD STILL, RAMESH" instead of addressing a stranger.
                 *
                 * The larger one is that this is the order the interaction
                 * actually wants. Being photographed by a machine that has
                 * not yet asked who you are is the wrong way round; asking
                 * first turns the scan into something done WITH the patient
                 * rather than TO them.
                 *
                 * registration_ui_reset() moved HERE, to the one point that
                 * is unambiguously the start of an enrolment. It used to run
                 * after the capture, which under this order would wipe the
                 * name that had just been typed. */
                registration_ui_reset();
                current_state   = STATE_KEYBOARD_REGISTER;
                state_init_done = false;
            }
            break;

        /* ── REGISTER camera (face scan) ──────────────────────────────── */
        /* The flow: a brief live-preview window so
         * the user can get into frame, then freeze the camera DMA and run
         * the NPU pass, retrying up to three times. The captured embedding
         * is only HELD (in registration_ui's static buffer) - nothing
         * reaches the gallery until the user has typed a name and a pill
         * count and tapped Confirm.
         *
         * UNLIKE STATE_CAMERA_DISPENSE, the camera is deliberately NOT
         * resumed after the capture attempt (found on real hardware in
         * Resuming it left the DCMIPP DMA continuously
         * overwriting BUFFER_ADDRESS, which wiped out both the keyboard
         * screen drawn on success and the error dialog drawn on failure).
         * Every state from here on is static UI, not a camera preview, so
         * the camera must stay off.
         *
         * None of it blocks: the
         * preview window is a timestamp comparison instead of
         * osal_delay_ms(1500), and the retry loop lives in the AI task. */
        case STATE_CAMERA_REGISTER:
            if (!state_init_done)
            {
                {
                    char greet[64];
                    const char *nm = registration_ui_name();
                    if (nm && *nm) {
                        snprintf(greet, sizeof(greet), "HOLD STILL, %s", nm);
                    } else {
                        snprintf(greet, sizeof(greet), "LOOK AT THE CAMERA");
                    }
                    ai_overlay_preview_begin(
                        greet, "Keep your whole face inside the frame",
                        ACCENT_NEUTRAL);
                }
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                s_cap_phase      = CAP_PREVIEW;
                s_cancel_pending = false;
                MS_DBG_PRINTF("STATE_CAMERA_REGISTER\n");
                SD_Log_Event_Async("STATE: CAMERA_REGISTER");
            }

            if (s_cap_phase == CAP_PREVIEW)
            {
                (void)ai_overlay_preview_tick();
                if ((HAL_GetTick() - state_entry_time) > CAPTURE_PREVIEW_MS)
                {
                    /* Hand the camera back to BUFFER_ADDRESS and give it a
                     * few frames before the NPU is asked to read from there.
                     * See PREVIEW_SETTLE_MS.
                     *
                     * BLANK FIRST. The moment camera_start() runs, the
                     * DCMIPP begins overwriting all 800x480 of the frame
                     * buffer with raw sensor output - and on hardware that
                     * showed as a full-screen camera flash between the
                     * composed preview and the result screen. The display
                     * has nothing worth showing from here until the result
                     * is drawn, so it stays dark across the settle window
                     * AND the inference. begin_capture() blanks too; this
                     * one is idempotent and simply starts it earlier. */
                    ai_overlay_preview_end();
                    display_blank(true);
                    camera_start();
                    s_cap_phase      = CAP_SETTLE;
                    state_entry_time = HAL_GetTick();
                }
            }
            else if (s_cap_phase == CAP_SETTLE)
            {
                if ((HAL_GetTick() - state_entry_time) > PREVIEW_SETTLE_MS)
                {
                    begin_capture();
                    state_entry_time = HAL_GetTick();
                }
            }
            else if (s_cap_phase == CAP_WAITING)
            {
                int8_t embedding[EMBEDDING_SIZE];
                ai_capture_result_t r =
                    poll_capture(embedding, HAL_GetTick() - state_entry_time);

                if (r == AI_CAPTURE_TIMEOUT)
                {
                    break;   /* still running; the AI task owns the frame */
                }

                if (s_cancel_pending)
                {
                    /* USER1 was pressed mid-capture. Now that the AI task
                     * has released BUFFER_ADDRESS it is safe to redraw. */
                    s_cancel_pending = false;
                    MS_DBG_PRINTF("Registration: cancelled by USER1.\n");
                    SD_Log_Event_Async("EVENT: Registration - cancelled");
                    current_state   = STATE_HOME;
                    state_init_done = false;
                }
                else if (r == AI_CAPTURE_OK)
                {
                    /* NO registration_ui_reset() here. It used to sit on this
                     * line, back when the face came first and there was
                     * nothing yet to lose. The name is now typed BEFORE this
                     * point, so clearing here would silently wipe it and the
                     * confirm screen would offer to save an unnamed patient. */
                    registration_ui_set_embedding(embedding);
                    /* Show what the detector actually found -
                     * the frozen crop, its box and the five landmarks -
                     * before the flow moves on. This is the moment the user
                     * was told to look at the camera, so it is the moment
                     * the evidence is worth showing. */
                    /* The LTDC layer stays DISABLED here, deliberately.
                     * STATE_CAPTURE_RESULT unblanks AFTER it has drawn.
                     *
                     * Unblanking on this line is what put a flash of
                     * garbage between the capture and the result: at this
                     * instant the framebuffer still holds the face
                     * networks' activation tensors, because ST's codegen
                     * overlaps them with BUFFER_ADDRESS. Lighting the panel
                     * before something has been drawn shows whatever the
                     * NPU left behind. */
                    {
                        const char *nm = registration_ui_name();
                        if (nm && *nm) {
                            snprintf(s_capres_title, sizeof(s_capres_title),
                                     "NICE TO MEET YOU, %s", nm);
                        } else {
                            snprintf(s_capres_title, sizeof(s_capres_title),
                                     "FACE CAPTURED");
                        }
                    }
                    MS_DBG_PRINTF("Registration: face captured.\n");
                    SD_Log_Event_Async("EVENT: Registration - face captured");
                    s_capres_next   = STATE_PILLCOUNT_REGISTER;
                    current_state   = STATE_CAPTURE_RESULT;
                    state_init_done = false;
                }
                else if (r == AI_CAPTURE_NOT_READY)
                {
                    MS_DBG_PRINTF("Registration: AI pipeline not ready.\n");
                    SD_Log_Event_Async("EVENT: Registration - AI not ready");
                    show_alert("CAMERA NOT READY",
                               "The face camera did not start.\n\n"
                               "Please restart the device.",
                               COLOR_ALERT, STATE_HOME);
                }
                else
                {
                    /* Three failed attempts used to dead-end
                     * with a 2.5 s dialog and an unconditional return home.
                     * Offer the retry the user obviously wants instead. */
                    MS_DBG_PRINTF("Registration: no face detected after 3 attempts.\n");
                    SD_Log_Event_Async("EVENT: Registration - no face detected");
                    s_retry_target  = STATE_CAMERA_REGISTER;
                    s_retry_message = NO_FACE_MESSAGE;
                    current_state   = STATE_FACE_RETRY;
                    state_init_done = false;
                }
            }
            break;

        /* ── REGISTER keyboard (on-screen name entry) ───────────────────── */
        case STATE_KEYBOARD_REGISTER:
            if (!state_init_done)
            {
                registration_ui_draw_keyboard();
                state_init_done = true;
                was_touching    = true;   /* force release before new touch */
                MS_DBG_PRINTF("STATE_KEYBOARD_REGISTER\n");
                SD_Log_Event_Async("STATE: KEYBOARD_REGISTER");
            }

            if (new_touch && registration_ui_handle_keyboard_touch(tx, ty))
            {
                current_state   = STATE_CAMERA_REGISTER;
                state_init_done = false;
            }
            break;

        /* ── REGISTER pill count (daily dose count, 1-10) ───────────────── */
        /* ── CAPTURE RESULT ("what the camera saw") ────────────────── */
        /* The one screen in this product that shows a model's own output to
         * the person it was run on: the frozen crop the NPU was given, the
         * box it chose, the five landmarks it placed, and how confident it
         * was. Both face flows pass through here on success.
         *
         * It waits for NEXT rather than timing out. An automatic dwell has to
         * pick one duration for a viewer who wants to study the picture and
         * one who wants to get on, and it will be wrong for both.
         *
         * If there is nothing to draw - the capture view is only valid after
         * a successful detection - the flow continues immediately rather
         * than parking the user on a screen with no way forward. */
        case STATE_CAPTURE_RESULT:
            if (!state_init_done)
            {
                /* DRAW FIRST, LIGHT THE PANEL SECOND. The framebuffer holds
                 * NPU activation tensors on entry to this state; unblanking
                 * before the draw shows them for a frame. */
                const bool drew = ai_overlay_show_capture(s_capres_title, 0u);
                display_blank(false);
                if (!drew)
                {
                    MS_DBG_PRINTF("Capture result: nothing to show; skipping.\n");
                    current_state   = s_capres_next;
                    state_init_done = false;
                    break;
                }
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;   /* force a release first */
                MS_DBG_PRINTF("STATE_CAPTURE_RESULT\n");
                SD_Log_Event_Async("STATE: CAPTURE_RESULT");
            }

            /* Same 500 ms guard the other button screens use, so the tap
             * that GOT here cannot bleed through into NEXT. */
            if (new_touch &&
                (HAL_GetTick() - state_entry_time > 500u) &&
                check_hit(tx, ty, CAPRES_BTN_X, CAPRES_BTN_Y,
                          CAPRES_BTN_W, CAPRES_BTN_H))
            {
                current_state   = s_capres_next;
                state_init_done = false;
            }
            break;

        case STATE_PILLCOUNT_REGISTER:
            if (!state_init_done)
            {
                registration_ui_draw_pillcount();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_PILLCOUNT_REGISTER\n");
                SD_Log_Event_Async("STATE: PILLCOUNT_REGISTER");
            }

            if (new_touch && registration_ui_handle_pillcount_touch(tx, ty))
            {
                current_state   = STATE_CONFIRM_REGISTER;
                state_init_done = false;
            }
            break;

        /* ── REGISTER confirm (summary, save or retry) ──────────────────── */
        case STATE_CONFIRM_REGISTER:
            if (!state_init_done)
            {
                registration_ui_draw_confirm();
                state_init_done  = true;
                was_touching     = true;
                MS_DBG_PRINTF("STATE_CONFIRM_REGISTER\n");
                SD_Log_Event_Async("STATE: CONFIRM_REGISTER");
            }

            if (new_touch && !s_confirm_done)
            {
                reg_confirm_result_t result = registration_ui_handle_confirm_touch(tx, ty);
                switch (result)
                {
                    case REG_CONFIRM_SAVED:
                        gui_draw_dialog_text("Registered!\nWelcome aboard.");
                        SD_Log_Event_Async("EVENT: Registration - patient saved");
                        s_confirm_done    = true;
                        state_entry_time  = HAL_GetTick();
                        break;
                    case REG_CONFIRM_SAVED_NO_SD:
                        /* Found on hardware: the patient IS in the
                         * gallery and will be recognised right now, but
                         * patients.dat could not be written, so the enrolment
                         * dies with the power. Saying "Registered!" here was a
                         * straight untruth about whether the data is safe. */
                        gui_draw_dialog_text("Saved for now - no SD card.\n"
                                             "Re-register after restarting.");
                        SD_Log_Event_Async("EVENT: Registration - saved to RAM only (no SD)");
                        s_confirm_done    = true;
                        state_entry_time  = HAL_GetTick();
                        break;
                    case REG_CONFIRM_FULL:
                        /* Normally unreachable now - the home
                         * screen refuses a full gallery before the flow even
                         * starts. Kept as the backstop for the one case that
                         * check cannot cover: a slot filled by something else
                         * between then and now. Upgraded from a 2 s dialog to
                         * the same alert screen, so the two paths say the same
                         * thing. */
                        SD_Log_Event_Async("EVENT: Registration - gallery full");
                        show_alert("GALLERY FULL",
                                   "This device already holds\n"
                                   "10 patients, the maximum.\n\n"
                                   "Please contact your administrator.",
                                   COLOR_WARN, STATE_HOME);
                        break;
                    case REG_CONFIRM_RETRY:
                        current_state    = STATE_CAMERA_REGISTER;
                        state_init_done  = false;
                        break;
                    case REG_CONFIRM_NONE:
                    default:
                        break;
                }
            }

            if (s_confirm_done && (HAL_GetTick() - state_entry_time > 2000u))
            {
                s_confirm_done   = false;
                current_state    = STATE_HOME;
                state_init_done  = false;
            }
            break;

        /* ── DISPENSE instruction screen ──────────────────────────────── */
        case STATE_INSTRUCT_DISPENSE:
            if (!state_init_done)
            {
                gui_draw_ready_screen(
                    "DISPENSE PILLS", ACCENT_DISPENSE,
                    "Let's find your pills!\n"
                    "Face the camera, then press READY."
                );
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_INSTRUCT_DISPENSE\n");
                SD_Log_Event_Async("STATE: INSTRUCT_DISPENSE");
            }

            anime_ui_update(HAL_GetTick());

            /* Same 500ms dwell guard as INSTRUCT_REGISTER */
            if (new_touch &&
                (HAL_GetTick() - state_entry_time > 500u) &&
                check_hit(tx, ty, READY_BTN_X, READY_BTN_Y, READY_BTN_W, READY_BTN_H))
            {
                current_state   = STATE_CAMERA_DISPENSE;
                state_init_done = false;
            }
            break;

        /* ── DISPENSE camera (identify against the patient gallery) ────── */
        /* Same shape as STATE_CAMERA_REGISTER above: preview window, freeze
         * the camera, hand the frame to the AI task, poll for the result.
         *
         * The camera stays STOPPED from here on
         * - every remaining screen in the dispense flow (the error screens,
         * STATE_DISPENSING, STATE_CONFIRM_TAKEN) is static UI, not a camera
         * preview, and resuming the DCMIPP DMA would race those draws
         * exactly like the bug STATE_CAMERA_REGISTER hit and fixed in
         * during registration. That still holds. */
        case STATE_CAMERA_DISPENSE:
            if (!state_init_done)
            {
                ai_overlay_preview_begin("LOOK AT THE CAMERA",
                                         "Checking who you are",
                                         ACCENT_DISPENSE);
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                s_cap_phase      = CAP_PREVIEW;
                s_cancel_pending = false;
                MS_DBG_PRINTF("STATE_CAMERA_DISPENSE\n");
                SD_Log_Event_Async("STATE: CAMERA_DISPENSE");
            }

            if (s_cap_phase == CAP_PREVIEW)
            {
                (void)ai_overlay_preview_tick();
                if ((HAL_GetTick() - state_entry_time) > CAPTURE_PREVIEW_MS)
                {
                    /* Hand the camera back to BUFFER_ADDRESS and give it a
                     * few frames before the NPU is asked to read from there.
                     * See PREVIEW_SETTLE_MS.
                     *
                     * BLANK FIRST. The moment camera_start() runs, the
                     * DCMIPP begins overwriting all 800x480 of the frame
                     * buffer with raw sensor output - and on hardware that
                     * showed as a full-screen camera flash between the
                     * composed preview and the result screen. The display
                     * has nothing worth showing from here until the result
                     * is drawn, so it stays dark across the settle window
                     * AND the inference. begin_capture() blanks too; this
                     * one is idempotent and simply starts it earlier. */
                    ai_overlay_preview_end();
                    display_blank(true);
                    camera_start();
                    s_cap_phase      = CAP_SETTLE;
                    state_entry_time = HAL_GetTick();
                }
            }
            else if (s_cap_phase == CAP_SETTLE)
            {
                if ((HAL_GetTick() - state_entry_time) > PREVIEW_SETTLE_MS)
                {
                    begin_capture();
                    state_entry_time = HAL_GetTick();
                }
            }
            else if (s_cap_phase == CAP_WAITING)
            {
                int8_t embedding[EMBEDDING_SIZE];
                ai_capture_result_t r =
                    poll_capture(embedding, HAL_GetTick() - state_entry_time);

                if (r == AI_CAPTURE_TIMEOUT)
                {
                    break;   /* still running; the AI task owns the frame */
                }

                s_dispense_identified = false;
                s_dispense_slot       = -1;
                s_dispense_patient_name[0] = '\0';

                if (s_cancel_pending)
                {
                    s_cancel_pending = false;
                    MS_DBG_PRINTF("Dispense: cancelled by USER1.\n");
                    SD_Log_Event_Async("EVENT: Dispense - cancelled");
                    current_state   = STATE_HOME;
                    state_init_done = false;
                }
                else if (r == AI_CAPTURE_NOT_READY)
                {
                    MS_DBG_PRINTF("Dispense: AI pipeline not ready.\n");
                    SD_Log_Event_Async("EVENT: Dispense - AI not ready");
                    show_alert("CAMERA NOT READY",
                               "The face camera did not start.\n\n"
                               "Please restart the device.",
                               COLOR_ALERT, STATE_HOME);
                }
                else if (r == AI_CAPTURE_OK)
                {
                    float confidence = -1.0f;
                    int   slot = gallery_find_best_match(embedding, &confidence);
                    if (slot >= 0)
                    {
                        s_dispense_identified = true;
                        s_dispense_slot       = slot;
                        s_dispense_pill_count = patient_gallery[slot].pill_count;
                        strncpy(s_dispense_patient_name, patient_gallery[slot].name,
                                PATIENT_NAME_MAX - 1);
                        s_dispense_patient_name[PATIENT_NAME_MAX - 1] = '\0';
                        /* Stays blanked; STATE_CAPTURE_RESULT unblanks after
                         * it draws. See the note on the registration path. */
                        snprintf(s_capres_title, sizeof(s_capres_title),
                                 "WELCOME BACK, %s", s_dispense_patient_name);
                MS_DBG_PRINTF("Dispense: matched patient '%s' (%u pill(s) per dose).\n",
                               s_dispense_patient_name,
                               (unsigned)s_dispense_pill_count);
                        SD_Log_Event_Async("EVENT: Dispense - patient matched");

                        /* There used to be a "no pills remaining" check here that
                         * used to live here. It was reading a software stock
                         * counter that this device has no way to keep honest -
                         * nothing tells the firmware when a carer tops the
                         * hopper up. Real hopper-level sensing arrives
                         * with the IR break-beam counter, which
                         * measures pills physically dropping instead of
                         * assuming a number. */
                        s_capres_next   = STATE_DISPENSING;
                        current_state   = STATE_CAPTURE_RESULT;
                        state_init_done = false;
                    }
                    else
                    {
                        /* A face WAS found, it just is not in the gallery.
                         * Still logged as an intruder event (that is the
                         * security-relevant record), but the user now gets
                         * a retry it did not originally offer - a marginal
                         * match often succeeds on a second, better-framed
                         * attempt. */
                        MS_DBG_PRINTF("Dispense: face detected but no gallery match (intruder).\n");
                        SD_Log_Event_Async("EVENT: Dispense - INTRUDER (no gallery match)");
                        s_retry_target  = STATE_CAMERA_DISPENSE;
                        s_retry_message = "Sorry, I do not recognise you.\n\n"
                                          "Move a little closer and\n"
                                          "look straight at the camera.";
                        current_state   = STATE_FACE_RETRY;
                        state_init_done = false;
                    }
                }
                else
                {
                    MS_DBG_PRINTF("Dispense: no face detected after 3 attempts.\n");
                    SD_Log_Event_Async("EVENT: Dispense - no face detected");
                    s_retry_target  = STATE_CAMERA_DISPENSE;
                    s_retry_message = NO_FACE_MESSAGE;
                    current_state   = STATE_FACE_RETRY;
                    state_init_done = false;
                }
            }
            break;

        /* ── FACE RETRY ────────────────────────────────────── */
        /* A failed identification used to end with a 2.5 s dialog and an
         * unconditional return to the home screen - the user then had to walk
         * the whole flow again from the top to get a second attempt at
         * something that fails for entirely ordinary reasons (standing too
         * far back, looking away, poor light). This offers the retry
         * directly. It is an error path, not a new feature. */
        case STATE_FACE_RETRY:
            if (!state_init_done)
            {
                gui_draw_two_choice_screen("FACE NOT RECOGNISED",
                                           s_retry_message,
                                           COLOR_WARN,
                                           "TRY AGAIN", "CANCEL");
                /* This screen reserves its own box for the crying pose. */
                anime_ui_set_error_box(MASCOT_SAD_X, MASCOT_SAD_Y,
                                       MASCOT_SAD_W, MASCOT_SAD_H);
                /* The mascot has a real error state now - the
                 * designer's three-frame crying pose, animated in the box
                 * this screen reserves for it (MASCOT_SAD_*). The screen
                 * draws frame 0 itself; anime_ui takes over from here. */
                anime_ui_set_state(MASCOT_ERROR);
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;   /* force release before new touch */
                MS_DBG_PRINTF("STATE_FACE_RETRY\n");
                SD_Log_Event_Async("STATE: FACE_RETRY");
            }

            anime_ui_update(HAL_GetTick());

            if (new_touch && (HAL_GetTick() - state_entry_time > 500u))
            {
                if (check_hit(tx, ty, CHOICE_LEFT_X, CHOICE_BTN_Y,
                              CHOICE_BTN_W, CHOICE_BTN_H))
                {
                    anime_ui_set_state(MASCOT_IDLE);
                    current_state   = s_retry_target;
                    state_init_done = false;
                }
                else if (check_hit(tx, ty, CHOICE_RIGHT_X, CHOICE_BTN_Y,
                                   CHOICE_BTN_W, CHOICE_BTN_H))
                {
                    anime_ui_set_state(MASCOT_IDLE);
                    current_state   = STATE_HOME;
                    state_init_done = false;
                }
            }
            /* Never strand the device on this screen if nobody answers. */
            else if ((HAL_GetTick() - state_entry_time) > FACE_RETRY_TIMEOUT_MS)
            {
                MS_DBG_PRINTF("Face retry: timed out, returning home.\n");
                anime_ui_set_state(MASCOT_IDLE);
                current_state   = STATE_HOME;
                state_init_done = false;
            }
            break;

        /* ── ALERT ─────────────────────────────────────────── */
        /* One-button screen for conditions the user has to act on outside the
         * device: the gallery is full, a patient has no pills left, or the SD
         * card is not writable. Each of these previously either failed
         * silently or flashed past in a timed dialog. */
        case STATE_ALERT:
            if (!state_init_done)
            {
                gui_draw_alert_screen(s_alert_title, s_alert_message,
                                      s_alert_accent, "OK");
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_ALERT: %s\n", s_alert_title);
                /* Every state transition reaches the SD
                 * audit log, this one included. The specific condition
                 * (gallery full, refill needed, save failed) was already
                 * logged by whoever raised the alert. */
                SD_Log_Event_Async("STATE: ALERT");
            }

            if (new_touch && (HAL_GetTick() - state_entry_time > 500u) &&
                check_hit(tx, ty, ALERT_BTN_X, ALERT_BTN_Y,
                          ALERT_BTN_W, ALERT_BTN_H))
            {
                current_state   = s_alert_next;
                state_init_done = false;
            }
            else if ((HAL_GetTick() - state_entry_time) > ALERT_TIMEOUT_MS)
            {
                current_state   = s_alert_next;
                state_init_done = false;
            }
            break;

        /* ═══════════════════════════════════════════════════════════════════
         * The passcode gate and carer mode
         *
         * Every one of the states below is reachable ONLY through
         * STATE_PASSWORD, and STATE_PASSWORD is reachable from exactly two
         * places: the hidden title-bar gesture, and REGISTER PATIENT. There
         * is no third way in and no shortcut between them.
         *
         * The shape is the one registration_ui.c established
         * and this file has used ever since: the UI module owns drawing and
         * hit-testing, this file owns every transition. A carer screen is
         * therefore about ten lines here regardless of how much it draws.
         * ═══════════════════════════════════════════════════════════════════ */

        /* ── PASSWORD (shared gate, two callers) ────────────────────────── */
        case STATE_PASSWORD:
            if (!state_init_done)
            {
                camera_stop();
                carer_ui_draw_prompt();
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_PASSWORD (-> %d)\n", (int)s_pw_next);
                /* Deliberately logged for BOTH callers. An attempt to enter
                 * carer mode and an attempt to start a registration are both
                 * security-relevant, and the audit trail should show the
                 * attempt whether or not it succeeded. No digits are logged,
                 * here or anywhere. */
                SD_Log_Event_Async((s_pw_next == STATE_INSTRUCT_REGISTER)
                                   ? "STATE: PASSWORD (registration)"
                                   : "STATE: PASSWORD (carer mode)");
            }

            if (new_touch)
            {
                switch (carer_ui_handle_prompt_touch(tx, ty))
                {
                    case CARER_ACT_ACCEPTED:
                        SD_Log_Event_Async(
                            (s_pw_next == STATE_INSTRUCT_REGISTER)
                            ? "SECURITY: carer authorised a registration"
                            : "SECURITY: carer mode entered");
                        current_state   = s_pw_next;
                        state_init_done = false;
                        break;
                    case CARER_ACT_LOCKED_OUT:
                        SD_Log_Event_Async("SECURITY: passcode entry locked out");
                        break;
                    case CARER_ACT_REJECTED:
                        SD_Log_Event_Async("SECURITY: wrong carer passcode");
                        break;
                    case CARER_ACT_BACK:
                        current_state   = STATE_HOME;
                        state_init_done = false;
                        break;
                    default:
                        break;
                }
            }
            /* Never leave a passcode prompt on the screen unattended. */
            else if ((HAL_GetTick() - state_entry_time) > ALERT_TIMEOUT_MS)
            {
                current_state   = STATE_HOME;
                state_init_done = false;
            }
            break;

        /* ── CARER MENU ─────────────────────────────────────────────────── */
        case STATE_CARER_MENU:
            if (!state_init_done)
            {
                carer_ui_draw_menu();
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_CARER_MENU\n");
                SD_Log_Event_Async("STATE: CARER_MENU");
            }

            if (new_touch)
            {
                switch (carer_ui_handle_menu_touch(tx, ty))
                {
                    case CARER_ACT_GOTO_CLOCK:
                        carer_ui_begin_clock();
                        current_state = STATE_CARER_CLOCK;   state_init_done = false; break;
                    case CARER_ACT_GOTO_PATIENTS:
                        carer_ui_begin_patients();
                        current_state = STATE_CARER_PATIENTS; state_init_done = false; break;
                    case CARER_ACT_GOTO_LOG:
                        carer_ui_begin_log();
                        current_state = STATE_CARER_LOG;     state_init_done = false; break;
                    case CARER_ACT_GOTO_CHANGE_CODE:
                        carer_ui_begin_change_code();
                        current_state = STATE_CARER_CHANGE_CODE; state_init_done = false; break;
                    case CARER_ACT_BACK:
                        /* Leaving carer mode re-arms the schedule, because
                         * anything the carer just changed - the clock, a dose
                         * time, a deleted patient - can have changed which
                         * dose comes next. Doing it once here rather than at
                         * every SAVE keeps the alarm's re-arm on one path. */
                        SD_Log_Event_Async("SECURITY: carer mode left");
                        schedule_arm_next();
                        current_state   = STATE_HOME;
                        state_init_done = false;
                        break;
                    default:
                        break;
                }
            }
            break;

        /* ── CARER: set the clock ───────────────────────────────────────── */
        case STATE_CARER_CLOCK:
            if (!state_init_done)
            {
                carer_ui_draw_clock();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_CLOCK\n");
            }

            if (new_touch && !s_confirm_done)
            {
                carer_action_t a = carer_ui_handle_clock_touch(tx, ty);
                if (a == CARER_ACT_ACCEPTED)
                {
                    gui_draw_dialog_text("Clock set.");
                    s_confirm_done   = true;
                    state_entry_time = HAL_GetTick();
                }
                else if (a == CARER_ACT_REJECTED)
                {
                    show_alert("CLOCK NOT SET",
                               "The clock could not be written.\n\n"
                               "The RTC may not be running.",
                               COLOR_ALERT, STATE_CARER_MENU);
                }
                else if (a == CARER_ACT_BACK)
                {
                    current_state   = STATE_CARER_MENU;
                    state_init_done = false;
                }
            }

            if (s_confirm_done && (HAL_GetTick() - state_entry_time > 1200u))
            {
                s_confirm_done  = false;
                /* A new clock means every armed window is against the old
                 * one. Re-arm immediately rather than waiting for the carer
                 * to leave carer mode - this is the one edit where a stale
                 * alarm would fire at visibly the wrong time. */
                schedule_arm_next();
                current_state   = STATE_CARER_MENU;
                state_init_done = false;
            }
            break;

        /* ── CARER: change the passcode ─────────────────────────────────── */
        case STATE_CARER_CHANGE_CODE:
            if (!state_init_done)
            {
                carer_ui_draw_change_code();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_CHANGE_CODE\n");
            }

            if (new_touch && !s_confirm_done)
            {
                carer_action_t a = carer_ui_handle_change_code_touch(tx, ty);
                if (a == CARER_ACT_ACCEPTED)
                {
                    gui_draw_dialog_text("New code saved.");
                    s_confirm_done   = true;
                    state_entry_time = HAL_GetTick();
                }
                else if (a == CARER_ACT_BACK)
                {
                    current_state   = STATE_CARER_MENU;
                    state_init_done = false;
                }
                /* CARER_ACT_REJECTED is handled entirely inside the module:
                 * either the two entries disagreed, in which case it has
                 * already redrawn itself saying so, or the card write failed
                 * - and in that second case the new code IS live for this
                 * power cycle, which the screen below states rather than
                 * hides. */
                else if (a == CARER_ACT_REJECTED && !SD_Logger_Is_Available())
                {
                    show_alert("CODE NOT SAVED",
                               "The new code works now, but there is\n"
                               "no SD card to remember it on.\n\n"
                               "It reverts on the next restart.",
                               COLOR_WARN, STATE_CARER_MENU);
                }
            }

            if (s_confirm_done && (HAL_GetTick() - state_entry_time > 1200u))
            {
                s_confirm_done  = false;
                current_state   = STATE_CARER_MENU;
                state_init_done = false;
            }
            break;

        /* ── CARER: the patient list ────────────────────────────────────── */
        case STATE_CARER_PATIENTS:
            if (!state_init_done)
            {
                carer_ui_draw_patients();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_PATIENTS\n");
            }

            if (new_touch)
            {
                carer_action_t a = carer_ui_handle_patients_touch(tx, ty);
                if (a == CARER_ACT_PATIENT_PICKED)
                {
                    current_state   = STATE_CARER_PATIENT;
                    state_init_done = false;
                }
                else if (a == CARER_ACT_BACK)
                {
                    current_state   = STATE_CARER_MENU;
                    state_init_done = false;
                }
            }
            break;

        /* ── CARER: one patient ─────────────────────────────────────────── */
        case STATE_CARER_PATIENT:
            if (!state_init_done)
            {
                carer_ui_draw_patient_menu();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_PATIENT\n");
            }

            if (new_touch)
            {
                switch (carer_ui_handle_patient_menu_touch(tx, ty))
                {
                    case CARER_ACT_GOTO_SCHEDULE:
                        carer_ui_begin_schedule();
                        current_state = STATE_CARER_SCHEDULE; state_init_done = false; break;
                    case CARER_ACT_GOTO_DOSE:
                        carer_ui_begin_dose();
                        current_state = STATE_CARER_DOSE;     state_init_done = false; break;
                    case CARER_ACT_GOTO_DELETE:
                        current_state = STATE_CARER_DELETE_CONFIRM; state_init_done = false; break;
                    case CARER_ACT_BACK:
                        carer_ui_begin_patients();   /* re-read: a save may have changed the summaries */
                        current_state   = STATE_CARER_PATIENTS;
                        state_init_done = false;
                        break;
                    default:
                        break;
                }
            }
            break;

        /* ── CARER: dose times ──────────────────────────────────────────── */
        case STATE_CARER_SCHEDULE:
            if (!state_init_done)
            {
                carer_ui_draw_schedule();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_SCHEDULE\n");
            }

            if (new_touch && !s_confirm_done)
            {
                carer_action_t a = carer_ui_handle_schedule_touch(tx, ty);
                if (a == CARER_ACT_ACCEPTED)
                {
                    gui_draw_dialog_text("Dose times saved.");
                    s_confirm_done   = true;
                    state_entry_time = HAL_GetTick();
                }
                else if (a == CARER_ACT_REJECTED)
                {
                    /* Same honesty rule as for
                     * registration: an edit that reached RAM but not the card
                     * is not "saved", and the screen has to say which of the
                     * two happened. */
                    show_alert("NOT SAVED TO CARD",
                               "The new times work now, but there is\n"
                               "no SD card to remember them on.\n\n"
                               "They revert on the next restart.",
                               COLOR_WARN, STATE_CARER_PATIENT);
                }
                else if (a == CARER_ACT_BACK)
                {
                    current_state   = STATE_CARER_PATIENT;
                    state_init_done = false;
                }
            }

            if (s_confirm_done && (HAL_GetTick() - state_entry_time > 1200u))
            {
                s_confirm_done  = false;
                current_state   = STATE_CARER_PATIENT;
                state_init_done = false;
            }
            break;

        /* ── CARER: dose size ───────────────────────────────────────────── */
        case STATE_CARER_DOSE:
            if (!state_init_done)
            {
                carer_ui_draw_dose();
                state_init_done = true;
                was_touching    = true;
                MS_DBG_PRINTF("STATE_CARER_DOSE\n");
            }

            if (new_touch && !s_confirm_done)
            {
                carer_action_t a = carer_ui_handle_dose_touch(tx, ty);
                if (a == CARER_ACT_ACCEPTED)
                {
                    gui_draw_dialog_text("Dose size saved.");
                    s_confirm_done   = true;
                    state_entry_time = HAL_GetTick();
                }
                else if (a == CARER_ACT_REJECTED)
                {
                    show_alert("NOT SAVED TO CARD",
                               "The new dose works now, but there is\n"
                               "no SD card to remember it on.\n\n"
                               "It reverts on the next restart.",
                               COLOR_WARN, STATE_CARER_PATIENT);
                }
                else if (a == CARER_ACT_BACK)
                {
                    current_state   = STATE_CARER_PATIENT;
                    state_init_done = false;
                }
            }

            if (s_confirm_done && (HAL_GetTick() - state_entry_time > 1200u))
            {
                s_confirm_done  = false;
                current_state   = STATE_CARER_PATIENT;
                state_init_done = false;
            }
            break;

        /* ── CARER: review the log ──────────────────────────────────────── */
        /* The strongest thing in carer mode. The audit trail has existed
         * from the start, and until now reading it meant taking the card to
         * a laptop. */
        case STATE_CARER_LOG:
            if (!state_init_done)
            {
                carer_ui_draw_log();
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_CARER_LOG\n");
                SD_Log_Event_Async("CARER: reviewed the dose log");
            }

            if (new_touch && (carer_ui_handle_log_touch(tx, ty) == CARER_ACT_BACK))
            {
                /* Back to wherever it was opened from: the patient menu if a
                 * patient is selected, the carer menu otherwise. */
                current_state   = (carer_ui_selected_slot() >= 0)
                                  ? STATE_CARER_PATIENT : STATE_CARER_MENU;
                state_init_done = false;
            }
            break;

        /* ── CARER: delete a patient ────────────────────────────────────── */
        /* the technical reference records that an on-device delete
         * of biometric data was described in early drafts, argued against in
         * that same document, and deliberately never built - because it had
         * no authentication in front of it. Behind the passcode it becomes
         * the right thing to have, and it is the only way a patient's face
         * embedding can be removed short of wiping the card. */
        case STATE_CARER_DELETE_CONFIRM:
            if (!state_init_done)
            {
                {
                    static char msg[128];
                    snprintf(msg, sizeof(msg),
                             "Remove %s completely?\n\n"
                             "Their name and their face\n"
                             "are both deleted.",
                             carer_ui_selected_name());
                    gui_draw_two_choice_screen("DELETE PATIENT", msg,
                                               COLOR_ALERT, "DELETE", "KEEP");
                }
                anime_ui_set_error_box(MASCOT_SAD_X, MASCOT_SAD_Y,
                                       MASCOT_SAD_W, MASCOT_SAD_H);
                anime_ui_set_state(MASCOT_ERROR);
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;
                MS_DBG_PRINTF("STATE_CARER_DELETE_CONFIRM\n");
            }

            anime_ui_update(HAL_GetTick());

            if (new_touch && (HAL_GetTick() - state_entry_time > 500u))
            {
                if (check_hit(tx, ty, CHOICE_LEFT_X, CHOICE_BTN_Y,
                              CHOICE_BTN_W, CHOICE_BTN_H))
                {
                    int  slot = carer_ui_selected_slot();
                    /* The name is captured BEFORE the delete, because after
                     * it there is deliberately nothing left to read. */
                    char line[SD_LOG_MSG_MAX_LEN];
                    snprintf(line, sizeof(line), "CARER: deleted patient %s",
                             carer_ui_selected_name());
                    bool ok = gallery_delete_patient(slot);
                    SD_Log_Event_Async(ok ? line
                                          : "CARER: deleted a patient - RAM only (no SD)");

                    anime_ui_set_state(MASCOT_IDLE);
                    carer_ui_begin_patients();   /* the list is now shorter   */
                    schedule_arm_next();         /* their doses are now gone  */
                    current_state   = STATE_CARER_PATIENTS;
                    state_init_done = false;
                }
                else if (check_hit(tx, ty, CHOICE_RIGHT_X, CHOICE_BTN_Y,
                                   CHOICE_BTN_W, CHOICE_BTN_H))
                {
                    anime_ui_set_state(MASCOT_IDLE);
                    current_state   = STATE_CARER_PATIENT;
                    state_init_done = false;
                }
            }
            else if ((HAL_GetTick() - state_entry_time) > FACE_RETRY_TIMEOUT_MS)
            {
                /* Timing out on a destructive confirmation means KEEP. */
                anime_ui_set_state(MASCOT_IDLE);
                current_state   = STATE_CARER_PATIENT;
                state_init_done = false;
            }
            break;

        /* ── DISPENSING (simulated dispense animation) ──────────────────── */
        case STATE_DISPENSING:
            if (!state_init_done)
            {
                state_init_done = true;
                MS_DBG_PRINTF("STATE_DISPENSING\n");
                SD_Log_Event_Async("STATE: DISPENSING");

                /* The "DISPENSE: <name> <n> pills" line used to be
                 * written HERE, before anything had been dispensed - which was
                 * harmless while the dispense was a 2 s animation that could
                 * not fail, and is not harmless now that it can come up short
                 * or jam. The audit log must
                 * never contradict itself. The line is written after the fact
                 * instead, and says what actually happened. */

                gui_draw_dispensing_screen(s_dispense_patient_name, s_dispense_pill_count);

                /* Draw what the face detector actually saw - the
                 * frozen crop it ran on, its chosen box and the five
                 * landmarks. Safe HERE and not a moment earlier: the capture
                 * handshake has completed, so the AI task has released
                 * BUFFER_ADDRESS. It is on the dispensing screen rather than
                 * its own because that screen already pauses for the progress
                 * animation, so the evidence costs the patient no extra time. */

                /* Unblank HERE, not at the bottom of this
                 * function. This state does all its work inside its own entry
                 * block and clears state_init_done again before it breaks, so
                 * the end-of-function unblank (which waits for
                 * state_init_done) never fired for it - the whole dispensing
                 * screen and its two-second progress animation ran behind a
                 * disabled LTDC layer and the user simply never saw the
                 * progress bar. The screen is on the framebuffer by this
                 * line, so it is safe to show it. */
                display_blank(false);

                /* ~2s animated countdown - a filled-bar progress wipe, per
                 * the plan ("a progress bar is fine"). Blocking here is
                 * consistent with this file's existing pattern of blocking
                 * for known, bounded operations (e.g. the capture retry
                 * loops above).
                 *
                 * ONE STEP PER PILL. This ran 1..10 regardless of
                 * the dose, so a 6-pill dose showed a percentage marching
                 * 10, 20, 30... that had nothing to do with the six pills.
                 * A first fix kept four smooth sub-steps inside each pill's
                 * slice, which was still wrong for the same reason at a
                 * smaller scale: at a 2-pill dose the number still ticked
                 * through eight values for two pills.
                 *
                 * The bar is a report on a physical event, not a spinner.
                 * One pill leaves the hopper, one gem lights, the bar and
                 * the number move once. At pill 1 of 2 the bar is exactly
                 * half full and reads 50%, and it holds there until the
                 * second pill drops. When a real IR break-beam count replaces this
                 * dwell, the loop body is already
                 * the right shape - only the wait changes. */
#if MEDSIGHT_PHYSICAL_DISPENSER
                /* ── The real hopper ──────────────────────────
                 *
                 * The comment above predicted this replacement exactly: "only
                 * the wait changes". It does. The bar still moves once per
                 * pill, but a pill is now a physical object that broke an IR
                 * beam rather than a timer expiring, and the loop can now end
                 * three ways instead of one.
                 *
                 * Blocking the UI task here is the same choice this state has
                 * always made, and it survives the "dispenser task, or not"
                 * question on its own evidence.
                 * The NPU moved into its own task because it
                 * blocked opaquely for hundreds of milliseconds with the CPU
                 * inside a single call. This loop is the opposite shape: it
                 * yields to the scheduler every MS_STEP_PERIOD_MS through
                 * osal_delay_ms(), so cam_isp at priority 5 still preempts it
                 * on schedule and the logger still drains. Adding a sixth task
                 * to an eight-slot table to wrap a loop that already yields
                 * would buy nothing and cost a second synchronisation
                 * mechanism where Part B item 2 explicitly asks for none.
                 *
                 * Touch is not polled during the dispense - as it was not
                 * before - and there is nothing on this screen to touch. */
                {
                    uint8_t counted = 0;

                    dispenser_set_progress_cb(dispensing_progress_cb);
                    dispense_result_t dr =
                        dispenser_dispense(s_dispense_pill_count, &counted);
                    dispenser_set_progress_cb(NULL);

                    char log_line[SD_LOG_MSG_MAX_LEN];
                    snprintf(log_line, sizeof(log_line),
                             "DISPENSE: %s %u requested, %u counted (%s)",
                             s_dispense_patient_name,
                             (unsigned)s_dispense_pill_count,
                             (unsigned)counted,
                             dispenser_result_str(dr));
                    SD_Log_Event_Async(log_line);
                    MS_DBG_PRINTF("%s\n", log_line);

                    /* Part E, and the Program Plan's own wording: "green =
                     * correct, red = incorrect/missed". Two short beeps for a
                     * dose that came out, four rapid ones for a dose that did
                     * not - audible from across the room without reading the
                     * screen, which is the point for this device's users. */
                    buzzer_play((dr == DISPENSE_OK) ? BUZZ_DISPENSE_OK
                                                    : BUZZ_DISPENSE_FAIL);

                    if (dr == DISPENSE_OK)
                    {
                        current_state   = STATE_CONFIRM_TAKEN;
                        state_init_done = false;
                    }
                    else if (dr == DISPENSE_NOT_READY)
                    {
                        /* The driver never came up. Not the patient's problem
                         * to solve, and not a reason to certify a dose. */
                        show_alert("DISPENSER NOT READY",
                                   "The pill dispenser did not start.\n\n"
                                   "Please tell your carer.",
                                   COLOR_ALERT, STATE_HOME);
                    }
                    else if (dr == DISPENSE_SHORT)
                    {
                        /* The software stock counter was removed because
                         * the firmware had no way to know the hopper level.
                         * This is that signal, finally measured: the actuator
                         * ran a full cycle and fewer pills came out than were
                         * asked for. */
                        SD_Log_Event_Async("REFILL: hopper may be empty");
                        show_alert("NOT ENOUGH PILLS",
                                   "Fewer pills came out than expected.\n\n"
                                   "Please tell your carer - the hopper\n"
                                   "may need refilling.",
                                   COLOR_ALERT, STATE_HOME);
                    }
                    else /* DISPENSE_JAM */
                    {
                        show_alert("DISPENSER JAMMED",
                                   "I could not release your pills.\n\n"
                                   "Please tell your carer.",
                                   COLOR_ALERT, STATE_HOME);
                    }
                }
#else
                /* ── The simulation, kept working ──────────
                 * With no hardware attached the device still runs the whole
                 * flow. This is what makes the project demonstrable while the
                 * mechanism is on a bench rather than in an enclosure. */
                {
                    uint32_t pills = (s_dispense_pill_count > 0u)
                                     ? s_dispense_pill_count : 1u;
                    /* Time per pill: ~2 s of travel spread over the dose,
                     * but never so brief that a step cannot be read, nor so
                     * long that a single-pill dose feels stalled. */
                    uint32_t dwell = 2000u / pills;
                    if (dwell < 300u)  dwell = 300u;
                    if (dwell > 900u)  dwell = 900u;

                    for (uint32_t pill = 1; pill <= pills; pill++)
                    {
                        osal_delay_ms(dwell);   /* the pill falls... */
                        gui_draw_dispensing_progress(
                            (uint16_t)(pill * 100u / pills), (uint8_t)pill);
                    }

                    char log_line[SD_LOG_MSG_MAX_LEN];
                    snprintf(log_line, sizeof(log_line),
                             "DISPENSE: %s %u pills (simulated)",
                             s_dispense_patient_name,
                             (unsigned)s_dispense_pill_count);
                    SD_Log_Event_Async(log_line);
                }

                current_state   = STATE_CONFIRM_TAKEN;
                state_init_done = false;
#endif /* MEDSIGHT_PHYSICAL_DISPENSER */
            }
            break;

        /* ── CONFIRM TAKEN ("I Took It" / "Skip") ───────────────────────── */
        case STATE_CONFIRM_TAKEN:
            if (!state_init_done)
            {
                gui_draw_confirm_taken_screen();
                state_init_done  = true;
                state_entry_time = HAL_GetTick();
                was_touching     = true;   /* force release before new touch */
                MS_DBG_PRINTF("STATE_CONFIRM_TAKEN\n");
                SD_Log_Event_Async("STATE: CONFIRM_TAKEN");

                /* Start watching for the pill going to the mouth.
                 *
                 * This is the ONLY screen in the device where the camera runs
                 * while the UI keeps drawing, and it is possible only because
                 * intake_camera_start() points the DCMIPP at PSRAM instead of
                 * the framebuffer (Inc/ai/intake_camera.h). The screen above
                 * has already been drawn into BUFFER_ADDRESS and stays there
                 * untouched - this call cannot disturb it.
                 *
                 * Nothing below waits on the result. */
                intake_begin(s_dispense_pill_count);
            }

            if (!s_taken_ack_shown && new_touch)
            {
                if (check_hit(tx, ty, TAKEN_BTN_X, TAKEN_BTN_Y, TAKEN_BTN_W, TAKEN_BTN_H))
                {
                    char log_line[SD_LOG_MSG_MAX_LEN];
                    /* A dose taken inside its scheduled window is
                     * recorded AGAINST that window, not merely as "a dose
                     * happened". That difference is the whole of adherence:
                     * "took their 08:00 pills" and "took some pills at some
                     * point" answer different questions, and only the first
                     * one is worth reviewing. */
                    /* The model's verdict is read HERE, at the
                     * moment of the tap, and it only ever chooses a SUFFIX.
                     * The button has already decided that a dose happened.
                     *
                     * intake_verdict_text() returns "gesture confirmed",
                     * "gesture NOT observed" or "gesture uncertain" - and the
                     * third is the honest answer whenever the watch was still
                     * mid-sequence, the detector was unavailable, or the
                     * camera never started. None of those can prevent this
                     * line being written. */
#if MEDSIGHT_ACTION_RECOGNITION
                    const char *verdict = intake_verdict_text(intake_peek());
#else
                    const char *verdict = NULL;
#endif
                    if ((s_due_slot >= 0) && !s_due_served &&
                        (s_due_slot == s_dispense_slot))
                    {
                        char hhmm[8];
                        time_source_format_hhmm(hhmm, sizeof(hhmm), s_due_minute);
                        if (verdict != NULL) {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED: %s took the %s dose (on time, %s)",
                                     s_dispense_patient_name, hhmm, verdict);
                        } else {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED: %s took the %s dose (on time)",
                                     s_dispense_patient_name, hhmm);
                        }
                        s_due_served = true;
                    }
                    else if ((s_missed_slot >= 0) &&
                             (s_missed_slot == s_dispense_slot))
                    {
                        /* LATE, not unattributed. The window closed before
                         * they tapped, so a MISSED line is already in the log
                         * above - name the same dose and say this supersedes
                         * it, rather than writing an unrelated-looking "took
                         * pills" that leaves a carer to guess. */
                        char hhmm[8];
                        time_source_format_hhmm(hhmm, sizeof(hhmm),
                                                 s_missed_minute);
                        if (verdict != NULL) {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED LATE: %s took the %s dose (supersedes MISSED; %s)",
                                     s_dispense_patient_name, hhmm, verdict);
                        } else {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED LATE: %s took the %s dose (supersedes MISSED)",
                                     s_dispense_patient_name, hhmm);
                        }
                        s_missed_slot = -1;
                        /* The sad face has been answered. */
                        s_sad_until = 0u;
                        anime_ui_set_state(MASCOT_IDLE);
                    }
                    else
                    {
                        if (verdict != NULL) {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED: %s took pills (%s)",
                                     s_dispense_patient_name, verdict);
                        } else {
                            snprintf(log_line, sizeof(log_line),
                                     "CONFIRMED: %s took pills",
                                     s_dispense_patient_name);
                        }
                    }
                    SD_Log_Event_Async(log_line);
                    intake_end();

                    /* Nothing is written to the
                     * gallery here any more.
                     *
                     * Earlier builds decremented a `pills_remaining` field and
                     * re-saved patients.dat on every confirmed dose. That was
                     * wrong - pill_count is the DOSE, not a stock level (see
                     * ai_vision.h) - and it also meant a full 1.6 KB card
                     * write on the patient's most latency-sensitive tap, for
                     * data that never needed to change. The dose itself is
                     * still recorded, in the append-only event log above,
                     * which is where an adherence record belongs.
                     *
                     * The patient record is now written exactly once, at
                     * registration. */

                    gui_draw_taken_thankyou_screen();
                    s_taken_ack_shown = true;
                    state_entry_time  = HAL_GetTick();
                }
                else if (check_hit(tx, ty, SKIP_BTN_X, SKIP_BTN_Y, SKIP_BTN_W, SKIP_BTN_H))
                {
                    /* Caretaker skip. The plan's literal instruction was
                     * "returns home without logging", which was first read as
                     * writing nothing at all. The audit
                     * log review revised that: a dose that was dispensed and
                     * then NOT confirmed is exactly the kind of thing an
                     * adherence audit trail exists to record, and an audit
                     * log with a silent hole in it is worse than one with an
                     * inconvenient entry. No CONFIRMED line is written - the
                     * prompt's actual requirement - but the skip itself is. */
                    MS_DBG_PRINTF("Dispense: consumption confirmation skipped.\n");
                    SD_Log_Event_Async("EVENT: Dispense - confirmation SKIPPED (caretaker)");
                    /* Stop the watch on every exit from this
                     * screen, not just the confirming one. A camera left
                     * streaming into PSRAM after the UI has moved on burns
                     * power for a result nobody will read. */
                    intake_end();
                    current_state   = STATE_HOME;
                    state_init_done = false;
                }
            }

            if (s_taken_ack_shown && (HAL_GetTick() - state_entry_time > 1500u))
            {
                s_taken_ack_shown = false;
                current_state     = STATE_HOME;
                state_init_done   = false;
            }
            else if (!s_taken_ack_shown &&
                     (HAL_GetTick() - state_entry_time > CONFIRM_TAKEN_TIMEOUT_MS))
            {
                /* Missed consumption confirmation - neither button tapped
                 * within the timeout. Per the technical reference's
                 * edge-case table: log and return home. */
                MS_DBG_PRINTF("Dispense: consumption confirmation timed out.\n");
                SD_Log_Event_Async("EVENT: Dispense - missed confirmation (timeout)");
                intake_end();
                current_state   = STATE_HOME;
                state_init_done = false;
            }
            break;
    }

    /* Retire the overlay on any state change, so a preview can
     * never be inherited by the next screen. Same discipline as the carer
     * clock strip, and for the same reason. */
    if (!state_init_done)
    {
        ai_overlay_reset();
    }

    /* Advance the live clock strip, if the screen that is up has
     * one. It redraws only when the displayed text actually changes - about
     * once a real second in demo mode, once a minute otherwise - and flushes
     * only that band, so this costs almost nothing on the other 99 ticks.
     *
     * Guarded on the capture: between ai_vision_capture_request() and its
     * result the AI task owns the framebuffer and NOTHING in this file may
     * draw. The capture states do not show a strip, so s_strip_y is already
     * 0 there, but the guard is cheap and the invariant is worth stating at
     * every site that could break it. */
    if (!s_cap_in_flight && !s_display_blanked)
    {
        (void)carer_ui_clock_tick();

        /* The live intake overlay.
         *
         * GATED ON THE SCREEN, not merely on the subsystem being busy. It
         * used to run whenever intake_is_active() was true, and on hardware
         * that painted the camera pane over the HOME screen after a dose
         * timed out.
         *
         * The reason is that intake_end() only REQUESTS a stop. The AI task
         * notices when it next comes round its loop, and a hand inference
         * takes ~305 ms, so for up to a third of a second after the UI has
         * moved on the watch is still active and still publishing. Anything
         * keyed on "is the watch running" will draw during that window.
         *
         * The pane belongs to ONE SCREEN, so that is what it is tied to. A
         * subsystem's liveness is not a licence to draw. */
        if (current_state == STATE_CONFIRM_TAKEN) {
            (void)ai_overlay_tick();
        }
    }

    /* Unblank once the capture is finished AND the state that follows it has
     * drawn itself. state_init_done is exactly that signal: it is set at the
     * end of every state's entry block, after its screen is on the
     * framebuffer. Unblanking any earlier would show the corrupted frame the
     * NPU left behind, which is the whole thing this is avoiding.
     *
     * CAP_SETTLE HAS TO BE EXCLUDED, and missing it was the last of the three
     * camera flashes. During the settle window every condition here is true -
     * the capture has not started so s_cap_in_flight is false, and the camera
     * state entered long ago so state_init_done is true - while the DCMIPP is
     * writing all 800x480 of the framebuffer to give the NPU a fresh frame.
     * This line then dutifully turned the panel back on and showed it.
     *
     * The deeper lesson: this condition is trying to express "is anything
     * other than the UI writing the framebuffer right now?", and
     * s_cap_in_flight only ever covered the NPU half of that. The camera half
     * arrived with the live preview and had to be added by hand. */
    if (s_display_blanked && !s_cap_in_flight &&
        (s_cap_phase != CAP_SETTLE) && state_init_done)
    {
        display_blank(false);
    }
}
