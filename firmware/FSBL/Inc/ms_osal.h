/* ms_osal.h - MedSight OS Abstraction Layer
 *
 * PUBLIC API - defined against a FreeRTOS backend, unchanged since.
 * ms_osal.c's internals were swapped to µT-Kernel 3.0 calls without
 * changing a single signature here - that was the entire point of staging
 * the project behind this header from the beginning.
 *
 * One primitive was added later, the only widening this API has had:
 * event flags (osal_flag_*). The original four - task / queue / mutex /
 * delay - were chosen as the lowest common denominator
 * between FreeRTOS and µT-Kernel, precisely so the earlier backend swap
 * would be mechanical. That worked, but it also left the finished firmware
 * talking to µT-Kernel only through primitives every RTOS has. FreeRTOS was
 * removed, so there is no longer a second backend to keep the
 * surface narrow for, and the AI request/response handshake introduced here
 *  is a genuine multi-condition wait - exactly what µT-Kernel's
 * event flags exist for.
 * The idioms that were evaluated and
 * deliberately NOT adopted.
 *
 * CONTRACT:
 *   No application module (.c file) outside ms_osal.c may include RTOS
 *   headers (FreeRTOS or µT-Kernel's <tk/tkernel.h>) or call any RTOS API
 *   directly. Everything goes through the functions declared here.
 *
 * API surface matches the technical reference exactly.
 */

#ifndef MS_OSAL_H
#define MS_OSAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t */

/* ── Opaque handle types ─────────────────────────────────────────────────── */
/* Declared as void* so the backend (FreeRTOS or µT-Kernel) can store
 * whatever pointer type it needs without leaking RTOS headers upward.       */

typedef void *osal_task_handle_t;
typedef void *osal_queue_handle_t;
typedef void *osal_mutex_handle_t;
typedef void *osal_flag_handle_t;   /* Event flags */

/* Max length (incl. NUL) of the name passed to osal_task_create(), truncated
 * beyond this - sized for this backend's internal task-name storage. */
#define OSAL_TASK_NAME_MAX  16u

/* Sentinel: wait forever */
#define OSAL_WAIT_FOREVER   ( (uint32_t)0xFFFFFFFFu )
/* Sentinel: do not wait (non-blocking) */
#define OSAL_NO_WAIT        ( (uint32_t)0u )

/* ── Task entry function type ────────────────────────────────────────────── */
typedef void (*osal_task_fn_t)(void *arg);

/* ═══════════════════════════════════════════════════════════════════════════
 * Task Management
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Create an RTOS task and immediately make it ready to run.
 *
 * @param  fn           Task entry function - must never return.
 * @param  name         Human-readable task name (truncated to 15 chars).
 * @param  stack_words  Stack depth in 32-bit WORDS (not bytes).
 * @param  arg          Opaque argument passed to fn(arg).
 * @param  priority     Task priority, 1 = lowest, higher numbers = higher priority
 *                      (this backend maps the ordering onto µT-Kernel's own
 *                      priority scale internally - see ms_osal.c).
 *                      Suggested values:
 *                        1 = heartbeat / background
 *                        2 = logger
 *                        4 = UI / touch
 *                        5 = camera / ISP
 * @return Handle to the created task, or NULL on failure.
 */
osal_task_handle_t osal_task_create(osal_task_fn_t fn,
                                    const char    *name,
                                    uint32_t       stack_words,
                                    void          *arg,
                                    uint32_t       priority);

/**
 * @brief  Start the RTOS scheduler. Does NOT return on success.
 *         Call this at the end of main() after all tasks are created.
 */
void osal_scheduler_start(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Queue (message-passing FIFO)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Create a fixed-depth, fixed-item-size queue.
 *
 * @param  item_count  Maximum number of items the queue can hold.
 * @param  item_size   Size of each item in bytes.
 * @return Handle, or NULL on failure.
 */
osal_queue_handle_t osal_queue_create(uint32_t item_count,
                                      uint32_t item_size);

/**
 * @brief  Send one item to the back of a queue.
 *
 * @param  q           Queue handle returned by osal_queue_create().
 * @param  item        Pointer to the item to copy into the queue.
 * @param  timeout_ms  Max wait in ms. OSAL_NO_WAIT = non-blocking.
 *                     OSAL_WAIT_FOREVER = block until space available.
 * @return true if item was posted, false if timeout expired.
 */
bool osal_queue_send(osal_queue_handle_t q,
                     const void         *item,
                     uint32_t            timeout_ms);

/**
 * @brief  Receive one item from the front of a queue.
 *
 * @param  q           Queue handle.
 * @param  item_out    Buffer to copy the item into. Must be at least
 *                     item_size bytes (as given to osal_queue_create).
 * @param  timeout_ms  Max wait in ms. OSAL_WAIT_FOREVER = block indefinitely.
 * @return true if an item was received, false on timeout.
 */
bool osal_queue_receive(osal_queue_handle_t q,
                        void               *item_out,
                        uint32_t            timeout_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * Mutex
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Create a mutual exclusion mutex (non-recursive).
 * @return Handle, or NULL on failure.
 */
osal_mutex_handle_t osal_mutex_create(void);

/**
 * @brief  Acquire (lock) the mutex.
 *
 * @param  m           Mutex handle.
 * @param  timeout_ms  Max wait in ms. OSAL_WAIT_FOREVER = block until locked.
 * @return true if mutex was acquired, false on timeout.
 */
bool osal_mutex_lock(osal_mutex_handle_t m, uint32_t timeout_ms);

/**
 * @brief  Release (unlock) the mutex.
 *         Must be called by the same task that locked it.
 * @param  m  Mutex handle.
 */
void osal_mutex_unlock(osal_mutex_handle_t m);

/* ═══════════════════════════════════════════════════════════════════════════
 * Event flags
 *
 * A single 32-bit bit-pattern object that any task can set bits in, and that
 * one or more tasks can wait on for a *combination* of bits - which is the
 * thing a queue, a semaphore and a mutex all cannot express. Backed directly
 * by µT-Kernel's tk_cre_flg / tk_set_flg / tk_clr_flg / tk_wai_flg.
 *
 * Used in this project for the AI request/response handshake between the UI
 * task and the NPU task (see ai_vision.c): the UI waits on
 * (AI_DONE | AI_FAIL) with a timeout - one wait, three outcomes, no polling.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Wait modes for osal_flag_wait(). Values are this OSAL's own, mapped onto
 * µT-Kernel's TWF_* inside ms_osal.c - callers must not assume they are the
 * same numbers. */
#define OSAL_FLAG_WAIT_AND   ( (uint32_t)0u )   /* all bits in pattern must be set */
#define OSAL_FLAG_WAIT_OR    ( (uint32_t)1u )   /* any bit in pattern is enough    */
/* OR-able into the wait mode: atomically clear the bits that satisfied the
 * wait, so the next wait starts clean without a separate osal_flag_clear(). */
#define OSAL_FLAG_WAIT_CLEAR ( (uint32_t)2u )

/**
 * @brief  Create an event-flag object with all bits initially clear.
 *         Multiple tasks may wait on the same object.
 * @return Handle, or NULL on failure.
 */
osal_flag_handle_t osal_flag_create(void);

/**
 * @brief  Set (OR in) one or more bits. Wakes every task whose wait
 *         condition the new pattern satisfies.
 * @param  f        Flag handle.
 * @param  pattern  Bits to set. Setting 0 bits is a no-op.
 */
void osal_flag_set(osal_flag_handle_t f, uint32_t pattern);

/**
 * @brief  Clear (AND out) one or more bits.
 * @param  f        Flag handle.
 * @param  pattern  Bits to clear.
 */
void osal_flag_clear(osal_flag_handle_t f, uint32_t pattern);

/**
 * @brief  Block until the flag's bit pattern satisfies the wait condition.
 *
 * @param  f            Flag handle.
 * @param  pattern      Bits of interest (must be non-zero).
 * @param  wait_mode    OSAL_FLAG_WAIT_AND or OSAL_FLAG_WAIT_OR, optionally
 *                      OR-ed with OSAL_FLAG_WAIT_CLEAR.
 * @param  out_pattern  If non-NULL, receives the flag pattern that satisfied
 *                      the wait (before any CLEAR is applied) - this is how
 *                      an OR-wait tells you *which* condition fired.
 * @param  timeout_ms   Max wait in ms. OSAL_NO_WAIT = poll (return
 *                      immediately), OSAL_WAIT_FOREVER = block indefinitely.
 * @return true if the condition was met, false on timeout/error.
 */
bool osal_flag_wait(osal_flag_handle_t f,
                    uint32_t            pattern,
                    uint32_t            wait_mode,
                    uint32_t           *out_pattern,
                    uint32_t            timeout_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * Alarm handlers
 *
 * The SECOND widening of this API, and it earns its place the same way event
 * flags did in There is a genuine consumer that no primitive
 * already here expresses.
 *
 * The device has a schedule. A dose window is a one-shot
 * deadline at an absolute time of day - "wake me at 08:00" - and the
 * alternatives are all worse:
 *   - a task polling the clock every N ms burns wake-ups forever to catch an
 *     event that happens three times a day, and directly damages the one
 *     power number this project has been measuring throughout;
 *   - osal_delay_ms() until the next dose blocks a whole task on nothing;
 *   - an event flag has no notion of time at all.
 * µT-Kernel's tk_cre_alm / tk_sta_alm is exactly this object: a one-shot
 * handler armed for a relative time, costing nothing until it fires. It is
 * also, not incidentally, the mechanism the original Program Plan named -
 * "The Camera Task wakes either on a scheduled µT-Kernel alarm (aligned to
 * dose times)". That sentence has been unimplemented since March.
 *
 * HANDLER CONTEXT - this is the constraint that shapes every use.
 * The handler runs in µT-Kernel's timer/interrupt context, not in a task. It
 * may not block, may not printf (there is a measured
 * record of what a slow handler path costs on this hardware), and may not
 * make a call that would wait. The only correct shape is: set an event flag
 * and return. A task does the work. state_machine.c/schedule handling
 * follows exactly that pattern - the handler is three lines.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void *osal_alarm_handle_t;

/** Alarm callback. Runs in HANDLER context - see the warning above. Set a
 *  flag and return; do nothing else. */
typedef void (*osal_alarm_fn_t)(void *arg);

/**
 * @brief  Create a one-shot alarm handler. Not armed until osal_alarm_start().
 *
 *         Like every other osal_*_create() in this API, this may be called
 *         from main() before the scheduler starts - the µT-Kernel object is
 *         created for real inside ms_osal.c's usermain() in that case.
 *
 * @param  fn   Handler, called in handler context when the alarm expires.
 * @param  arg  Opaque value passed to fn().
 * @return Handle, or NULL on failure.
 */
osal_alarm_handle_t osal_alarm_create(osal_alarm_fn_t fn, void *arg);

/**
 * @brief  Arm (or re-arm) the alarm to fire once, delay_ms from now.
 *         Re-arming an already-armed alarm replaces the pending expiry.
 * @return true if the alarm was armed.
 */
bool osal_alarm_start(osal_alarm_handle_t a, uint32_t delay_ms);

/** @brief  Cancel a pending alarm. Harmless if it was not armed. */
void osal_alarm_stop(osal_alarm_handle_t a);

/* ═══════════════════════════════════════════════════════════════════════════
 * Time
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Block the calling task for at least ms milliseconds.
 *         Other tasks run during the delay (cooperative/preemptive).
 *         Do NOT use HAL_Delay() from task context - use this instead.
 *
 * @param  ms  Delay duration. 0 yields the CPU for the remainder of
 *             the current tick (equivalent to taskYIELD).
 */
void osal_delay_ms(uint32_t ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * Power saving / idle accounting
 *
 * µT-Kernel's dispatcher calls low_pow() from its idle path whenever no task
 * is runnable (mtk3_bsp2.../armv8m/dispatch.S, label l_dispatch_110). The
 * vendored BSP ships low_pow() as an empty function, so before this project
 * the Cortex-M55 spun at full clock whenever the system had nothing to do.
 *
 * mtk3_bsp2/sysdepend/stm32_cube/power_save.c's low_pow() now forwards to
 * ms_osal_low_power_idle() below - the actual WFI plus its cycle accounting
 * live on THIS side of the vendored-code boundary, exactly like
 * ms_osal_clean_dcache(), so the CMSIS dependency stays in this project's
 * own code and the BSP diff stays one line.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Idle hook. Puts the core into WFI until the next interrupt and
 *         accumulates the cycles spent asleep.
 *
 *         Called ONLY from µT-Kernel's dispatcher idle path, in handler
 *         (PendSV) context with BASEPRI masked. Must not printf, must not
 *         call any tk_* API, and must return promptly once woken -
 *         there is a measured record of exactly what a slow
 *         dispatcher/ISR path costs on this hardware.
 */
void ms_osal_low_power_idle(void);

/**
 * @brief  Read back the idle accounting gathered by ms_osal_low_power_idle().
 *
 * @param  out_idle_cycles  If non-NULL, total CPU cycles spent inside WFI
 *                          since boot.
 * @param  out_entries      If non-NULL, number of times the idle hook ran.
 * @return false if the cycle counter could not be enabled on this silicon
 *         (in which case out_idle_cycles is meaningless and only the entry
 *         count is valid).
 */
bool ms_osal_idle_stats(uint64_t *out_idle_cycles, uint32_t *out_entries);

/**
 * @brief  Clean a region of the D-cache to the point of coherency.
 *         Required because µT-Kernel builds its
 *         exception vector table at runtime in write-back cacheable RAM and
 *         the vendored BSP does no cache maintenance of its own. Called from
 *         mtk3_bsp2's sys_start.c and interrupt.c. Do not remove.
 */
void ms_osal_clean_dcache(void *addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MS_OSAL_H */
