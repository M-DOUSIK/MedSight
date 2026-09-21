/* ms_osal.c - MedSight OS Abstraction Layer (µT-Kernel 3.0 backend)
 *
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║  THIS IS THE ONLY FILE IN THE PROJECT THAT MAY CALL µT-KERNEL API.      ║
 * ║  FreeRTOS backend, swapped to µT-Kernel 3.0.                            ║
 * ║  The public API in ms_osal.h has NOT changed at all.                   ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 *
 * BSP: mtk3_bsp2 (TRON Forum reference BSP2 for STM32N6570-DK), vendored
 *      unmodified under FSBL/mtk3_bsp2/ - see THIRD_PARTY_SOFTWARE.md for
 *      where it came from and how it was integrated.
 *
 * ── Why this file is not a 1:1 API transliteration ─────────────────────────
 *
 * µT-Kernel object-creation syscalls (tk_cre_tsk/tk_cre_mbf/tk_cre_mtx) can
 * only be called once the kernel itself is initialised - which happens
 * inside knl_start_mtkernel() (called from osal_scheduler_start(), below),
 * which never returns to its caller. The kernel then calls this file's
 * usermain() (via mtk3_bsp2's inittask) once it is ready.
 *
 * But main.c - unchanged, by design - creates every
 * task/queue and THEN calls osal_scheduler_start() as the last thing it
 * does. So osal_task_create()/osal_queue_create()/osal_mutex_create() are
 * always called *before* the kernel exists to create anything.
 *
 * This file bridges that mismatch: creation calls made before the kernel is
 * running just record their parameters into a small static pool and hand
 * back a stable pointer into that pool as the "handle" - exactly what
 * main.c/sd_logger.c already expect to receive and hold onto. usermain()
 * (called by the kernel once it's alive, before any of our tasks run) then
 * walks that pool and performs the real tk_cre_mbf/tk_cre_mtx/tk_cre_tsk
 * calls, patching each slot's real object ID in place. Any application code
 * that runs after that point (i.e. every task body) only ever sees a fully
 * "real" object - the ordering in usermain() (queues, then mutexes, then
 * tasks-started-last) guarantees no task can run before the objects it
 * depends on are real.
 */

#include "ms_osal.h"

/* ── µT-Kernel includes - only allowed in this file ──────────────────────── */
#include <tk/tkernel.h>

/* ── HAL for HAL_IncTick, and main.h for Error_Handler() ─────────────────── */
#include "stm32n6xx_hal.h"
#include "main.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════════
 * Deferred-creation pools
 * ════════════════════════════════════════════════════════════════════════════ */

#define OSAL_MAX_TASKS      8u
#define OSAL_MAX_QUEUES     4u
#define OSAL_MAX_MUTEXES    4u
#define OSAL_MAX_FLAGS      4u   /* mtk3 config.h CNF_MAX_FLGID = 16          */
#define OSAL_MAX_ALARMS     2u   /* mtk3 config.h CNF_MAX_ALMID = 8           */

/* Priority mapping: ms_osal.h documents "1 = lowest... N = highest" (the
 * FreeRTOS convention this project's callers were written against - see
 * main.c's task table: 1=heartbeat, 2=logger, 4=ui, 5=cam_isp). µT-Kernel is
 * the opposite: itskpri 1 is the HIGHEST priority. OSAL_PRI_CEILING is an
 * arbitrary internal ceiling (well inside CNF_MAX_TSKPRI=32) that inverts
 * the ordering while leaving headroom above the values this project actually
 * uses (1..5) - only relative order matters, not the absolute numbers. */
#define OSAL_PRI_CEILING    16u

typedef struct {
    osal_task_fn_t  fn;
    void           *arg;
    char            name[OSAL_TASK_NAME_MAX];
    uint32_t        stack_words;
    uint32_t        priority;
    ID              tskid;     /* filled in by usermain() once real */
    bool            in_use;
} osal_task_slot_t;

typedef struct {
    uint32_t item_count;
    uint32_t item_size;
    ID       mbfid;            /* filled in by usermain() once real */
    bool     in_use;
} osal_queue_slot_t;

typedef struct {
    ID   mtxid;                /* filled in by usermain() once real */
    bool in_use;
} osal_mutex_slot_t;

typedef struct {
    ID   flgid;                /* filled in by usermain() once real */
    bool in_use;
} osal_flag_slot_t;

typedef struct {
    osal_alarm_fn_t fn;
    void           *arg;
    ID              almid;     /* filled in by usermain() once real */
    bool            in_use;
} osal_alarm_slot_t;

static osal_task_slot_t   s_tasks[OSAL_MAX_TASKS];
static uint32_t           s_task_count   = 0u;

static osal_queue_slot_t  s_queues[OSAL_MAX_QUEUES];
static uint32_t           s_queue_count  = 0u;

static osal_mutex_slot_t  s_mutexes[OSAL_MAX_MUTEXES];
static uint32_t           s_mutex_count  = 0u;

static osal_flag_slot_t   s_flags[OSAL_MAX_FLAGS];
static uint32_t           s_flag_count   = 0u;

static osal_alarm_slot_t  s_alarms[OSAL_MAX_ALARMS];
static uint32_t           s_alarm_count  = 0u;

/* Set true by usermain() right after queues/mutexes are made real, before
 * any task is started - see the file header comment above. */
static volatile bool s_kernel_running = false;

/* ════════════════════════════════════════════════════════════════════════════
 * ms_to_tmo - helper: OSAL timeout (ms) to µT-Kernel TMO
 *
 * T-Kernel TMO parameters are specified directly in milliseconds by spec
 * (unlike a raw tick count) - no scaling needed, only the two sentinel
 * values differ from this project's OSAL sentinels.
 * ════════════════════════════════════════════════════════════════════════════ */
static inline TMO ms_to_tmo(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER) {
        return TMO_FEVR;
    }
    return (TMO)ms;   /* OSAL_NO_WAIT (0) already equals TMO_POL (0) */
}

/* ════════════════════════════════════════════════════════════════════════════
 * Task Management
 * ════════════════════════════════════════════════════════════════════════════ */

/* Every µT-Kernel task entry has the signature void(INT stacd, void *exinf).
 * This trampoline adapts that to this project's void(void *arg) contract,
 * using the task's own slot (passed as exinf) to recover fn/arg. */
static void osal_task_trampoline(INT stacd, void *exinf)
{
    (void)stacd;
    osal_task_slot_t *slot = (osal_task_slot_t *)exinf;
    slot->fn(slot->arg);
    /* Contract says task functions never return; this is a defensive
     * net only - falling off the end would otherwise corrupt the stack. */
    tk_ext_tsk();
}

static ID osal_start_task_slot(osal_task_slot_t *slot)
{
    T_CTSK ctsk = {
        .exinf   = slot,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)osal_task_trampoline,
        .itskpri = (PRI)(OSAL_PRI_CEILING - slot->priority),
        .stksz   = (SZ)(slot->stack_words * sizeof(uint32_t)),
    };
    ID id = tk_cre_tsk(&ctsk);
    if (id > 0) {
        (void)tk_sta_tsk(id, 0);
    }
    return id;
}

osal_task_handle_t osal_task_create(osal_task_fn_t fn,
                                    const char    *name,
                                    uint32_t       stack_words,
                                    void          *arg,
                                    uint32_t       priority)
{
    if (s_task_count >= OSAL_MAX_TASKS) {
        printf("FATAL: osal_task_create - task pool exhausted (max %u)\r\n",
               OSAL_MAX_TASKS);
        Error_Handler();
        return NULL;
    }

    osal_task_slot_t *slot = &s_tasks[s_task_count++];
    slot->fn          = fn;
    slot->arg         = arg;
    slot->stack_words = stack_words;
    slot->priority    = priority;
    slot->in_use      = true;
    slot->tskid       = 0;
    (void)strncpy(slot->name, name, OSAL_TASK_NAME_MAX - 1u);
    slot->name[OSAL_TASK_NAME_MAX - 1u] = '\0';

    if (s_kernel_running) {
        /* Not exercised by this codebase today (every task is created from
         * main() before osal_scheduler_start()) but implemented for
         * correctness: the kernel is already up, so create for real now
         * instead of waiting for a usermain() pass that has already run. */
        slot->tskid = osal_start_task_slot(slot);
        if (slot->tskid <= 0) {
            printf("FATAL: tk_cre_tsk failed for task '%s': %d\r\n", name, (int)slot->tskid);
            Error_Handler();
            return NULL;
        }
    }
    /* else: left pending - usermain() creates and starts it. */

    return (osal_task_handle_t)slot;
}

void osal_scheduler_start(void)
{
    /* Never returns: starts the µT-Kernel, which brings up inittask, which
     * calls usermain() (below) once the kernel is ready to create objects. */
    extern void knl_start_mtkernel(void);
    knl_start_mtkernel();

    /* Should never reach here. */
    printf("FATAL: knl_start_mtkernel() returned\r\n");
    Error_Handler();
}

/* ════════════════════════════════════════════════════════════════════════════
 * Queue (message-passing FIFO) - backed by a µT-Kernel message buffer
 * ════════════════════════════════════════════════════════════════════════════ */

/* Per-message overhead the kernel's message buffer reserves internally
 * (messagebuf.h: HEADERSZ = sizeof(INT), each stored message is rounded up
 * to a HEADER-sized boundary before the header is added) - sizing bufsz any
 * smaller silently makes the buffer hold fewer items than item_count. */
#define OSAL_MBF_HEADER_SZ   ((uint32_t)sizeof(INT))
#define OSAL_MBF_ROUND(sz)   (((uint32_t)(sz) + (OSAL_MBF_HEADER_SZ - 1u)) & ~(OSAL_MBF_HEADER_SZ - 1u))

static ID osal_start_queue_slot(osal_queue_slot_t *slot)
{
    uint32_t per_msg = OSAL_MBF_ROUND(slot->item_size) + OSAL_MBF_HEADER_SZ;
    T_CMBF cmbf = {
        .exinf  = NULL,
        .mbfatr = TA_TFIFO,
        .bufsz  = (SZ)(per_msg * slot->item_count),
        .maxmsz = (INT)slot->item_size,
        .bufptr = NULL,   /* no TA_USERBUF: kernel allocates via imalloc */
    };
    return tk_cre_mbf(&cmbf);
}

osal_queue_handle_t osal_queue_create(uint32_t item_count,
                                      uint32_t item_size)
{
    if (s_queue_count >= OSAL_MAX_QUEUES) {
        printf("FATAL: osal_queue_create - queue pool exhausted (max %u)\r\n",
               OSAL_MAX_QUEUES);
        Error_Handler();
        return NULL;
    }

    osal_queue_slot_t *slot = &s_queues[s_queue_count++];
    slot->item_count = item_count;
    slot->item_size  = item_size;
    slot->in_use     = true;
    slot->mbfid      = 0;

    if (s_kernel_running) {
        slot->mbfid = osal_start_queue_slot(slot);
        if (slot->mbfid <= 0) {
            printf("FATAL: tk_cre_mbf failed: %d\r\n", (int)slot->mbfid);
            Error_Handler();
            return NULL;
        }
    }

    return (osal_queue_handle_t)slot;
}

bool osal_queue_send(osal_queue_handle_t q,
                     const void         *item,
                     uint32_t            timeout_ms)
{
    osal_queue_slot_t *slot = (osal_queue_slot_t *)q;
    return (tk_snd_mbf(slot->mbfid, item, (INT)slot->item_size, ms_to_tmo(timeout_ms)) == E_OK);
}

bool osal_queue_receive(osal_queue_handle_t q,
                        void               *item_out,
                        uint32_t            timeout_ms)
{
    osal_queue_slot_t *slot = (osal_queue_slot_t *)q;
    return (tk_rcv_mbf(slot->mbfid, item_out, ms_to_tmo(timeout_ms)) >= E_OK);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Mutex - backed by a µT-Kernel mutex (priority-inheritance protocol)
 * ════════════════════════════════════════════════════════════════════════════ */

static ID osal_start_mutex_slot(void)
{
    T_CMTX cmtx = {
        .exinf   = NULL,
        .mtxatr  = TA_INHERIT,
        .ceilpri = 0,   /* unused under TA_INHERIT */
    };
    return tk_cre_mtx(&cmtx);
}

osal_mutex_handle_t osal_mutex_create(void)
{
    if (s_mutex_count >= OSAL_MAX_MUTEXES) {
        printf("FATAL: osal_mutex_create - mutex pool exhausted (max %u)\r\n",
               OSAL_MAX_MUTEXES);
        Error_Handler();
        return NULL;
    }

    osal_mutex_slot_t *slot = &s_mutexes[s_mutex_count++];
    slot->in_use = true;
    slot->mtxid  = 0;

    if (s_kernel_running) {
        slot->mtxid = osal_start_mutex_slot();
        if (slot->mtxid <= 0) {
            printf("FATAL: tk_cre_mtx failed: %d\r\n", (int)slot->mtxid);
            Error_Handler();
            return NULL;
        }
    }

    return (osal_mutex_handle_t)slot;
}

bool osal_mutex_lock(osal_mutex_handle_t m, uint32_t timeout_ms)
{
    osal_mutex_slot_t *slot = (osal_mutex_slot_t *)m;
    return (tk_loc_mtx(slot->mtxid, ms_to_tmo(timeout_ms)) == E_OK);
}

void osal_mutex_unlock(osal_mutex_handle_t m)
{
    osal_mutex_slot_t *slot = (osal_mutex_slot_t *)m;
    (void)tk_unl_mtx(slot->mtxid);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Event flags - backed by a µT-Kernel event flag
 *
 * TA_WMUL so more than one task may wait on the same object at once. This
 * project only ever has one waiter per flag today, but TA_WSGL would turn a
 * future second waiter into a silent E_OBJ at runtime rather than something
 * that just works, and the multi-wait bookkeeping costs nothing while unused.
 * ════════════════════════════════════════════════════════════════════════════ */

static ID osal_start_flag_slot(void)
{
    T_CFLG cflg = {
        .exinf   = NULL,
        .flgatr  = TA_TFIFO | TA_WMUL,
        .iflgptn = 0u,          /* all bits clear at creation */
    };
    return tk_cre_flg(&cflg);
}

osal_flag_handle_t osal_flag_create(void)
{
    if (s_flag_count >= OSAL_MAX_FLAGS) {
        printf("FATAL: osal_flag_create - flag pool exhausted (max %u)\r\n",
               OSAL_MAX_FLAGS);
        Error_Handler();
        return NULL;
    }

    osal_flag_slot_t *slot = &s_flags[s_flag_count++];
    slot->in_use = true;
    slot->flgid  = 0;

    if (s_kernel_running) {
        slot->flgid = osal_start_flag_slot();
        if (slot->flgid <= 0) {
            printf("FATAL: tk_cre_flg failed: %d\r\n", (int)slot->flgid);
            Error_Handler();
            return NULL;
        }
    }
    /* else: left pending - usermain() creates it. */

    return (osal_flag_handle_t)slot;
}

void osal_flag_set(osal_flag_handle_t f, uint32_t pattern)
{
    osal_flag_slot_t *slot = (osal_flag_slot_t *)f;
    if ((slot == NULL) || (slot->flgid <= 0)) {
        return;
    }
    (void)tk_set_flg(slot->flgid, (UINT)pattern);
}

void osal_flag_clear(osal_flag_handle_t f, uint32_t pattern)
{
    osal_flag_slot_t *slot = (osal_flag_slot_t *)f;
    if ((slot == NULL) || (slot->flgid <= 0)) {
        return;
    }
    /* tk_clr_flg's argument is the pattern to KEEP (the flag is AND-ed with
     * it), not the pattern to clear - invert here so this OSAL's callers get
     * the "clear these bits" semantics ms_osal.h documents. */
    (void)tk_clr_flg(slot->flgid, (UINT)(~pattern));
}

bool osal_flag_wait(osal_flag_handle_t f,
                    uint32_t            pattern,
                    uint32_t            wait_mode,
                    uint32_t           *out_pattern,
                    uint32_t            timeout_ms)
{
    osal_flag_slot_t *slot = (osal_flag_slot_t *)f;
    if ((slot == NULL) || (slot->flgid <= 0) || (pattern == 0u)) {
        return false;
    }

    UINT wfmode = ((wait_mode & OSAL_FLAG_WAIT_OR) != 0u) ? TWF_ORW : TWF_ANDW;
    if ((wait_mode & OSAL_FLAG_WAIT_CLEAR) != 0u) {
        /* TWF_BITCLR clears only the bits that satisfied this wait, leaving
         * any other bits set in the meantime intact - TWF_CLR would wipe the
         * whole pattern and lose them. */
        wfmode |= TWF_BITCLR;
    }

    UINT got = 0u;
    ER   err = tk_wai_flg(slot->flgid, (UINT)pattern, wfmode, &got,
                          ms_to_tmo(timeout_ms));
    if (out_pattern != NULL) {
        *out_pattern = (uint32_t)got;
    }
    return (err == E_OK);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Alarm handlers - backed by a µT-Kernel one-shot alarm
 *
 * Same deferred-creation shape as every other object in this file: the
 * application calls osal_alarm_create() from main(), the real tk_cre_alm()
 * happens in usermain() once the kernel can accept it.
 *
 * The trampoline below is the whole reason this indirection exists. µT-Kernel
 * hands the alarm handler the T_CALM's `exinf`, and the natural thing would
 * be to pass the caller's arg there directly - but then there is nowhere to
 * put the caller's function pointer. Passing the SLOT as exinf gives the
 * trampoline both, and keeps osal_alarm_fn_t free of any µT-Kernel type, so
 * the OSAL contract at the top of ms_osal.h still holds: no caller sees a
 * tk_* type or a µT-Kernel calling convention.
 * ════════════════════════════════════════════════════════════════════════════ */

/* Runs in HANDLER context. Everything the ms_osal.h warning says about the
 * application's handler applies here first: no blocking, no printf, nothing
 * unbounded. This function is deliberately three lines long. */
static void osal_alarm_trampoline(void *exinf)
{
    osal_alarm_slot_t *slot = (osal_alarm_slot_t *)exinf;
    if ((slot != NULL) && (slot->fn != NULL)) {
        slot->fn(slot->arg);
    }
}

static ID osal_start_alarm_slot(osal_alarm_slot_t *slot)
{
    T_CALM calm = {
        .exinf  = (void *)slot,
        .almatr = TA_HLNG,          /* handler written in C */
        .almhdr = (FP)osal_alarm_trampoline,
    };
    return tk_cre_alm(&calm);
}

osal_alarm_handle_t osal_alarm_create(osal_alarm_fn_t fn, void *arg)
{
    if (fn == NULL) {
        return NULL;
    }
    if (s_alarm_count >= OSAL_MAX_ALARMS) {
        printf("FATAL: osal_alarm_create - alarm pool exhausted (max %u)\r\n",
               OSAL_MAX_ALARMS);
        Error_Handler();
        return NULL;
    }

    osal_alarm_slot_t *slot = &s_alarms[s_alarm_count++];
    slot->in_use = true;
    slot->fn     = fn;
    slot->arg    = arg;
    slot->almid  = 0;

    if (s_kernel_running) {
        slot->almid = osal_start_alarm_slot(slot);
        if (slot->almid <= 0) {
            printf("FATAL: tk_cre_alm failed: %d\r\n", (int)slot->almid);
            Error_Handler();
            return NULL;
        }
    }
    /* else: left pending - usermain() creates it. */

    return (osal_alarm_handle_t)slot;
}

bool osal_alarm_start(osal_alarm_handle_t a, uint32_t delay_ms)
{
    osal_alarm_slot_t *slot = (osal_alarm_slot_t *)a;
    if ((slot == NULL) || (slot->almid <= 0)) {
        return false;
    }
    /* RELTIM is milliseconds by spec, same as the TMO values elsewhere in
     * this file, so no scaling. tk_sta_alm() on an already-armed alarm
     * replaces the pending expiry rather than stacking a second one, which
     * is exactly the re-arm semantics the dose schedule wants. */
    return (tk_sta_alm(slot->almid, (RELTIM)delay_ms) == E_OK);
}

void osal_alarm_stop(osal_alarm_handle_t a)
{
    osal_alarm_slot_t *slot = (osal_alarm_slot_t *)a;
    if ((slot == NULL) || (slot->almid <= 0)) {
        return;
    }
    (void)tk_stp_alm(slot->almid);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Time
 * ════════════════════════════════════════════════════════════════════════════ */
void osal_delay_ms(uint32_t ms)
{
    (void)tk_dly_tsk((RELTIM)ms);
}

/* ════════════════════════════════════════════════════════════════════════════
 * SysTick Handler - PRE-KERNEL phase only
 *
 * stm32n6xx_it.c deliberately does NOT define SysTick_Handler (see its own
 * comment at that call site) - this project's convention throughout
 * is that whichever OSAL backend is active owns it, because the backend is
 * what decides how (or whether) a tick needs bridging to HAL_IncTick().
 *
 * This was missed when ms_osal.c was rewritten for µT-Kernel: with no
 * SysTick_Handler anywhere in the link, that vector fell back to the
 * startup file's weak Default_Handler alias - a bare `b Infinite_Loop`,
 * never returning from the exception. The very first SysTick tick (fired
 * from inside HAL_Init()'s HAL_InitTick(), before main() has done anything
 * else) would jump straight into that infinite loop and never come back,
 * which is exactly the "stuck in Default_Handler, zero UART output" boot
 * hang that took most of the debugging time, chased everywhere except here.
 *
 * This handler only matters before osal_scheduler_start() - the instant
 * knl_start_mtkernel() relocates the vector table and calls
 * knl_init_interrupt(), µT-Kernel overwrites the SysTick vector with its
 * own knl_systim_inthdr(), and this function is never called again (the
 * post-kernel HAL_IncTick() bridge is the tk_cre_cyc cyclic handler further
 * down this file, which is a completely separate mechanism for a completely
 * separate phase of execution - both are needed, neither substitutes for
 * the other). */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ════════════════════════════════════════════════════════════════════════════
 * D-cache maintenance for µT-Kernel's runtime-built exception vector table
 *
 * knl_start_mtkernel() copies the ROM vector table into knl_exctbl[] (plain
 * AXI SRAM, normal write-back cacheable memory), points VTOR at it, and then
 * knl_init_interrupt() overwrites entries 2..15 with the kernel's own
 * handlers - all with ordinary stores and no cache maintenance anywhere. The
 * reference project this BSP was vendored from
 * (mtk3bsp2_samples/Examples/prj_stm32n6_cam) gets away with that because it
 * runs with both CPU caches OFF: its main.c has SCB_EnableICache() and
 * SCB_EnableDCache() commented out. THIS project enables both (main.c), and
 * has always done - the camera/NPU pipeline needs them.
 *
 * That difference matters: with the D-cache on and write-back, freshly
 * written vector entries can still be sitting dirty in the cache when the
 * CPU's exception-entry logic fetches a vector, so the fetch can see stale
 * physical memory (whatever the previous image or reset state left at that
 * address) rather than the handler the kernel just installed.
 *
 * Exposed here rather than inside the vendored kernel so the CMSIS
 * cache intrinsic (which already no-ops when the D-cache is disabled) stays
 * in this project's own code, consistent with how every other
 * kernel-specific decision in this OSAL is handled.
 * Called from mtk3_bsp2 sys_start.c and interrupt.c. */
void ms_osal_clean_dcache(void *addr, uint32_t size)
{
    if ((addr == NULL) || (size == 0u)) {
        return;
    }
    SCB_CleanDCache_by_Addr((uint32_t *)addr, (int32_t)size);
    __DSB();
    __ISB();
}

/* ════════════════════════════════════════════════════════════════════════════
 * Power saving - the idle hook behind µT-Kernel's low_pow()  
 *
 * "Power saving" is one of the
 * RTOS features an entry is evaluated on. Before that the project had
 * none: mtk3_bsp2/sysdepend/stm32_cube/power_save.c's low_pow() is an empty
 * function, and the dispatcher's idle path (dispatch.S, l_dispatch_110) calls
 * it in a tight loop whenever no task is runnable - so the Cortex-M55 spun at
 * full clock doing nothing. In this application that is most of the time: all
 * four (now five) tasks are periodic sleepers, and the busiest of them polls
 * at 1 ms.
 *
 * power_save.c's low_pow() now calls straight into this function. Keeping the
 * body here rather than in the vendored tree follows the precedent
 * set by ms_osal_clean_dcache(): CMSIS intrinsics stay in this
 * project's own code, and the diff against upstream mtk3_bsp2 stays a single
 * forwarding line that is trivial to audit for the contest's rule-1.3
 * modification table.
 *
 * ── Execution context (this is the part that constrains everything) ────────
 * low_pow() runs in HANDLER mode, inside PendSV, with
 * BASEPRI = INTPRI_VAL(INTPRI_MAX_EXTINT_PRI) (0x10) - which masks SysTick
 * (0x10) and PendSV (0xF0). So:
 *   - No printf. Ever. There is a measured record of how
 *     a slow dispatcher/ISR path starved PendSV permanently on this board.
 *   - No tk_* call: this is not task context.
 *   - Nothing that can block or take unbounded time.
 * Two DWT register reads and a couple of adds are the entire budget, and that
 * is all this does.
 *
 * ── WFI vs BASEPRI - CORRECTED ON HARDWARE ────────────────────────────────
 *
 * The first version of this function did a plain `DSB; WFI; ISB` with BASEPRI
 * left as the dispatcher set it, on the reasoning that WFI's wake-up condition
 * ignores PRIMASK/FAULTMASK/BASEPRI. **That reasoning was wrong, and the board
 * proved it: the system booted, all five tasks started, and then froze the
 * instant it first had nothing to run.** Halting the debugger found the core
 * parked in this function with the whole application dead.
 *
 * What the Arm ARM actually says is that a WFI wake-up event is an
 * asynchronous exception "at a priority that, if PRIMASK was set to 0, would
 * preempt any currently active exceptions". It excludes PRIMASK from that
 * judgement. It does NOT exclude BASEPRI. And this idle path runs with
 * BASEPRI = INTPRI_VAL(INTPRI_MAX_EXTINT_PRI) = 0x10, while SysTick's priority
 * (SHPR3, verified) is *also* 0x10. An exception only preempts
 * when its priority is strictly higher - numerically lower - than the current
 * execution priority, so SysTick at 0x10 against BASEPRI 0x10 does not
 * qualify, is not a wake-up event, and the core sleeps forever. µT-Kernel's
 * tick is the only thing that could ever have woken this system, so masking it
 * is fatal rather than merely slow.
 *
 * The fix below is the textbook race-free idle, and it is now unconditional -
 * the plain-WFI variant is gone rather than left behind a flag, because it is
 * not a tuning option, it is a hang:
 *
 *   PRIMASK = 1   nothing can be TAKEN from here on, so no interrupt can
 *                 sneak in between the dispatcher's "is anything runnable?"
 *                 check and our WFI and then be slept through;
 *   BASEPRI = 0   but every enabled interrupt is now a valid WFI wake-up
 *                 event, SysTick included;
 *   WFI           sleeps, and returns immediately if something was already
 *                 pending - PRIMASK does not suppress the wake-up;
 *   restore both  BASEPRI goes back to 0x10 before PRIMASK is cleared, so the
 *                 woken exception is still held off here and is taken where
 *                 the dispatcher expects it: at its own `msr basepri, #0`
 *                 two instructions later. That keeps this function's contract
 *                 with dispatch.S exactly as it was.
 * ════════════════════════════════════════════════════════════════════════════ */

/* Idle accounting. Written only by ms_osal_low_power_idle() (single writer,
 * always in the same PendSV context); read by task context via
 * ms_osal_idle_stats(), which re-reads until it sees a stable value because a
 * 64-bit accumulator cannot be updated atomically on a 32-bit core. */
static volatile uint64_t s_idle_cycles  = 0u;
static volatile uint32_t s_idle_entries = 0u;
static bool              s_cyccnt_ok    = false;

/* Enable the DWT cycle counter. Called once from usermain(), in task context,
 * before any task runs. Harmless if a debugger is not attached: TRCENA simply
 * powers the trace block, and CYCCNT free-runs from there. */
static void osal_init_cycle_counter(void)
{
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0u) {
        s_cyccnt_ok = false;      /* this implementation has no cycle counter */
        return;
    }
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    /* Prove it actually ticks rather than assuming the write took. */
    uint32_t a = DWT->CYCCNT;
    __NOP(); __NOP(); __NOP(); __NOP();
    s_cyccnt_ok = (DWT->CYCCNT != a);
}

/* ════════════════════════════════════════════════════════════════════════
 * MS_OSAL_IDLE_WFI - bench experiment display fault
 *
 * Set to 0 to keep every other part of the idle path (the accounting, the
 * PRIMASK/BASEPRI dance, the timing) and remove ONLY the sleep instruction.
 *
 * Why: the cold-boot display fade is the only fault on this board that
 * survives a full LCD re-init and a panel power cycle, and the WFI idle is
 * the only thing added since that runs continuously afterwards. It is
 * also the only mechanism yet proposed that explains "it works if I pause in
 * the debugger for a few seconds" without hand-waving: a halted core does not
 * execute WFI at all.
 *
 * If the screen survives with this at 0, sleep is the cause and the fix is to
 * find what the LTDC loses during CSleep. If it still fades, put this back to
 * 1 immediately - a device that never sleeps fails the power
 * criterion, and the measured 89.6% idle figure is a headline
 * result, not something to trade away for a diagnostic.
 * ════════════════════════════════════════════════════════════════════════ */
#ifndef MS_OSAL_IDLE_WFI
#define MS_OSAL_IDLE_WFI 1
#endif

void ms_osal_low_power_idle(void)
{
    uint32_t before         = DWT->CYCCNT;
    uint32_t saved_primask  = __get_PRIMASK();
    uint32_t saved_basepri  = __get_BASEPRI();

    __disable_irq();          /* PRIMASK = 1 - close the check/sleep race     */
    __set_BASEPRI(0u);        /* every enabled IRQ is a wake-up event again   */
    __DSB();

#if MS_OSAL_IDLE_WFI
    __WFI();
#else
    __NOP();   /* experiment: everything but the sleep */
#endif

    __set_BASEPRI(saved_basepri);   /* re-mask BEFORE re-enabling, so the     */
    __set_PRIMASK(saved_primask);   /* woken exception is taken in dispatch.S */
    __ISB();

    /* 32-bit unsigned subtraction is wrap-safe for any single sleep shorter
     * than 2^32 CPU cycles (~7 s at 600 MHz); the idle path can never sleep
     * that long here because the 1 ms kernel tick always wakes it. */
    s_idle_cycles  += (uint64_t)(DWT->CYCCNT - before);
    s_idle_entries += 1u;
}

bool ms_osal_idle_stats(uint64_t *out_idle_cycles, uint32_t *out_entries)
{
    uint64_t c1, c2;
    uint32_t e;
    do {
        c1 = s_idle_cycles;
        e  = s_idle_entries;
        c2 = s_idle_cycles;
    } while (c1 != c2);   /* retry a torn 64-bit read; values only increase */

    if (out_idle_cycles != NULL) { *out_idle_cycles = c1; }
    if (out_entries     != NULL) { *out_entries     = e;  }
    return s_cyccnt_ok;
}

/* ════════════════════════════════════════════════════════════════════════════
 * HAL tick bridge
 *
 * µT-Kernel owns SysTick outright (mtk3_bsp2 wires vector index 15 straight
 * to its own knl_systim_inthdr() - see sysdepend/stm32_cube/cpu/core/armv8m/
 * interrupt.c - so our old FreeRTOS-era SysTick_Handler() is never called
 * again). Without a bridge, HAL_GetTick()/HAL_Delay() would freeze at boot,
 * silently breaking every HAL peripheral timeout (DCMIPP, SDMMC2, camera
 * ISP,...) that calls them internally. Rather than editing the vendored
 * kernel's interrupt.c, drive HAL_IncTick() from an ordinary µT-Kernel
 * cyclic handler - keeps the bridge entirely inside this file, matching
 * every other kernel-specific decision in this OSAL.
 *
 * Rate note (corrected after hardware testing): HAL_IncTick()
 * adds exactly ONE to HAL's uwTick counter per call. So the cyclic period
 * and the number of increments per firing must together add up to one HAL
 * millisecond per real millisecond, or HAL_GetTick() does not measure
 * milliseconds any more.
 *
 * The first version of this bridge fired every 10 ms (mtk3_bsp2's stock
 * CNF_TIMER_PERIOD) and called HAL_IncTick() once - so HAL_GetTick()
 * advanced 1 ms per 10 ms of real time and HAL's clock ran 10x SLOW. That
 * is not the harmless coarseness the original comment here claimed: every
 * HAL_GetTick()-based deadline in the UI stretched by 10x. The mascot's
 * 250 ms frame gate (anime_ui.h LUMIO_FRAME_MS) became 2.5 s - 0.4 FPS
 * instead of 4 - and state_machine.c's 500 ms touch dwell guard became 5 s,
 * which is what "everything feels slow compared to FreeRTOS" was.
 *
 * CNF_TIMER_PERIOD is now 1 (see mtk3_bsp2/config/config.h for the full
 * reasoning, including the separate tk_dly_tsk() granularity half of the
 * same regression), matching what FreeRTOS's tick did. The period below is
 * DERIVED from that macro rather than hardcoded, and the handler adds one
 * HAL millisecond per millisecond of cyclic period, so the two can never
 * silently disagree again even if the kernel tick is retuned later.
 * ════════════════════════════════════════════════════════════════════════════ */
#define OSAL_HAL_TICK_PERIOD_MS   ((uint32_t)CNF_TIMER_PERIOD)

static void osal_hal_tick_cychdr(void *exinf)
{
    (void)exinf;
    for (uint32_t i = 0u; i < OSAL_HAL_TICK_PERIOD_MS; i++) {
        HAL_IncTick();      /* one HAL millisecond per ms of cyclic period */
    }
}

static void osal_start_hal_tick_bridge(void)
{
    T_CCYC ccyc = {
        .exinf  = NULL,
        .cycatr = TA_HLNG | TA_STA,   /* start immediately on creation */
        .cychdr = (FP)osal_hal_tick_cychdr,
        .cyctim = (RELTIM)OSAL_HAL_TICK_PERIOD_MS,
        .cycphs = 0,
    };
    ID id = tk_cre_cyc(&ccyc);
    if (id <= 0) {
        printf("FATAL: tk_cre_cyc (HAL tick bridge) failed: %d\r\n", (int)id);
        Error_Handler();
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * usermain() - µT-Kernel's application entry point (called by inittask once
 * the kernel is fully up). This is where every osal_*_create() call made
 * from main() before osal_scheduler_start() finally becomes real - see the
 * file header comment for why this indirection exists.
 * ════════════════════════════════════════════════════════════════════════════ */
EXPORT INT usermain(void)
{
    uint32_t i;

    /* 1. Queues first - task bodies may osal_queue_receive() the moment
     *    they start, so their queue must already be real. */
    for (i = 0; i < s_queue_count; i++) {
        s_queues[i].mbfid = osal_start_queue_slot(&s_queues[i]);
        if (s_queues[i].mbfid <= 0) {
            printf("FATAL: tk_cre_mbf (deferred) failed: %d\r\n", (int)s_queues[i].mbfid);
            Error_Handler();
        }
    }

    /* 2. Mutexes - none are actually used by this codebase today, but
     *   created up front for the same reason as queues. */
    for (i = 0; i < s_mutex_count; i++) {
        s_mutexes[i].mtxid = osal_start_mutex_slot();
        if (s_mutexes[i].mtxid <= 0) {
            printf("FATAL: tk_cre_mtx (deferred) failed: %d\r\n", (int)s_mutexes[i].mtxid);
            Error_Handler();
        }
    }

    /* 2b. Event flags - same reasoning as queues: the AI task's
     *     very first statement is a wait on its request flag, so the flag has
     *     to be real before any task is started. */
    for (i = 0; i < s_flag_count; i++) {
        s_flags[i].flgid = osal_start_flag_slot();
        if (s_flags[i].flgid <= 0) {
            printf("FATAL: tk_cre_flg (deferred) failed: %d\r\n", (int)s_flags[i].flgid);
            Error_Handler();
        }
    }

    /* 2c. Alarm handlers - the dose-schedule alarm. Created
     *     before any task starts for the same reason as the flags: the UI
     *     task arms it as soon as it has read the gallery. An alarm that is
     *     not real yet would silently fail to arm and the device would
     *     simply never remind anyone, which is the worst possible failure
     *     mode for this feature - nothing happens, and nothing says so. */
    for (i = 0; i < s_alarm_count; i++) {
        s_alarms[i].almid = osal_start_alarm_slot(&s_alarms[i]);
        if (s_alarms[i].almid <= 0) {
            printf("FATAL: tk_cre_alm (deferred) failed: %d\r\n", (int)s_alarms[i].almid);
            Error_Handler();
        }
    }

    /* 3. HAL tick bridge - must be running before any task that touches a
     *    HAL peripheral (camera/SD) starts. */
    osal_start_hal_tick_bridge();

    /* 3b. DWT cycle counter for the earlier idle accounting. Done here,
     *     in task context, because ms_osal_low_power_idle() itself runs in
     *     PendSV with interrupts masked and must do nothing but read it. */
    osal_init_cycle_counter();

    /* From here on, a fresh osal_task_create() call would create for real
     * immediately instead of deferring - see osal_task_create() above. */
    s_kernel_running = true;

    /* 4. Tasks - started last, so every queue/mutex a task might touch on
     *    its very first line is already real. */
    for (i = 0; i < s_task_count; i++) {
        s_tasks[i].tskid = osal_start_task_slot(&s_tasks[i]);
        if (s_tasks[i].tskid <= 0) {
            printf("FATAL: tk_cre_tsk (deferred) failed for task '%s': %d\r\n",
                   s_tasks[i].name, (int)s_tasks[i].tskid);
            Error_Handler();
        }
    }

    /* usermain() itself is not a task we schedule work on - sleep forever.
     * (Matches the pattern in every mtk3_bsp2 sample's own usermain().) */
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
