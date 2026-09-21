#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATE_HOME,
    STATE_INSTRUCT_REGISTER,
    STATE_CAMERA_REGISTER,
    STATE_KEYBOARD_REGISTER,    /* On-screen name entry */
    STATE_CAPTURE_RESULT,       /* "what the camera saw" + NEXT */
    STATE_PILLCOUNT_REGISTER,   /* Daily pill-count entry */
    STATE_CONFIRM_REGISTER,     /* Summary + confirm/retry */
    STATE_INSTRUCT_DISPENSE,
    STATE_CAMERA_DISPENSE,
    STATE_DISPENSING,           /* Simulated dispense animation/countdown */
    STATE_CONFIRM_TAKEN,        /* "I Took It" / "Skip" confirmation screen */
    /* Hardening states. Both are error/alert paths for cases the
     * earlier flows ended in a dead end or handled only in a
     * timed dialog - not new functionality. */
    STATE_FACE_RETRY,           /* "Face not recognised" -> TRY AGAIN / CANCEL */
    STATE_ALERT,                /* one-button alert (gallery full, refill, SD) */

    /* ── The passcode gate and carer mode ──────────────────────
     * STATE_PASSWORD is reached from TWO places and remembers which: the
     * hidden title-bar gesture on the home screen (carer mode), and
     * REGISTER PATIENT (B2a - closing the open-enrolment hole). One screen,
     * one validation routine, two entry points; see ui/carer_ui.h.
     *
     * Everything from STATE_CARER_MENU down is reachable ONLY through
     * STATE_PASSWORD. There is no other transition into any of them. */
    STATE_PASSWORD,
    STATE_CARER_MENU,
    STATE_CARER_CLOCK,
    STATE_CARER_CHANGE_CODE,
    STATE_CARER_PATIENTS,
    STATE_CARER_PATIENT,        /* one patient: schedule / dose / delete     */
    STATE_CARER_SCHEDULE,
    STATE_CARER_DOSE,
    STATE_CARER_LOG,
    STATE_CARER_DELETE_CONFIRM
} AppState_t;

void state_machine_init(void);
void state_machine_update(void);

/* Create the dose-schedule alarm and its event flag. Call once
 * from main(), alongside ai_vision_service_init() and the other
 * osal_*_create() calls, BEFORE osal_scheduler_start(). */
void state_machine_service_init(void);

#ifdef __cplusplus
}
#endif

#endif // STATE_MACHINE_H
