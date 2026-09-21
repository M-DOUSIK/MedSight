/* dispenser.h - MedSight physical dispenser
 *
 * One hopper: a 28BYJ-48 unipolar stepper turning a turntable through a
 * ULN2003 Darlington array, and a 3-pin IR break-beam module counting each
 * pill as it slides past.
 *
 * THE ONE IDEA THIS MODULE EXISTS FOR. The stepper is open-loop and stalls
 * silently, so steps commanded is not pills dispensed. The motor stops when
 * the IR beam has counted the requested number of pills, not when a timer
 * expires. If the step count and the beam count disagree, the beam is right.
 * the hardware specification rejection of a sliding gate rests on
 * exactly this: a gate cannot know how many pills went through it.
 *
 * THE PILLS SLIDE DOWN A RAMP, THEY DO NOT FALL. That inverts the usual
 * break-beam assumptions and every tuning constant in dispenser.c follows
 * from it - see the block comment above MS_IR_MIN_BREAK_MS there.
 *
 * BUILD SWITCH. With MEDSIGHT_PHYSICAL_DISPENSER at 0 this module still
 * compiles and links, dispenser_dispense() reports DISPENSE_NOT_READY, and
 * state_machine.c falls back to the earlier simulated animation. That is
 * deliberate, and it is not optional: the full flow has to remain
 * demonstrable on a board with no hardware attached.
 */

#ifndef DISPENSER_H
#define DISPENSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Set to 0 to build the earlier software simulation instead. */
#ifndef MEDSIGHT_PHYSICAL_DISPENSER
#define MEDSIGHT_PHYSICAL_DISPENSER 1
#endif

typedef enum {
    DISPENSE_OK = 0,        /* requested count counted out of the chute      */
    DISPENSE_SHORT,         /* fewer pills than requested before timeout     */
    DISPENSE_JAM,           /* actuator ran, no pill seen - or one stalled   */
    DISPENSE_NOT_READY      /* driver not initialised / not built in         */
} dispense_result_t;

/** Progress callback, invoked once per counted pill from the calling task's
 *  context (never from the ISR). NULL disables it. */
typedef void (*dispenser_progress_fn_t)(uint8_t counted, uint8_t requested);

/**
 * @brief  Configure the four coil GPIOs and the IR sensor's EXTI line.
 *         Call once from main(), before the scheduler starts.
 *
 *         Samples the IR module's idle level and derives the break polarity
 *         from it, so an active-LOW and an active-HIGH module both work
 *         without a rebuild. The detected polarity is printed at boot.
 */
void dispenser_init(void);

/**
 * @brief  Dispense `count` pills. BLOCKING, bounded by a timeout.
 *
 *         Turns the turntable and returns as soon as the IR beam has counted
 *         `count` pills. Leaves all four coils de-energised on every exit
 *         path, including the failures.
 *
 * @param  count          Pills requested. 0 is treated as 1.
 * @param  out_dispensed  If non-NULL, receives how many pills were actually
 *                        counted through the beam - meaningful for every
 *                        return value, not just DISPENSE_OK.
 * @return DISPENSE_OK / DISPENSE_SHORT / DISPENSE_JAM / DISPENSE_NOT_READY.
 *         A jam or a short count is a NORMAL outcome reported to the caller,
 *         never an Error_Handler() - the same rule applied to SD
 *         failures.
 */
dispense_result_t dispenser_dispense(uint8_t count, uint8_t *out_dispensed);

/** @brief  Register the per-pill progress callback used to drive the
 *          dispensing screen's bar. Pass NULL to clear it. */
void dispenser_set_progress_cb(dispenser_progress_fn_t cb);

/** @brief  Total pills counted through the beam since boot. */
uint32_t dispenser_get_total_dispensed(void);

/** @brief  True once dispenser_init() has configured the hardware. */
bool dispenser_is_ready(void);

/** @brief  Short, log-safe name for a result code ("OK"/"SHORT"/"JAM"/...). */
const char *dispenser_result_str(dispense_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* DISPENSER_H */
