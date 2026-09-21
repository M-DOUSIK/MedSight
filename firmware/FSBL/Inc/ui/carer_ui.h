/* ui/carer_ui.h - The passcode gate and carer mode
 *
 * Two things live in this module, and they live together on purpose.
 *
 * 1. THE PASSCODE GATE. One prompt screen, one validation routine, two entry
 *    points: carer mode itself, and REGISTER PATIENT (section B2a). The design
 *    brief is explicit about why they must be the same code - "two copies of
 *    a password check is two places to get the comparison, the rate-limiting,
 *    or the hash wrong" - and it is right. There is exactly one
 *    carer_pass_check() in this firmware.
 *
 * 2. CARER MODE. The screens a carer uses to set the device up: the clock,
 *    each patient's dose schedule and dose size, the event log, and deleting
 *    a patient.
 *
 * ── Why REGISTER PATIENT is now gated (B2a) ──────────────────────────────
 *
 * This is the most important security change in the project and it closes a
 * real hole, not a theoretical one. Until now anyone could walk up to the
 * device, tap REGISTER PATIENT, enrol their own face and their own dose
 * size, and then use DISPENSE to be handed medication - and the audit log
 * would record it as a legitimate, face-matched dispense to a registered
 * patient, because from the device's point of view that is exactly what it
 * was. Face recognition was doing its job perfectly. The gallery it matched
 * against would accept anyone who asked.
 *
 * Every other control in this device rests on that gallery being
 * trustworthy. With enrolment open, identification is theatre.
 *
 * The gate is at the START of registration, not the end. Asking afterwards
 * would waste the user's time, would mean a face and a name had already been
 * taken from someone who was never authorised, and would leave a half-written
 * flow to unwind. Ask first and everything downstream is already authorised.
 *
 * ── What the passcode actually protects against ──────────────────────────
 *
 * A curious patient or a visitor. Nothing more, and the documents say so:
 * this board has no secure element, the SD card is unencrypted, and the hash
 * below is a 32-bit FNV-1a - it would not survive five minutes of attention
 * from someone holding the card. That honesty is worth more than the feature
 * (the technical reference).
 */
#ifndef CARER_UI_H
#define CARER_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── The build-time default passcode ─────────────────────────────────────
 *
 * A fresh device with no card, or a card with no carer.cfg on it, has to be
 * usable - otherwise the first thing a carer must do requires the thing they
 * cannot yet do. So there is a default, it is documented in the README, and
 * carer mode can change it (the new value's hash is written to the card).
 *
 * Digits only. A numeric passcode rather than a typed word is a deliberate
 * choice for this device: the keypad's targets are four times the area of
 * the QWERTY keys registration uses, which matters when the person holding
 * the device may be a carer in a hurry or an elderly user's relative, and
 * nothing about a word is more secure than digits against the threat model
 * above. */
#ifndef MEDSIGHT_DEFAULT_CARER_CODE
#define MEDSIGHT_DEFAULT_CARER_CODE  "1234"
#endif

#define CARER_CODE_MIN_DIGITS   4u
#define CARER_CODE_MAX_DIGITS   8u

/* ── Entry gesture (see carer_ui.c for the reasoning behind the numbers) ── */
#define CARER_GESTURE_TAPS      5u
#define CARER_GESTURE_WINDOW_MS 3000u

/* ── What a touch on a carer screen did ──────────────────────────────────
 * state_machine.c owns every transition, exactly as it does for
 * registration_ui.c. This module draws, hit-tests, and reports. */
typedef enum {
    CARER_ACT_NONE = 0,     /* tap hit nothing that matters                 */
    CARER_ACT_BACK,         /* leave this screen for the one above it       */
    CARER_ACT_ACCEPTED,     /* passcode correct / value committed           */
    CARER_ACT_REJECTED,     /* passcode wrong - screen stays up, says so    */
    CARER_ACT_LOCKED_OUT,   /* too many wrong attempts, refusing for a while*/
    CARER_ACT_GOTO_CLOCK,
    CARER_ACT_GOTO_PATIENTS,
    CARER_ACT_GOTO_LOG,
    CARER_ACT_GOTO_CHANGE_CODE,
    CARER_ACT_GOTO_SCHEDULE,
    CARER_ACT_GOTO_DOSE,
    CARER_ACT_GOTO_DELETE,
    CARER_ACT_PATIENT_PICKED  /* carer_ui_selected_slot() says which        */
} carer_action_t;

/* ── Passcode store ─────────────────────────────────────────────────────── */

/** Load the passcode hash from the SD card, or fall back to the build-time
 *  default. Call once at start-up, after the gallery is loaded. */
void carer_pass_init(void);

/** True if the stored passcode is still the build-time default - the README
 *  and the home screen both have a reason to say so. */
bool carer_pass_is_default(void);

/* ── The shared passcode prompt (used by carer mode AND registration) ───── */

/** Which entry point is asking. The wording on the screen differs: a patient
 *  who pressed REGISTER PATIENT by mistake should be told a carer needs to
 *  set this up for them, not "access denied". */
typedef enum {
    CARER_PROMPT_CARER_MODE = 0,
    CARER_PROMPT_REGISTRATION
} carer_prompt_reason_t;

/** Reset the prompt (clears entered digits) and set its wording. Call on
 *  entry to the password state. */
void carer_ui_prompt_begin(carer_prompt_reason_t reason);

void carer_ui_draw_prompt(void);
/** Rising-edge touches only. Returns ACCEPTED / REJECTED / LOCKED_OUT /
 *  BACK / NONE. The screen redraws itself on REJECTED and LOCKED_OUT. */
carer_action_t carer_ui_handle_prompt_touch(uint32_t tx, uint32_t ty);

/* ── The live clock strip ────────────────────────────────────────────────
 *
 * After the first hardware round. The clock was drawn once on
 * screen entry and never again, so in demo mode - where a simulated minute
 * passes every real second - it was visibly frozen. A frozen clock on a
 * screen whose entire job is scheduling is worse than no clock at all.
 *
 * Any screen may draw one; state_machine.c calls the tick every UI pass and
 * it redraws only when the text changes, flushing only that band. */

/** Draw the clock/status strip at `y` and arm it for ticking. */
void carer_ui_draw_clock_strip(uint16_t y);
/** Stop ticking (call when leaving a screen that showed one). */
void carer_ui_clock_strip_hide(void);
/** Redraw the strip if its text changed. Cheap; safe to call every tick. */
bool carer_ui_clock_tick(void);

/* ── Carer menu ─────────────────────────────────────────────────────────── */
void carer_ui_draw_menu(void);
carer_action_t carer_ui_handle_menu_touch(uint32_t tx, uint32_t ty);

/* ── Set clock ──────────────────────────────────────────────────────────── */
void carer_ui_begin_clock(void);
void carer_ui_draw_clock(void);
carer_action_t carer_ui_handle_clock_touch(uint32_t tx, uint32_t ty);

/* ── Change passcode ────────────────────────────────────────────────────── */
void carer_ui_begin_change_code(void);
void carer_ui_draw_change_code(void);
carer_action_t carer_ui_handle_change_code_touch(uint32_t tx, uint32_t ty);

/* ── Patient list ───────────────────────────────────────────────────────── */
void carer_ui_begin_patients(void);
void carer_ui_draw_patients(void);
carer_action_t carer_ui_handle_patients_touch(uint32_t tx, uint32_t ty);
/** Gallery slot chosen on the patient list, or -1. */
int  carer_ui_selected_slot(void);

/* ── One patient: schedule / dose / delete ──────────────────────────────── */
void carer_ui_draw_patient_menu(void);
carer_action_t carer_ui_handle_patient_menu_touch(uint32_t tx, uint32_t ty);

void carer_ui_begin_schedule(void);
void carer_ui_draw_schedule(void);
carer_action_t carer_ui_handle_schedule_touch(uint32_t tx, uint32_t ty);

void carer_ui_begin_dose(void);
void carer_ui_draw_dose(void);
carer_action_t carer_ui_handle_dose_touch(uint32_t tx, uint32_t ty);

/** Name of the currently selected patient, for the delete-confirm screen's
 *  message. Returns "" if nothing is selected. */
const char *carer_ui_selected_name(void);

/* ── Log review ─────────────────────────────────────────────────────────── */
void carer_ui_begin_log(void);
void carer_ui_draw_log(void);
carer_action_t carer_ui_handle_log_touch(uint32_t tx, uint32_t ty);

#ifdef __cplusplus
}
#endif

#endif /* CARER_UI_H */
