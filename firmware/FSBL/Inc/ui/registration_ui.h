/* ui/registration_ui.h - Patient registration UI flow
 *
 * Owns the three registration screens (on-screen keyboard,
 * pill-count entry, confirm summary) between STATE_CAMERA_REGISTER's face
 * capture and gallery_add_patient()'s save. Per the technical reference
 * section 3's documented exception, this module is allowed to call ai_vision.h's
 * gallery_add_patient() directly (unlike other UI modules, which only ever
 * go through state_machine.c) because registration is its own
 * self-contained flow.
 *
 * Touch is still polled once per tick by state_machine.c (touch_driver.h's
 * API, per the module-boundary rule that only touch_driver.c touches I2C2
 * directly) - state_machine.c passes the resulting (x,y) coordinate into
 * this module's handlers only on a rising-edge touch, exactly like the
 * existing home-screen/ready-screen button handling. This module does not
 * poll touch_driver.h itself, to avoid two independent edge-detectors
 * racing on the same physical touch.
 */
#ifndef REGISTRATION_UI_H
#define REGISTRATION_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REG_CONFIRM_NONE,   /* tap didn't hit Confirm or Retry */
    REG_CONFIRM_SAVED,  /* saved to the gallery AND written to the SD card  */
    /* Added in RAM, but patients.dat could not be written - the
     * card is missing or unwritable. The enrolment works for this power
     * cycle and is gone after the next one, so the UI must say so instead of
     * reporting success. Found on hardware: the log said "save to SD failed"
     * and the screen said "Registered! Welcome aboard." */
    REG_CONFIRM_SAVED_NO_SD,
    REG_CONFIRM_FULL,   /* gallery_add_patient() returned -1 (gallery full) */
    REG_CONFIRM_RETRY   /* Retry tapped - caller should return to STATE_CAMERA_REGISTER */
} reg_confirm_result_t;

/* Call once on entering STATE_KEYBOARD_REGISTER (i.e. right after a
 * successful face capture) - clears the name buffer and resets pill count
 * to its default (1). Does NOT clear the embedding; call
 * registration_ui_set_embedding() separately right after this. */
void registration_ui_reset(void);

/* Store the embedding captured by STATE_CAMERA_REGISTER for later use by
 * registration_ui_handle_confirm_touch()'s gallery_add_patient() call. */
void registration_ui_set_embedding(const int8_t *embedding);

/* ── Keyboard screen (STATE_KEYBOARD_REGISTER) ──────────────────────────── */
void registration_ui_draw_keyboard(void);
/* Call only on a rising-edge touch. Returns true when "DONE" is tapped with
 * a non-empty name - caller should advance to STATE_PILLCOUNT_REGISTER. */
bool registration_ui_handle_keyboard_touch(uint32_t tx, uint32_t ty);

/* ── Pill-count screen (STATE_PILLCOUNT_REGISTER) ───────────────────────── */
/** The name typed so far, for screens that want to address the patient by it.
 *  Never NULL; empty until the keyboard step has run. Name
 *  entry comes BEFORE the face scan so the camera screen can use it. */
const char *registration_ui_name(void);

void registration_ui_draw_pillcount(void);
/* Call only on a rising-edge touch. Returns true when "NEXT" is tapped -
 * caller should advance to STATE_CONFIRM_REGISTER. */
bool registration_ui_handle_pillcount_touch(uint32_t tx, uint32_t ty);

/* ── Confirm screen (STATE_CONFIRM_REGISTER) ────────────────────────────── */
void registration_ui_draw_confirm(void);
/* Call only on a rising-edge touch. See reg_confirm_result_t above. */
reg_confirm_result_t registration_ui_handle_confirm_touch(uint32_t tx, uint32_t ty);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRATION_UI_H */
