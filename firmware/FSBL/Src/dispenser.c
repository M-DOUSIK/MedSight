/* dispenser.c - 28BYJ-48 turntable + IR pill counter
 *
 * PIN BUDGET. Traced against this firmware's live consumers before a line of
 * this file was written - the LTDC block in stm32n6xx_hal_msp.c, the I2C
 * defines in stm32n6570_discovery_bus.h, and the SD/camera/LED assignments in
 * the BSP. All five pins sit on the main VDD I/O domain, so the
 * unpowered-bank trap cannot apply to any of them;
 * no HAL_PWREx_EnableVddIOn() call is needed for GPIOA or GPIOD.
 *
 *   Arduino A0  PA5    ULN2003 IN1
 *   Arduino A1  PA9    ULN2003 IN2
 *   Arduino A2  PA10   ULN2003 IN3
 *   Arduino A3  PA12   ULN2003 IN4
 *   Arduino D2  PD0    IR module OUT   (EXTI0)
 *
 *   Arduino D10 PA3    RESERVED, unwired - buzzer, TIM16_CH1 AF1 (Part E)
 *   Arduino D14 PC1 and D15 PH9 are the camera's I2C1 and are OFF LIMITS.
 *
 * The four coil lines are one bank at four consecutive header pins, so a
 * half-step is a single atomic GPIOA->BSRR write: no read-modify-write, and
 * no torn coil pattern if the UI task is preempted mid-step.
 *
 * NO NEW DMA DESTINATION, so the sleep-clock rule
 * does not bite here: nothing in this module is a bus master, and
 * ms_configure_sleep_clocks() needs no new LPEN bit. The one sleep-related
 * consequence is latency, not loss - see the note above the ISR.
 */

#include "dispenser.h"
#include "ms_osal.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>

#ifndef MEDSIGHT_DEBUG
#define MEDSIGHT_DEBUG 0
#endif
#if MEDSIGHT_DEBUG
#define MS_DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define MS_DBG_PRINTF(...) do { } while (0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Pins
 *
 * TWO COIL PIN SETS, selectable with one number, because the first hardware
 * run never lit a single LED on the driver board and "the header pin is not
 * what we think" is one of the few explanations a firmware change can test.
 *
 *   MS_COIL_PINSET 0   CN7, the ANALOG header   A0-A3  = PA5/PA9/PA10/PA12
 *   MS_COIL_PINSET 1   CN11/CN12, DIGITAL       D3/D5/D6/D9 = PE9/PE10/PE13/PE14
 *
 * Both sets are one GPIO bank, so a half-step stays a single atomic BSRR
 * write either way. Both are free of every live consumer in this firmware -
 * traced against the LTDC block in stm32n6xx_hal_msp.c, the I2C defines in
 * stm32n6570_discovery_bus.h, and the SD/camera/audio/LED assignments in the
 * BSP.
 *
 * ONE DIFFERENCE WORTH KNOWING. GPIOA sits on the main VDD I/O domain, which
 * is powered from reset and needs no HAL_PWREx call. GPIOE sits on VDDIO5 -
 * which main() already enables unconditionally at boot (the earlier fix,
 * enabled at boot), so set 1 is safe, but it depends on that call in a
 * way set 0 does not. If set 1 is ever used on a board where that enable has
 * been removed, that trap is live again and the pins read and drive
 * nothing at all, silently.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef MS_COIL_PINSET
#define MS_COIL_PINSET  1
#endif

#if (MS_COIL_PINSET == 0)
  #define COIL_PORT       GPIOA
  #define COIL_IN1_PIN    GPIO_PIN_5    /* CN7  A0  PA5  */
  #define COIL_IN2_PIN    GPIO_PIN_9    /* CN7  A1  PA9  */
  #define COIL_IN3_PIN    GPIO_PIN_10   /* CN7  A2  PA10 */
  #define COIL_IN4_PIN    GPIO_PIN_12   /* CN7  A3  PA12 */
  #define COIL_CLK_ENABLE()  __HAL_RCC_GPIOA_CLK_ENABLE()
  #define COIL_SET_NAME   "CN7 analog A0-A3 (PA5/PA9/PA10/PA12)"
  #define COIL_N1 "IN1  A0/PA5"
  #define COIL_N2 "IN2  A1/PA9"
  #define COIL_N3 "IN3  A2/PA10"
  #define COIL_N4 "IN4  A3/PA12"
#else
  #define COIL_PORT       GPIOE
  #define COIL_IN1_PIN    GPIO_PIN_9    /* CN11/12  D3  PE9  */
  #define COIL_IN2_PIN    GPIO_PIN_10   /* CN11/12  D5  PE10 */
  #define COIL_IN3_PIN    GPIO_PIN_13   /* CN11/12  D6  PE13 */
  #define COIL_IN4_PIN    GPIO_PIN_14   /* CN11/12  D9  PE14 */
  #define COIL_CLK_ENABLE()  __HAL_RCC_GPIOE_CLK_ENABLE()
  #define COIL_SET_NAME   "digital D3/D5/D6/D9 (PE9/PE10/PE13/PE14)"
  #define COIL_N1 "IN1  D3/PE9"
  #define COIL_N2 "IN2  D5/PE10"
  #define COIL_N3 "IN3  D6/PE13"
  #define COIL_N4 "IN4  D9/PE14"
#endif

#define COIL_ALL_PINS   (COIL_IN1_PIN | COIL_IN2_PIN | COIL_IN3_PIN | COIL_IN4_PIN)

#define IR_PORT         GPIOD
#define IR_PIN          GPIO_PIN_0    /* Arduino D2 */
#define IR_EXTI_IRQn    EXTI0_IRQn

/* ═══════════════════════════════════════════════════════════════════════════
 * Tuning constants - every one of them measurable, and stated as a starting
 * value rather than a truth. The console prints each break's duration on
 * every dispense precisely so these can be corrected from real data.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Step period. The 28BYJ-48 is open-loop and stalls silently if driven too
 * fast; start around 2 ms and only speed up once
 * the mechanism is proven. With a 1 ms kernel tick osal_delay_ms(2) yields
 * 2-3 ms in practice, which is on the safe side of that. */
#define MS_STEP_PERIOD_MS       2u

/* ── WHICH WAY THE TURNTABLE TURNS ────────────────────────────────────────
 *
 * The turntable turns ANTICLOCKWISE to dispense, which is what the built
 * hopper and ramp need. This comment used to say the mechanism did not exist
 * yet and the handedness was not knowable from here; it does now, and it is
 * settled. A differently-handed build flips this to 1 rather than rewiring.
 *
 * One line flips the whole module consistently: the dispensing rotation AND
 * the one-second settle-back at the end of a successful dose, which must
 * always oppose it.
 *
 *    1  = the direction the bench rig turned on 2026-09-12 (clockwise)
 *   -1  = anticlockwise - what the built hopper and ramp need, and the
 *         value this ships with
 *
 * Nothing else needs touching, and the self-test deliberately ignores this
 * and always goes forward-then-back, so it stays a symmetric wiring check
 * rather than a test of the mechanism's handedness. */
#ifndef MS_DISPENSE_DIRECTION
#define MS_DISPENSE_DIRECTION   (-1)
#endif

#if (MS_DISPENSE_DIRECTION >= 0)
  #define STEP_DISPENSE   true
  #define STEP_SETTLE     false
#else
  #define STEP_DISPENSE   false
  #define STEP_SETTLE     true
#endif

/* THE PILLS SLIDE. This is the constant that the ramp changes.
 *
 * A 10 mm pill in free fall crosses the beam at 1-2 m/s - 5-10 ms. Sliding
 * at 0.2-0.5 m/s it breaks the beam for 20-50 ms or more. A long break is
 * therefore the SIGNAL, not noise, and a debounce window sized for a drop
 * would throw away real counts.
 *
 * The dominant error also flips. Free fall separates pills; a ramp lets them
 * slide nose-to-tail in contact, so TWO pills can produce ONE continuous
 * break. Aggressive debouncing makes that worse, not better. So this is set
 * to the smallest value that suppresses genuine LM393 chatter (sub-millisecond
 * to a couple of ms) and nothing more.
 *
 * Anything shorter than this is discarded as chatter and logged as such.
 *
 * MEASURED, 2026-09-12, three successful dispenses on the real sensor:
 *   genuine pill breaks   17, 27, 27, 31, 33, 35, 46 ms
 *   chatter               0-1 ms, hundreds of them
 * The two populations do not overlap and are an order of magnitude apart, so
 * 8 ms sits in the empty gap with margin on both sides: well above every
 * observed chatter pulse, well below the shortest real pill. Raised from the
 * initial guess of 5 ms on that evidence - the guess was sound but 5 ms left
 * only 4 ms of headroom over the noise. */
#define MS_IR_MIN_BREAK_MS      8u

/* THE STALL BOUND IS GONE, and its removal is a finding rather than a
 * simplification.
 *
 * The original reasoning was sound: a pill can stall in the beam, friction on
 * a ramp stops it dead, and a stall is detectable where a pill that never
 * left the hopper is invisible. What was wrong was the number. At 1500 ms it
 * sat only 290 ms above the longest legitimate single-pill break the hardware
 * actually produced - 1210 ms, with an 852 ms alongside it, both counted
 * correctly on 2026-09-12. On a medication device that margin means a
 * slightly slower pill gets a dose refused for a jam that did not happen.
 *
 * A stalled pill is not a separate condition needing its own detector: it
 * stops producing counts, and MS_NO_PILL_TIMEOUT_MS catches it with
 * everything else that stops producing counts. One bound, no false alarms. */

/* THE ONLY BOUND on a dispense: how long since the last pill arrived.
 *
 * Every wait still bounded, per this project's
 * rule about polling loops - but bounded on the right quantity. A working
 * mechanism restarts this clock with every pill and may take as long as it
 * takes; a stopped one is caught within one window. See the long note at the
 * check itself for why the previous stall bound was dangerous.
 *
 * 25 s is comfortably beyond any hand-fed or ramp-fed pill and still quick
 * enough that a patient is not left watching a dead machine. */
#define MS_NO_PILL_TIMEOUT_MS   25000u

/* ── IR POLARITY, and the bug that made this a setting ────────────────────
 *
 * The first hardware run learned the polarity backwards and reported an
 * instant JAM on a perfectly good sensor. dispenser_init() sampled the line
 * ONCE, 5 ms after configuring the pin, and locked that value in as "idle":
 *
 *     dispenser:... idle=LOW -> break=HIGH
 *     dispenser: beam ALREADY broken at start
 *     dispenser: JAM - 0 of 3 counted in 1495 ms
 *
 * The line read LOW at boot and HIGH by the time a dose was dispensed, so
 * every clear beam looked broken. One unvalidated sample is not enough to
 * decide something the entire count depends on.
 *
 * Three fixes, in order of how much they are trusted:
 *   1. MS_IR_IDLE_FORCE, below - once the level is MEASURED on the bench,
 *      state it here and nothing has to infer anything ever again.
 *   2. Failing that, the level is re-learned at the START OF EVERY DISPENSE
 *      and required to hold still for MS_IR_SETTLE_MS first. A line that is
 *      still moving is reported rather than silently believed.
 *   3. A pre-existing beam state never arms the jam timer. A stall is a
 *      condition that BEGINS during a dispense; something already true when
 *      we started is not a stall, and treating it as one is what turned a
 *      wrong guess into a 1.5-second failure instead of a slow one.
 *
 * Set to 0 or 1 to force it; leave at -1 to keep learning it.
 *
 * NOW MEASURED, so it is forced. Across three successful dispenses the line
 * idled HIGH every time and a pill pulled it LOW. Boot-time sampling remained
 * unreliable even after the settle logic - one boot read the line LOW for its
 * whole two-second trace and the per-dispense re-learn corrected it to HIGH -
 * which is the module needing longer than a boot to stabilise. There is no
 * reason to keep inferring a value we have now observed seven times. */
#ifndef MS_IR_IDLE_FORCE
#define MS_IR_IDLE_FORCE        1
#endif

/* The line must read the same for this long before it is believed. */
#define MS_IR_SETTLE_MS         120u

/* Boot self-test. Prints the raw IR line for two seconds and wiggles the
 * turntable, so "is the sensor connected" and "is the motor wired and
 * powered" are answered before any dose depends on them. Set to 0 once the
 * rig is trusted - it costs about five seconds of boot. */
#ifndef MS_DISPENSER_SELFTEST
#define MS_DISPENSER_SELFTEST   0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Half-step (8-phase) sequence
 *
 * Smoother than full-step and double the resolution: the 28BYJ-48's 1:64
 * gearbox gives ~4096 half-steps per output revolution. Each table entry is
 * a complete BSRR word - set bits in the low half, reset bits in the high
 * half - so one store drives all four coils to their new state at once.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* The actuator half of this file exists only in the hardware build. In the
 * Part C simulation build the ISR helpers below still compile - stm32n6xx_it.c
 * references them unconditionally - but nothing steps a motor, and leaving
 * these in would cost three -Wunused-function warnings against a project that
 * holds a zero-warning build. */
#if MEDSIGHT_PHYSICAL_DISPENSER

#define BSRR_WORD(a, b, c, d)                                                 \
    ( ((a) ? COIL_IN1_PIN : ((uint32_t)COIL_IN1_PIN << 16))                   \
    | ((b) ? COIL_IN2_PIN : ((uint32_t)COIL_IN2_PIN << 16))                   \
    | ((c) ? COIL_IN3_PIN : ((uint32_t)COIL_IN3_PIN << 16))                   \
    | ((d) ? COIL_IN4_PIN : ((uint32_t)COIL_IN4_PIN << 16)) )

static const uint32_t s_halfstep[8] = {
    BSRR_WORD(1, 0, 0, 0),
    BSRR_WORD(1, 1, 0, 0),
    BSRR_WORD(0, 1, 0, 0),
    BSRR_WORD(0, 1, 1, 0),
    BSRR_WORD(0, 0, 1, 0),
    BSRR_WORD(0, 0, 1, 1),
    BSRR_WORD(0, 0, 0, 1),
    BSRR_WORD(1, 0, 0, 1),
};

/* All four coils off. Used on EVERY exit path - leaving one energised holds
 * torque the gearbox already provides mechanically, and cooks both the coil
 * and the ULN2003 for no benefit. */
#define COILS_OFF_BSRR  ((uint32_t)COIL_ALL_PINS << 16)

#endif /* MEDSIGHT_PHYSICAL_DISPENSER - actuator tables */

/* ═══════════════════════════════════════════════════════════════════════════
 * ISR-shared state
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Completed breaks, produced by the ISR and drained by the dispense loop.
 * The ISR must not printf, because of
 * what a slow interrupt path costs on this board - so durations are queued
 * here and printed from task context. */
#define BREAK_LOG_LEN   16u

typedef struct {
    uint32_t duration_ms;
    bool     counted;       /* false = discarded as chatter */
} break_record_t;

static volatile break_record_t s_break_log[BREAK_LOG_LEN];
static volatile uint32_t       s_break_wr = 0;
#if MEDSIGHT_PHYSICAL_DISPENSER
static uint32_t                s_break_rd = 0;   /* drained from task context */
#endif

static volatile uint32_t s_pill_count   = 0;   /* counted this dispense     */
static volatile uint32_t s_total_pills  = 0;   /* since boot                */
static volatile bool     s_beam_broken  = false;
static volatile uint32_t s_break_start  = 0;

static GPIO_PinState s_idle_level = GPIO_PIN_SET;  /* sampled at init */
static bool          s_ready      = false;
#if MEDSIGHT_PHYSICAL_DISPENSER
static uint8_t       s_step_index = 0;
#endif

static dispenser_progress_fn_t s_progress_cb = NULL;

#if MEDSIGHT_PHYSICAL_DISPENSER
/* Sub-threshold pulses, counted rather than printed one per line. */
static uint32_t s_chatter_run   = 0;   /* since the last counted pill */
static uint32_t s_chatter_total = 0;   /* this dispense               */
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * The ISR
 *
 * Both edges, because on a ramp the DURATION is the information: a long
 * break may be two pills nose-to-tail, and a break that never ends is a
 * stalled pill. The falling edge opens a break, the rising edge closes it.
 *
 * Nothing here printf()s, and nothing here calls any tk_* or osal_* API.
 * HAL_GetTick() is a plain 32-bit read of uwTick and is safe from an ISR.
 *
 * LATENCY, NOT LOSS. The idle path runs with BASEPRI = 0x10, so an interrupt
 * in the normal peripheral band cannot itself wake the core out of WFI
 * (the Arm ARM counts BASEPRI when deciding
 * what constitutes a wake-up event). The EXTI pending bit latches regardless,
 * so no edge is ever lost; the handler simply runs on the next SysTick wake,
 * up to about a millisecond later. Against break durations of 20-50 ms that
 * is a 2-5% error on the measurement and none at all on the count. If µs
 * accuracy is ever wanted, this IRQ can move to priority 0 - it touches no
 * kernel API, so it is safe above the kernel's mask - but that trades a real
 * preemption risk for a number we do not need.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void ir_break_opened(void)
{
    if (!s_beam_broken) {
        s_beam_broken = true;
        s_break_start = HAL_GetTick();
    }
}

static inline void ir_break_closed(void)
{
    if (s_beam_broken) {
        uint32_t dur = HAL_GetTick() - s_break_start;
        bool     ok  = (dur >= MS_IR_MIN_BREAK_MS);

        s_beam_broken = false;

        if (ok) {
            s_pill_count++;
            s_total_pills++;
        }

        uint32_t w = s_break_wr % BREAK_LOG_LEN;
        s_break_log[w].duration_ms = dur;
        s_break_log[w].counted     = ok;
        s_break_wr++;
    }
}

void dispenser_exti_rising(void)
{
    if (s_idle_level == GPIO_PIN_RESET) {
        ir_break_opened();      /* active-HIGH module: rising = beam broken */
    } else {
        ir_break_closed();      /* active-LOW  module: rising = beam clear  */
    }
}

void dispenser_exti_falling(void)
{
    if (s_idle_level == GPIO_PIN_RESET) {
        ir_break_closed();
    } else {
        ir_break_opened();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Init
 * ═══════════════════════════════════════════════════════════════════════════ */

#if MEDSIGHT_PHYSICAL_DISPENSER

/* Defined with the stepping helpers further down; the self-test needs them. */
static void step_once(bool forward);
static void coils_off(void);

/* The settle helper and the self-test are only compiled where something
 * actually calls them. With the polarity now forced from measurement and
 * the self-test retired, both would otherwise sit unused and cost the
 * project's zero-warning build. */
#if (MS_IR_IDLE_FORCE < 0) || MS_DISPENSER_SELFTEST

/* Read the IR line until it has held the same value for MS_IR_SETTLE_MS, or
 * give up after `bound_ms`. Returns the settled level; *out_stable says
 * whether it actually settled or we ran out of patience. Bounded, per
 * this project's rule about polling loops. */
static GPIO_PinState ir_settled_level(uint32_t bound_ms, bool *out_stable)
{
    uint32_t      t0    = HAL_GetTick();
    GPIO_PinState level = HAL_GPIO_ReadPin(IR_PORT, IR_PIN);
    uint32_t      since = HAL_GetTick();

    while ((HAL_GetTick() - t0) < bound_ms) {
        GPIO_PinState now = HAL_GPIO_ReadPin(IR_PORT, IR_PIN);
        if (now != level) {
            level = now;
            since = HAL_GetTick();
        } else if ((HAL_GetTick() - since) >= MS_IR_SETTLE_MS) {
            if (out_stable != NULL) { *out_stable = true; }
            return level;
        }
    }

    if (out_stable != NULL) { *out_stable = false; }
    return level;
}

#endif /* settle helper */

#if MS_DISPENSER_SELFTEST

/* Boot self-test. Answers the two questions that cost the first hardware run:
 * is the sensor line alive and steady, and is the motor actually wired and
 * powered. Both are visible in five seconds instead of inferred from a failed
 * dose. */
static void dispenser_selftest(void)
{
    printf("dispenser selftest: IR line for 2 s "
           "(block the slot and watch it change)\r\n");
    printf("  ");
    for (int i = 0; i < 40; i++) {
        printf("%c", (HAL_GPIO_ReadPin(IR_PORT, IR_PIN) == GPIO_PIN_SET) ? 'H' : 'L');
        HAL_Delay(50);
    }
    printf("\r\n");

    bool          stable = false;
    GPIO_PinState lvl    = ir_settled_level(500u, &stable);
    printf("dispenser selftest: IR settles %s (%s)\r\n",
           (lvl == GPIO_PIN_SET) ? "HIGH" : "LOW",
           stable ? "steady" : "STILL MOVING - check wiring or ambient IR");

    /* ── Per-pin test ─────────────────────────────────────────────────────
     *
     * Three different faults all present as "the motor does not move", and
     * this separates them. Each coil line is driven HIGH on its own for two
     * seconds - long enough to get a multimeter probe on it - and read back
     * through IDR, which is the pin's ACTUAL electrical level rather than
     * what we asked for.
     *
     *   log HIGH, reads back HIGH, meter ~3.3 V   the MCU side is correct;
     *                                             the fault is past the pin
     *   log HIGH, reads back LOW                  something else owns or is
     *                                             holding this pin
     *   meter ~3.3 V but the ULN LED stays dark   wiring to the driver, or
     *                                             the driver's own supply
     *
     * Measure against CN8 GND, not against the ULN board's ground, so a
     * missing ground link cannot hide inside the measurement. */
    {
        static const struct { uint16_t pin; const char *name; } coil[4] = {
            { COIL_IN1_PIN, COIL_N1 },
            { COIL_IN2_PIN, COIL_N2 },
            { COIL_IN3_PIN, COIL_N3 },
            { COIL_IN4_PIN, COIL_N4 },
        };

        printf("dispenser selftest: per-pin, 2 s each - "
               "meter each pin against CN8 GND\r\n");

        for (int i = 0; i < 4; i++) {
            COIL_PORT->BSRR = coil[i].pin;
            HAL_Delay(20);
            bool idr = ((COIL_PORT->IDR & coil[i].pin) != 0u);
            printf("  %-13s driven HIGH -> reads back %s\r\n",
                   coil[i].name,
                   idr ? "HIGH   ok" : "LOW    <-- PIN IS NOT GOING HIGH");
            HAL_Delay(2000);
            COIL_PORT->BSRR = ((uint32_t)coil[i].pin << 16);
            HAL_Delay(250);
        }
    }

    /* Slow chase: sixteen half-steps at 200 ms. Fast enough to finish in
     * three seconds, slow enough that the ULN2003's four LEDs visibly walk
     * around the sequence instead of blurring. If they light here, the
     * signal path is proven end to end. */
    printf("dispenser selftest: slow chase, 16 half-steps at 200 ms - "
           "watch the driver LEDs\r\n");
    for (int i = 0; i < 16; i++) {
        step_once(true);
        HAL_Delay(200);
    }

    /* 1024 half-steps each way: a quarter turn of the output shaft, forward
     * then back, so the rig ends where it started. */
    printf("dispenser selftest: motor 1024 half-steps forward...\r\n");
    for (int i = 0; i < 1024; i++) { step_once(true);  HAL_Delay(MS_STEP_PERIOD_MS); }
    HAL_Delay(300);
    printf("dispenser selftest: motor 1024 half-steps back...\r\n");
    for (int i = 0; i < 1024; i++) { step_once(false); HAL_Delay(MS_STEP_PERIOD_MS); }
    coils_off();
    printf("dispenser selftest: done, coils off\r\n");
}
#endif /* MS_DISPENSER_SELFTEST */

#endif /* MEDSIGHT_PHYSICAL_DISPENSER - settle helper + self-test */

void dispenser_init(void)
{
#if MEDSIGHT_PHYSICAL_DISPENSER
    GPIO_InitTypeDef gi = {0};

    COIL_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Coils first, and OFF first.
     *
     * GPIO ODR resets to zero, so configuring these as outputs drives them
     * low with no intervening glitch - the same mechanism that had the
     * display accidentally holding the touch controller in reset
     *  working in our favour here. The explicit
     * write is belt-and-braces in case this is ever called twice. */
    COIL_PORT->BSRR = COILS_OFF_BSRR;

    gi.Pin   = COIL_ALL_PINS;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;   /* a stepper coil is not a fast signal */
    HAL_GPIO_Init(COIL_PORT, &gi);

    COIL_PORT->BSRR = COILS_OFF_BSRR;

    /* IR sensor: plain input, NO internal pull-up. The module drives the line
     * actively (or open-collector with its own pull-up on the board); adding
     * ours would fight it. */
    gi.Pin  = IR_PIN;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(IR_PORT, &gi);

    /* Let the line settle, then learn the polarity instead of assuming it.
     *
     * Most of these modules are active LOW - OUT idles HIGH and is pulled LOW
     * when the beam breaks - but "most" is not "yours", and getting it
     * backwards counts the GAPS BETWEEN pills instead of the pills. Sampling
     * it here means an active-HIGH module works with no rebuild, and the
     * detected level is printed, so it is recorded rather than assumed
     * anywhere. */
    {
#if (MS_IR_IDLE_FORCE >= 0)
        /* No settle wait. The level is known from measurement, so spending
         * 600 ms of boot sampling a line we would immediately overwrite is
         * pure cost. */
        s_idle_level = (MS_IR_IDLE_FORCE != 0) ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
        bool stable = false;
        s_idle_level = ir_settled_level(600u, &stable);
        if (!stable) {
            printf("dispenser: WARNING - IR line never settled at boot; "
                   "it is re-learned at every dispense\r\n");
        }
#endif
    }

    gi.Pin  = IR_PIN;
    gi.Mode = GPIO_MODE_IT_RISING_FALLING;
    gi.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(IR_PORT, &gi);

    /* Priority 6: the same band as DMA2D, numerically above the kernel's
     * BASEPRI mask so it can never preempt a kernel critical section. This
     * ISR calls no kernel API, so it would be safe higher - see the latency
     * note above the ISR for why it does not need to be. */
    HAL_NVIC_SetPriority(IR_EXTI_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(IR_EXTI_IRQn);

    s_ready = true;

    /* Unconditional printf, not MS_DBG_PRINTF: this line IS a deliverable.
     * Part 0 asks for the module's measured idle/broken polarity in writing,
     * and a Release build that cannot tell you which way round its own
     * sensor is wired is worse than useless on a bench. */
    printf("dispenser: coils %s, IR PD0 (EXTI0), idle=%s -> break=%s\r\n",
           COIL_SET_NAME,
           (s_idle_level == GPIO_PIN_SET) ? "HIGH" : "LOW",
           (s_idle_level == GPIO_PIN_SET) ? "LOW"  : "HIGH");

#if MS_DISPENSER_SELFTEST
    dispenser_selftest();
#endif
#else
    printf("dispenser: MEDSIGHT_PHYSICAL_DISPENSER=0 - simulated dispense\r\n");
    s_ready = false;
#endif
}

bool dispenser_is_ready(void)
{
    return s_ready;
}

void dispenser_set_progress_cb(dispenser_progress_fn_t cb)
{
    s_progress_cb = cb;
}

uint32_t dispenser_get_total_dispensed(void)
{
    return s_total_pills;
}

const char *dispenser_result_str(dispense_result_t r)
{
    switch (r) {
        case DISPENSE_OK:        return "OK";
        case DISPENSE_SHORT:     return "SHORT";
        case DISPENSE_JAM:       return "JAM";
        case DISPENSE_NOT_READY: return "NOT_READY";
        default:                 return "?";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stepping
 * ═══════════════════════════════════════════════════════════════════════════ */

#if MEDSIGHT_PHYSICAL_DISPENSER

static void step_once(bool forward)
{
    if (forward) {
        s_step_index = (uint8_t)((s_step_index + 1u) & 7u);
    } else {
        s_step_index = (uint8_t)((s_step_index + 7u) & 7u);
    }
    COIL_PORT->BSRR = s_halfstep[s_step_index];
}

static void coils_off(void)
{
    COIL_PORT->BSRR = COILS_OFF_BSRR;
}

/* Drain the ISR's break log to the console. Task context only. */
static void drain_break_log(uint8_t requested)
{
    while (s_break_rd != s_break_wr) {
        uint32_t r   = s_break_rd % BREAK_LOG_LEN;
        uint32_t dur = s_break_log[r].duration_ms;
        bool     cnt = s_break_log[r].counted;
        s_break_rd++;

        if (cnt) {
            /* A counted pill also reports how much chatter preceded it, so
             * the noise is still visible as a number without drowning the
             * signal. The first hardware run printed one line per chatter
             * pulse and produced 130 lines of "0 ms DISCARDED" around three
             * real counts - technically complete and practically unreadable,
             * which is its own kind of wrong for a diagnostic. */
            printf("  BREAK  dur=%4lu ms   count=%lu/%u%s   [%lu chatter since last]\r\n",
                   (unsigned long)dur,
                   (unsigned long)s_pill_count,
                   (unsigned)requested,
                   (dur > 80u) ? "   <-- long: possibly two pills" : "",
                   (unsigned long)s_chatter_run);
            s_chatter_run = 0;
        } else {
            s_chatter_run++;
            s_chatter_total++;
        }
    }
}

#endif /* MEDSIGHT_PHYSICAL_DISPENSER - stepping helpers */

/* ═══════════════════════════════════════════════════════════════════════════
 * The dispense loop
 * ═══════════════════════════════════════════════════════════════════════════ */

dispense_result_t dispenser_dispense(uint8_t count, uint8_t *out_dispensed)
{
    if (out_dispensed != NULL) {
        *out_dispensed = 0;
    }

#if !MEDSIGHT_PHYSICAL_DISPENSER
    (void)count;
    return DISPENSE_NOT_READY;
#else
    if (!s_ready) {
        return DISPENSE_NOT_READY;
    }

    uint8_t requested = (count > 0u) ? count : 1u;

    /* ── Re-learn the idle level for THIS dispense ────────────────────────
     *
     * The boot-time sample is informational only. The first hardware run
     * learned it backwards there and every clear beam then looked broken,
     * so the level the whole count depends on is established here, from a
     * reading that has held still for MS_IR_SETTLE_MS, immediately before it
     * is used. MS_IR_IDLE_FORCE overrides all of this once the level has
     * actually been measured on the bench. */
    {
#if (MS_IR_IDLE_FORCE >= 0)
        /* Forced from measurement, so no per-dispense settle wait either.
         * That was 600 ms on the front of every dose spent re-deciding a
         * value we already know. */
        s_idle_level = (MS_IR_IDLE_FORCE != 0) ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
        bool          stable   = false;
        GPIO_PinState relearnt = ir_settled_level(600u, &stable);

        if (!stable) {
            /* A line still moving before anything has been dispensed is not
             * a pill - it is chatter, ambient IR, or a wire. Reported, and
             * the last known level kept rather than believing the noise. */
            printf("dispenser: WARNING - IR line unsettled at start, "
                   "keeping idle=%s\r\n",
                   (s_idle_level == GPIO_PIN_SET) ? "HIGH" : "LOW");
        } else {
            if (relearnt != s_idle_level) {
                printf("dispenser: IR idle re-learnt %s -> %s\r\n",
                       (s_idle_level == GPIO_PIN_SET) ? "HIGH" : "LOW",
                       (relearnt == GPIO_PIN_SET)     ? "HIGH" : "LOW");
            }
            s_idle_level = relearnt;
        }
#endif
    }

    /* Start clean: the ISR's counter and its log both belong to this run.
     *
     * s_beam_broken starts FALSE unconditionally, and that is a fix rather
     * than a simplification. Arming the stall timer from a state that was
     * already true when the dispense began turned a wrong polarity guess
     * into a JAM 1.5 s later instead of a slow, visible failure. A stall is
     * a condition that BEGINS during a dispense; the ISR opens a break on
     * the next genuine edge. A beam that really is blocked for the whole run
     * still fails - as a timeout with nothing counted, reported as
     * DISPENSE_JAM anyway, just honestly and after a full actuator run. */
    HAL_NVIC_DisableIRQ(IR_EXTI_IRQn);
    s_pill_count  = 0;
    s_beam_broken = false;
    s_break_start = HAL_GetTick();
    s_break_rd    = s_break_wr;
    s_chatter_run   = 0;
    s_chatter_total = 0;
    HAL_NVIC_EnableIRQ(IR_EXTI_IRQn);

    printf("dispenser: idle=%s, line now %s\r\n",
           (s_idle_level == GPIO_PIN_SET) ? "HIGH" : "LOW",
           (HAL_GPIO_ReadPin(IR_PORT, IR_PIN) == GPIO_PIN_SET) ? "HIGH" : "LOW");

    const uint32_t started  = HAL_GetTick();
    uint32_t last_reported  = 0;
    uint32_t last_pill_tick = HAL_GetTick();
    dispense_result_t result;

    printf("dispenser: %u requested, gives up %lu ms after the last pill\r\n",
           (unsigned)requested, (unsigned long)MS_NO_PILL_TIMEOUT_MS);

    for (;;) {
        uint32_t counted = s_pill_count;

        /* Report and draw only on a real change - one pill, one bar step -
         * and restart the patience clock, because a pill arriving is proof
         * the mechanism is still working. */
        if (counted != last_reported) {
            last_reported  = counted;
            last_pill_tick = HAL_GetTick();
            if (s_progress_cb != NULL) {
                uint8_t shown = (counted > requested) ? requested
                                                      : (uint8_t)counted;
                s_progress_cb(shown, requested);
            }
        }

        if (counted >= requested) {
            result = DISPENSE_OK;
            break;
        }

        /* ── ONE bound, and it measures the right thing ───────────────────
         *
         * This used to be two: a stall bound on how long the beam could stay
         * broken, and an absolute cap on the whole dispense. Both are gone.
         *
         * The stall bound was actively dangerous. It was set at 1500 ms, and
         * the very first standalone run produced legitimate single-pill
         * breaks of 852 ms and 1210 ms - 290 ms of margin on a medication
         * device, where tripping it means refusing a dose that was being
         * delivered correctly. A slower pill would have been called a jam.
         *
         * The absolute cap was the wrong quantity too: it punished a slow
         * dose rather than a stopped one, so a large dose needed a large
         * timeout and a stuck mechanism still waited it out.
         *
         * What actually matters is how long it has been since the last pill
         * arrived. Every counted pill restarts the clock, so a working
         * mechanism runs as long as it needs to and a stopped one is caught
         * within one window - including the stalled-in-the-beam case, which
         * simply stops producing counts and is caught here like anything
         * else. One bound, measuring the thing that distinguishes working
         * from stopped. */
        if ((HAL_GetTick() - last_pill_tick) > MS_NO_PILL_TIMEOUT_MS) {
            /* Nothing at all means nothing was released; some but not enough
             * means the hopper ran dry mid-dose:
             * this is the refill signal, measured rather than assumed. */
            result = (counted == 0u) ? DISPENSE_JAM : DISPENSE_SHORT;
            break;
        }

        step_once(STEP_DISPENSE);
        osal_delay_ms(MS_STEP_PERIOD_MS);
        drain_break_log(requested);
    }

    /* On success, back off for a second: it settles the turntable, nudges any
     * pill balanced on the edge of the chute back into the hopper, and gives
     * the patient an unambiguous "finished" to watch. */
    if (result == DISPENSE_OK) {
        uint32_t reverse_until = HAL_GetTick() + 1000u;
        while ((int32_t)(reverse_until - HAL_GetTick()) > 0) {
            step_once(STEP_SETTLE);
            osal_delay_ms(MS_STEP_PERIOD_MS);
        }
    }

    /* EVERY exit path, including both failures. */
    coils_off();

    drain_break_log(requested);

    uint32_t final = s_pill_count;
    if (out_dispensed != NULL) {
        *out_dispensed = (final > 255u) ? 255u : (uint8_t)final;
    }

    printf("dispenser: %s - %lu of %u counted in %lu ms (%lu chatter suppressed)\r\n",
           dispenser_result_str(result),
           (unsigned long)final, (unsigned)requested,
           (unsigned long)(HAL_GetTick() - started),
           (unsigned long)s_chatter_total);

    return result;
#endif /* MEDSIGHT_PHYSICAL_DISPENSER */
}
