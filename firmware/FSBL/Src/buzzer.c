/* buzzer.c - active piezo buzzer on PA3, driven by the Alert task
 *
 * PIN. Arduino D10 / PA3, main VDD I/O domain, plain push-pull output.
 * Reserved for this before the dispenser driver was written precisely so it
 * could not be taken later: a buzzer
 * which stole a pin from the dispenser would be a worse outcome than no
 * buzzer. PA3's alternate function is TIM16_CH1, which a passive buzzer
 * would have needed; an active one does not, so TIM16 is left free.
 *
 * NO TRANSISTOR. The part is an active PIEZO buzzer - a few milliamps at
 * 3.3 V - which a GPIO sources comfortably. A MAGNETIC active buzzer draws
 * 25-35 mA and would need an NPN and a flyback diode; this is not one.
 *
 * NO DMA, NO TIMER, so the sleep-clock rule does not apply:
 * ms_configure_sleep_clocks() needs no new LPEN bit for this module. The
 * tone is a GPIO held high by a task that sleeps between edges, and the
 * task's own osal_delay_ms() is what keeps the kernel tick running.
 */

#include "buzzer.h"
#include "ms_osal.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>

#define BUZZ_PORT       GPIOA
#define BUZZ_PIN        GPIO_PIN_3     /* Arduino D10 */

/* Event-flag bit. One bit is enough: the requested pattern travels in a
 * plain variable beside it, because only the most important pending request
 * is ever worth playing and the priority rule below resolves that at
 * request time rather than queueing. */
#define BUZZ_FLAG_REQ   (1u << 0)

/* ═══════════════════════════════════════════════════════════════════════════
 * The patterns
 *
 * Each is a NUL-terminated list of durations in milliseconds, alternating
 * ON, OFF, ON, OFF... A trailing OFF is harmless and keeps repeats separated.
 *
 * Rhythm carries the meaning because an active buzzer has one pitch:
 *
 *   TICK           a single 10 ms blip. Deliberately below what reads as a
 *                  "beep" - it should feel like a key travelling, not an
 *                  alert. Anything longer becomes irritating at speed.
 *   DISPENSE_OK    two quick beeps: something finished, successfully.
 *   DISPENSE_FAIL  four rapid beeps: the universal "that did not work".
 *   DOSE_REMINDER  three polite double-pulses, patient-facing. Short tones,
 *                  so it asks rather than demands.
 *   DOSE_MISSED    four LONG pulses, twice. The only pattern built from long
 *                  tones, because it is the only one that has to carry to a
 *                  carer in another room. Nothing else sounds like it.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint16_t pat_tick[]          = { 10, 0 };
static const uint16_t pat_dispense_ok[]   = { 80, 70, 80, 0 };
static const uint16_t pat_dispense_fail[] = { 60, 60, 60, 60, 60, 60, 60, 0 };
static const uint16_t pat_dose_reminder[] = { 150, 120, 150, 400,
                                              150, 120, 150, 400,
                                              150, 120, 150, 0 };
static const uint16_t pat_dose_missed[]   = { 500, 200, 500, 200,
                                              500, 200, 500, 700,
                                              500, 200, 500, 200,
                                              500, 200, 500, 0 };

static const uint16_t *const s_patterns[BUZZ_PATTERN_COUNT] = {
    [BUZZ_TICK]           = pat_tick,
    [BUZZ_DISPENSE_OK]    = pat_dispense_ok,
    [BUZZ_DISPENSE_FAIL]  = pat_dispense_fail,
    [BUZZ_DOSE_REMINDER]  = pat_dose_reminder,
    [BUZZ_DOSE_MISSED]    = pat_dose_missed,
};

/* Importance, so a tap tick can never cut off a missed-dose alert. Higher
 * wins; an equal or lower request arriving mid-pattern is simply dropped. */
static const uint8_t s_rank[BUZZ_PATTERN_COUNT] = {
    [BUZZ_TICK]           = 0,
    [BUZZ_DISPENSE_OK]    = 1,
    [BUZZ_DISPENSE_FAIL]  = 2,
    [BUZZ_DOSE_REMINDER]  = 2,
    [BUZZ_DOSE_MISSED]    = 3,
};

static osal_flag_handle_t s_flag        = NULL;
static volatile uint8_t   s_req_pattern = BUZZ_TICK;
static volatile bool      s_req_pending = false;
static volatile bool      s_playing     = false;
static volatile uint8_t   s_playing_rank = 0;
static volatile bool      s_abort       = false;
static bool               s_ready       = false;

/* ═══════════════════════════════════════════════════════════════════════════
 * Init
 * ═══════════════════════════════════════════════════════════════════════════ */

void buzzer_init(void)
{
#if MEDSIGHT_BUZZER
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* Silent first. GPIO ODR resets to zero, so configuring the pin as an
     * output drives it low with no intervening glitch - the device must not
     * chirp on its way through boot. */
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);

    gi.Pin   = BUZZ_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;    /* an on/off line, not a signal */
    HAL_GPIO_Init(BUZZ_PORT, &gi);

    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);

    s_ready = true;
    printf("buzzer: active piezo on PA3 (Arduino D10), keyboard click %s\r\n",
           MEDSIGHT_BUZZER_KEYBOARD_CLICK ? "ON" : "OFF");
#else
    printf("buzzer: MEDSIGHT_BUZZER=0 - silent build\r\n");
#endif
}

void buzzer_service_init(void)
{
#if MEDSIGHT_BUZZER
    /* Created here in main(), alongside every other osal_*_create(), for the
     * reason main.c already states for the AI flag: ms_osal.c's "create after
     * the kernel is running" branch exists but has never been exercised on
     * hardware, and this is not the place to become the first thing that
     * depends on it. */
    s_flag = osal_flag_create();
    if (s_flag == NULL) {
        printf("buzzer: flag create FAILED - device will be silent\r\n");
    }
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Requests
 * ═══════════════════════════════════════════════════════════════════════════ */

void buzzer_play(buzz_pattern_t pattern)
{
#if MEDSIGHT_BUZZER
    if (!s_ready || (s_flag == NULL) || (pattern >= BUZZ_PATTERN_COUNT)) {
        return;
    }

    /* A more important sound displaces a less important one; anything else
     * arriving mid-pattern is dropped rather than queued. A tick that had to
     * wait out a missed-dose alert would arrive seconds after the tap that
     * caused it, which is worse than no tick at all. */
    if (s_playing && (s_rank[pattern] <= s_playing_rank)) {
        return;
    }
    if (s_playing) {
        s_abort = true;    /* the task checks this between every edge */
    }

    s_req_pattern = (uint8_t)pattern;
    s_req_pending = true;
    osal_flag_set(s_flag, BUZZ_FLAG_REQ);
#else
    (void)pattern;
#endif
}

void buzzer_tick(bool is_keyboard)
{
#if MEDSIGHT_BUZZER
    if (is_keyboard && !MEDSIGHT_BUZZER_KEYBOARD_CLICK) {
        return;
    }
    buzzer_play(BUZZ_TICK);
#else
    (void)is_keyboard;
#endif
}

void buzzer_stop(void)
{
#if MEDSIGHT_BUZZER
    s_abort = true;
    if (s_ready) {
        HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);
    }
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * The Alert task
 *
 * PRIORITY 1, the same level as heartbeat, and here is the reason
 * rather than an assumption. A tone has no deadline of any
 * kind: nothing waits on it, nothing is incorrect if it is late, and a person
 * cannot tell 10 ms of jitter from none. Every other task in the table has a
 * reason to preempt it - the camera has a VSYNC to meet, the UI has a
 * touch-response budget, the AI has somebody waiting on a result, the logger
 * holds the audit trail. So this sits at the bottom with heartbeat, and the
 * rate-monotonic derivation in main.c is untouched:
 * nothing above it moved.
 *
 * It also does what Part E2 actually asks for, which is that a tone outlives
 * the call that triggered it. state_machine_update() returns immediately
 * after buzzer_play(); the sound continues here.
 * ═══════════════════════════════════════════════════════════════════════════ */

void buzzer_task_fn(void *arg)
{
    (void)arg;

#if MEDSIGHT_BUZZER
    for (;;) {
        uint32_t got = 0u;

        if (!osal_flag_wait(s_flag, BUZZ_FLAG_REQ,
                            OSAL_FLAG_WAIT_OR | OSAL_FLAG_WAIT_CLEAR,
                            &got, OSAL_WAIT_FOREVER)) {
            osal_delay_ms(100);
            continue;
        }

        if (!s_req_pending) {
            continue;
        }

        uint8_t pattern = s_req_pattern;
        s_req_pending   = false;
        s_abort         = false;
        s_playing       = true;
        s_playing_rank  = s_rank[pattern];

        const uint16_t *seq = s_patterns[pattern];

        for (uint32_t i = 0; (seq[i] != 0u) && !s_abort; i++) {
            /* Even indices are ON, odd are OFF. */
            HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN,
                              ((i & 1u) == 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            osal_delay_ms(seq[i]);
        }

        /* EVERY exit path leaves it silent, including the aborted one. */
        HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);
        s_playing      = false;
        s_playing_rank = 0;
    }
#else
    for (;;) {
        osal_delay_ms(60000);
    }
#endif
}
