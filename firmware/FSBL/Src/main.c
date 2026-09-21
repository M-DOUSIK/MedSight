/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file: main.c
  * @brief: Main program body - FreeRTOS via OSAL
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "isp_api.h"
#include "imx335_E27_isp_param_conf.h"
#include "ui/anime_ui.h"
#include "ui/state_machine.h"
#include "sd_logger.h"
#include "ai_vision.h"
#include "ms_osal.h"    /* All RTOS access goes through OSAL */
#include "schedule_time_source.h" /* RTC-backed dose scheduling  */
#include "dispenser.h"  /* Stepper turntable + IR pill counter */
#include "buzzer.h"     /* The Program Plan's Alert Task           */
#include <string.h>     /* Strstr() in __assert_func() below */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* The earlier boot-hang bring-up instrumentation - the solid
 * LED checkpoints and the six per-step GREEN blinks through camera/LCD
 * bring-up - is now compile-time gated and OFF by default. It cost ~3.7 s of
 * blinking on every single boot (six blinks of 620 ms plus a 1 s checkpoint-4
 * burst) for a hang that was root-caused and
 * fixed long ago. The cleanup that followed stripped the vendored kernel
 * tree but missed main.c, so this closes it
 * out. Kept gated rather than deleted because it is genuinely the right first
 * tool if the board ever goes dark before UART comes up again - set this to 1
 * and it is all back, unchanged.
 * NOTE this does NOT touch Error_Handler()'s own blinking RED, which is a
 * permanent fault indicator, not instrumentation. */
#ifndef MS_BOOT_LED_CHECKPOINTS
#define MS_BOOT_LED_CHECKPOINTS   0
#endif

/* How often task_heartbeat_fn prints the idle/power figure.
 * 10 s is frequent enough to watch a change take effect on the bench and
 * infrequent enough that it never competes with the flow's own logging. */
#define MS_IDLE_REPORT_MS   10000u

/* ════════════════════════════════════════════════════════════════════════
 * MS_DISPLAY_WATCH - diagnostic for the cold-boot "draws, fades, greys out"
 * fault, added after four wrong diagnoses.
 *
 * Every guess so far has been about a *mechanism*. This measures the two
 * things that would tell those mechanisms apart, once a second, from the
 * lowest-priority task:
 *
 *   1. Is the framebuffer's CONTENT changing after the home screen is drawn?
 *      A sampled checksum, read back from RAM (not from cache), plus the
 *      DCMIPP frame counter. If the checksum moves, something is writing
 *      over the display buffer and the only question left is who. If
 *      NbMainFrames is also climbing, the answer is the camera: main() starts
 *      DCMIPP PIPE1 into BUFFER_ADDRESS in CONTINUOUS mode before the
 *      scheduler, and state_machine_init()'s camera_stop() is supposed to end
 *      that.
 *
 *   2. Is the LTDC still SCANNING, and still scanning the right address?
 *      GCR.LTDCEN, layer 1 CR.LEN and CFBAR, plus CDSR - the live
 *      HSYNC/VSYNC/HDE/VDE status bits. If the checksum is rock steady and
 *      CDSR goes quiet or LTDCEN drops, the memory is fine and the fault is
 *      the controller or the panel, which is a completely different search.
 *
 * Also prints the three panel control pins, since PQ6 (LCD_BL_CTRL) going
 * low is literally "the screen fades".
 *
 * This is a bench instrument, not a feature. Set it to 0 once the fault is
 * understood - and unlike MS_BOOT_SETTLE_MS, do not let it quietly become
 * load-bearing: it only reads registers and memory, it changes nothing.
 * ════════════════════════════════════════════════════════════════════════ */
#ifndef MS_DISPLAY_WATCH
#define MS_DISPLAY_WATCH 0   /* bench instrument; see Addendum 9 */
#endif
#define MS_DISPLAY_WATCH_MS  1000u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* Calculate division factor for a given source and destination dimension */
#define DIV_FACTOR(SRC, DST) (((uint32_t)((1024 * DST) / SRC)) > 1023 ? 1023 : ((uint32_t)((1024 * DST) / SRC)))

/* Calculate down scale ratio in unsigned 3.13 fixed-point format */
#define DOWNSCALE_RATIO(SRC, DST) (((uint32_t)(((float_t)(SRC) / (float_t)(DST)) * 8192) < 8192) ? 8192 : \
                                   ((((uint32_t)(((float_t)(SRC) / (float_t)(DST)) * 8192)) > 65535) ? 65535 : \
                                   ((uint32_t)(((float_t)(SRC) / (float_t)(DST)) * 8192))))
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
DCMIPP_HandleTypeDef hdcmipp;
LTDC_HandleTypeDef   hltdc;
ISP_HandleTypeDef    hcamera_isp;
SD_HandleTypeDef     hsd2;
DMA2D_HandleTypeDef  hdma2d;

/* USER CODE BEGIN PV */
static __IO uint32_t NbMainFrames = 0;
static IMX335_Object_t   IMX335Obj;
static int32_t isp_gain;
static int32_t isp_exposure;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MX_DCMIPP_Init(void);
static void LCD_Init(uint32_t Width, uint32_t Height);
static void MX_SDMMC2_SD_Init(void);
/* USER CODE BEGIN PFP */
static void IMX335_Probe(uint32_t Resolution, uint32_t PixelFormat);
static ISP_StatusTypeDef GetSensorInfoHelper(uint32_t Instance, ISP_SensorInfoTypeDef *SensorInfo);
static ISP_StatusTypeDef SetSensorGainHelper(uint32_t Instance, int32_t Gain);
static ISP_StatusTypeDef GetSensorGainHelper(uint32_t Instance, int32_t *Gain);
static ISP_StatusTypeDef SetSensorExposureHelper(uint32_t Instance, int32_t Exposure);
static ISP_StatusTypeDef GetSensorExposureHelper(uint32_t Instance, int32_t *Exposure);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

/* ════════════════════════════════════════════════════════════════════════════
 * TASK DEFINITIONS, and where their priorities come from
 *
 * ── The priority scheme, and where the numbers come from ──────────────────
 *
 * Earlier builds used 5/4/2/1 with the ordering argued informally ("camera
 * must never be starved", "touch should feel immediate"). The ordering was
 * right; the numbers had never been justified against each task's actual
 * period and deadline. They were then derived properly, rate-monotonic
 * style - shortest period gets the highest priority - and the result agrees
 * with what was already there, which is why nothing was renumbered. The one
 * change is that the previously-vacant level 3 is now occupied by the new
 * NPU task, deliberately placed BELOW the UI.
 *
 *  pri | task            | period      | deadline / why it sits there
 *  ----+-----------------+-------------+--------------------------------------
 *   5  | task_camera_isp | 1 ms        | Shortest period in the system and the
 *      |                 |             | only one tied to external hardware
 *      |                 |             | timing: ISP_BackgroundProcess() must
 *      |                 |             | consume each frame's statistics
 *      |                 |             | before the next VSYNC (~33 ms at
 *      |                 |             | 30 fps). Missing it degrades AE/AWB
 *      |                 |             | convergence, which is visible.
 *   4  | task_ui         | 10 ms       | Touch-to-response budget. Humans
 *      |                 |             | notice input lag past ~100 ms; a
 *      |                 |             | 10 ms poll plus the 4 FPS mascot
 *      |                 |             | frame gate keeps the whole UI an
 *      |                 |             | order of magnitude inside that.
 *      |                 |             | This matters, and there is
 *      |                 |             | measured evidence for it: when
 *      |                 |             | the kernel tick made these deadlines
 *      |                 |             | stretch 10x, the device immediately
 *      |                 |             | "felt slower than FreeRTOS".
 *   3  | task_ai         | on demand   | NO deadline. Hundreds of ms of solid
 *      |                 | (~0.5-4 s   | NPU/CPU work per request, a few
 *      |                 |  per burst) | times per session, in response to a
 *      |                 |             | button press the user already knows
 *      |                 |             | takes a moment. Placed below the UI
 *      |                 |             | on purpose: this is precisely the
 *      |                 |             | work that must be preemptible, so
 *      |                 |             | touch and the physical USER1 button
 *      |                 |             | keep responding while it runs. Above
 *      |                 |             | the logger because the user is
 *      |                 |             | actively waiting on its result and
 *      |                 |             | nobody is waiting on a log line.
 *   2  | task_logger     | event-driven| Tolerates seconds of latency by
 *      |                 |             | construction - the async queue
 *      |                 |             | exists so callers never wait on an
 *      |                 |             | SD write (10-50 ms each, occasionally
 *      |                 |             | much worse on a slow card).
 *   1  | task_heartbeat  | 500 ms      | No deadline at all; it is a liveness
 *      |                 |             | indicator. Deliberately lowest so
 *      |                 |             | that "the LED stopped blinking" means
 *      |                 |             | "something above me is starving the
 *      |                 |             | system", which is exactly the signal
 *      |                 |             | you want it to carry.
 *
 * ms_osal.h's convention is 1 = lowest; ms_osal.c inverts it onto
 * µT-Kernel's opposite scale (1 = highest) as OSAL_PRI_CEILING - priority,
 * so 5/4/3/2/1 become itskpri 11/12/13/14/15. Only the ordering is load
 * bearing; the absolute numbers leave headroom on both sides inside
 * mtk3_bsp2's CNF_MAX_TSKPRI of 32.
 *
 * Stack sizes are in 32-bit WORDS:
 *   2048 words = 8 KB  - UI (deep call chain through gui_draw/registration_ui)
 *                        and AI (the ST Edge AI runtime's own call depth; this
 *                        is the stack the pipeline already ran on when it was
 *                        part of the UI task, carried over unchanged rather
 *                        than re-tuned blind).
 *   1024 words = 4 KB  - adequate for FATFS local buffers (logger) and
 *                        the ISP middleware's background-processing locals.
 *   256 words  = 1 KB  - minimal, heartbeat has no deep callstack.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Task: Camera / ISP background processing ───────────────────────────── */
/* Priority 5 - highest. ISP_BackgroundProcess() must run every frame period.
 * Running it in a dedicated task prevents UI/logging jitter from delaying it. */
static void task_camera_isp_fn(void *arg)
{
    (void)arg;
    printf("task_camera_isp: started.\r\n");

    for (;;) {
        BSP_LED_Toggle(LED_GREEN);
        if (ISP_BackgroundProcess(&hcamera_isp) != ISP_OK) {
            BSP_LED_Toggle(LED_RED);
        }
        /* Yield for 1 tick (1ms) - ISP needs to run once per frame (~30fps),
         * so 1ms sleep is faster than needed but ensures other tasks get CPU.
         * The ISP library is self-throttling internally. */
        osal_delay_ms(1u);
    }
}

/* ── Task: UI - touch polling, state machine, mascot animation ─────────── */
/* Priority 4 - second-highest. Touch events polled every 10ms = 100Hz.
 * This gives ≤10ms touch-to-response latency, well within "immediate" feel.
 * The mascot animation self-throttles to 4 FPS inside anime_ui_update().   */
static void task_ui_fn(void *arg)
{
    (void)arg;
    printf("task_ui: started.\r\n");

    /* state_machine_init() configures touch IC, draws home screen, etc.
     * Called here (inside the task) because some BSP functions need the
     * RTOS scheduler already running to use HAL I2C blocking functions
     * correctly. */
    state_machine_init();
    SD_Log_Event_Async("System Boot - MedSight on uT-Kernel 3.0");

    /* Ai_vision_init() no longer runs here. It used to do the
     * one-time NPU + gallery bring-up on this task because inference itself
     * was a synchronous call from state_machine_update(); it now lives in
     * task_ai_fn (ai_vision.c), which is the task that owns the NPU. */

    for (;;) {
        state_machine_update();
        /* 10ms poll period: responsive touch without burning CPU.
         * anime_ui_update() inside state_machine_update() uses HAL_GetTick()
         * for its own 250ms frame gate - unaffected by this sleep. */
        osal_delay_ms(10u);
    }
}

/* ── Task: Logger - dequeue and write to SD ─────────────────────────────── */
/* Priority 2 - low. SD writes can take 10-50ms; other tasks preempt freely.
 * task_logger_fn is defined in sd_logger.c (the log queue is private there). */

/* ── Task: Heartbeat - liveness LED blink ───────────────────────────────── */
/* Priority 1 - lowest. If this stops blinking, the scheduler has stalled.
 * Blink period: 500ms on, 500ms off = 1Hz. */
#if MS_DISPLAY_WATCH
/* Sampled checksum of the framebuffer, read from RAM rather than from cache.
 *
 * Clean-then-invalidate, not plain invalidate: a plain invalidate would throw
 * away any dirty lines the UI task had not flushed yet and corrupt the very
 * picture we are trying to observe. Cleaning first pushes our own writes out,
 * so what we then read back is exactly the bytes LTDC's DMA sees. */
static uint32_t framebuffer_sample_checksum(void)
{
    uint32_t sum = 0u;
    /* 32 rows spread down the screen, one 32-byte cache line each: enough to
     * catch any large-area overwrite, cheap enough to run once a second on
     * the lowest-priority task. */
    for (uint32_t i = 0u; i < 32u; i++) {
        uint32_t row  = (i * FRAME_HEIGHT) / 32u;
        uint32_t addr = (BUFFER_ADDRESS + row * FRAME_WIDTH * 2u) & ~31u;
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)addr, 32);
        const volatile uint32_t *p = (const volatile uint32_t *)addr;
        for (uint32_t w = 0u; w < 8u; w++) {
            sum = (sum * 31u) + p[w];
        }
    }
    return sum;
}

/* Read one pixel straight out of RAM, bypassing the cache the same way the
 * checksum does. Used to answer a question the checksum cannot: the buffer is
 * *stable*, but is it stable holding the home screen, or stable holding grey? */
static uint16_t framebuffer_peek(uint32_t x, uint32_t y)
{
    uint32_t addr    = BUFFER_ADDRESS + ((y * FRAME_WIDTH) + x) * 2u;
    uint32_t aligned = addr & ~31u;
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)aligned, 32);
    return *(const volatile uint16_t *)addr;
}

/* Is the LTDC actually scanning the panel?
 *
 * CDSR carries the live HSYNC/VSYNC/HDE/VDE status. A single sample cannot
 * distinguish "scanning, and I happened to catch the active area" from "not
 * scanning at all" - which is exactly the ambiguity the first watch build
 * left behind, because it read 0 on almost every sample.
 *
 * So sample it hard instead: a few thousand back-to-back reads spanning well
 * over a line period. If the controller is scanning, those bits MUST move.
 * Zero transitions across the whole burst means the pixel clock is not
 * running, whatever the enable bits claim. */
static uint32_t ltdc_scan_activity(uint32_t *seen_mask)
{
    uint32_t prev        = LTDC->CDSR & 0xFu;
    uint32_t transitions = 0u;
    uint32_t mask        = (1u << prev);

    /* Bound by TIME, not by iteration count. The first version of this burst
     * ran a fixed 20000 reads, which at -O0 is only a few milliseconds - less
     * than one 16 ms frame. That made the set of observed CDSR values look
     * like it changed between samples ("seen=405" then "seen=8CAF") when in
     * fact the burst was simply aliasing against the frame rate and sometimes
     * missing the vertical events entirely. 40 ms guarantees at least two
     * complete frames at any plausible refresh rate, so the value is stable
     * and actually means something. */
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 40u) {
        uint32_t cur = LTDC->CDSR & 0xFu;
        if (cur != prev) {
            transitions++;
            prev = cur;
        }
        mask |= (1u << cur);
    }
    if (seen_mask != NULL) {
        *seen_mask = mask;
    }
    return transitions;
}

/* ════════════════════════════════════════════════════════════════════════
 * MS_DISPLAY_RECOVER - the escalating recovery ladder
 *
 * Four rounds of register reading have now said the same thing: the picture
 * is in RAM, the pixel format is right, the layer is enabled and pointed at
 * it, the pixel clock is 25 MHz, the sync outputs are toggling, and the
 * panel's power and backlight pins are high. By every measurement the display
 * path is working. The screen is still grey.
 *
 * When observation keeps saying "fine" and reality keeps saying "no", the way
 * forward is to perturb it: apply progressively bigger hammers, spaced far
 * enough apart to watch, and announce each one over UART. Whichever rung
 * brings the picture back identifies the broken layer, and does it in ONE
 * flash cycle instead of one per theory.
 *
 *   1  shadow reload      - LTDC shadow registers never latched
 *   2  immediate reload   - same, via the other reload path
 *   3  LTDC off/on        - controller-level state
 *   4  panel power cycle  - the panel latched a bad state at power-on
 *   5  backlight cycle    - backlight driver enable
 *   6  full LCD re-init   - anything in the LTDC configuration itself
 *   7  relaxed blanking    - the panel cannot lock to the current timings
 *
 * The ordering is deliberate: cheapest and least destructive first, so the
 * answer is as specific as possible. If rung 4 fixes it, the LTDC was never
 * the problem and the panel needs its power sequenced differently at boot. If
 * only rung 6 fixes it, something in LCD_Init() is not surviving a cold start.
 * If NOTHING fixes it, the fault is below software entirely.
 *
 * Set MS_DISPLAY_RECOVER to 0 for normal builds. This is a bench instrument.
 * ════════════════════════════════════════════════════════════════════════ */
#ifndef MS_DISPLAY_RECOVER
#define MS_DISPLAY_RECOVER 0   /* bench instrument; see Addendum 9 */
#endif
/* Let the fault establish itself first - it needs a few seconds to grey out. */
#define MS_RECOVER_START_MS   12000u
/* Long enough to see the screen change and note which rung did it. */
#define MS_RECOVER_STEP_MS     6000u

#if MS_DISPLAY_RECOVER
static void display_recover_poll(void)
{
    static uint32_t next_at = MS_RECOVER_START_MS;
    static uint32_t step    = 0u;

    uint32_t now = HAL_GetTick();
    if ((step > 7u) || (now < next_at)) {
        return;
    }
    next_at = now + MS_RECOVER_STEP_MS;
    step++;

    switch (step) {
    case 1u:
        printf("recover 1: HAL_LTDC_SetAddress - force a shadow reload\r\n");
        HAL_LTDC_SetAddress(&hltdc, BUFFER_ADDRESS, LTDC_LAYER_1);
        break;

    case 2u:
        printf("recover 2: immediate reload (SRCR.IMR)\r\n");
        __HAL_LTDC_RELOAD_CONFIG(&hltdc);
        break;

    case 3u:
        printf("recover 3: LTDC disable -> 100ms -> enable\r\n");
        __HAL_LTDC_DISABLE(&hltdc);
        HAL_Delay(100u);
        __HAL_LTDC_ENABLE(&hltdc);
        break;

    case 4u:
        printf("recover 4: panel power cycle - LCD_ONOFF (PQ3) low 200ms\r\n");
        HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_3, GPIO_PIN_RESET);
        HAL_Delay(200u);
        HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_3, GPIO_PIN_SET);
        break;

    case 5u:
        printf("recover 5: backlight cycle - LCD_BL_CTRL (PQ6) low 200ms\r\n");
        HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_6, GPIO_PIN_RESET);
        HAL_Delay(200u);
        HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_6, GPIO_PIN_SET);
        break;

    case 6u:
        printf("recover 6: full LCD_Init() re-run\r\n");
        LCD_Init(FRAME_WIDTH, FRAME_HEIGHT);
        HAL_LTDC_SetAddress(&hltdc, BUFFER_ADDRESS, LTDC_LAYER_1);
        break;

    case 7u:
        /* Conventional 800x480 blanking, replacing the 812x492 total the
         * board currently runs. Written straight to the registers rather than
         * through HAL_LTDC_Init() so that this is the ONLY thing that changes
         * from rung 6 - same clock, same layer, same buffer, same everything
         * else. If the picture appears here and nowhere else, the panel was
         * never able to lock to 12 pixels of horizontal blanking from cold.
         *
         *   HSW  4   HBP  46  active 800  HFP 206   -> total 1056
         *   VSW  4   VBP  23  active 480  VFP  18   -> total  525
         *
         * At the existing 25 MHz pixel clock that is 45 Hz, which is fine.
         * All four registers hold (value - 1), and the back-porch and active
         * fields are ACCUMULATED, not per-region. */
        printf("recover 7: relaxed blanking - 1056x525 total "
               "(was 812x492)\r\n");
        LTDC->SSCR = (3u << 16) | 3u;           /* HSW 4,  VSW 4            */
        LTDC->BPCR = (49u << 16) | 26u;         /* +HBP 46, +VBP 23         */
        LTDC->AWCR = (849u << 16) | 506u;       /* +800 active, +480 active */
        LTDC->TWCR = (1055u << 16) | 524u;      /* total 1056 x 525         */
        __HAL_LTDC_RELOAD_CONFIG(&hltdc);
        printf("recover: ladder complete - if the screen is still grey, "
               "nothing in software fixed it\r\n");
        break;

    default:
        break;
    }
}
#endif /* MS_DISPLAY_RECOVER */

static void display_watch_poll(void)
{
    static uint32_t last_tick   = 0u;
    static uint32_t last_sum    = 0u;
    static uint32_t last_frames = 0u;
    static bool     primed      = false;

    uint32_t now = HAL_GetTick();
    if (primed && ((now - last_tick) < MS_DISPLAY_WATCH_MS)) {
        return;
    }
    last_tick = now;

    uint32_t sum    = framebuffer_sample_checksum();
    uint32_t frames = NbMainFrames;

    uint32_t seen  = 0u;
    uint32_t moves = ltdc_scan_activity(&seen);

    printf("watch %lums: fb=%08lX %s | px title=%04X bg=%04X reg=%04X "
           "disp=%04X dlg=%04X | scan moves=%lu seen=%lX | frames=%lu (+%lu)\r\n",
           (unsigned long)now,
           (unsigned long)sum,
           (primed && (sum != last_sum)) ? "CHANGED" : "same",
           framebuffer_peek(400u,  20u),   /* teal title strip, expect 0566 */
           framebuffer_peek(400u, 200u),   /* page background               */
           framebuffer_peek(120u, 300u),   /* REGISTER button               */
           framebuffer_peek(600u, 300u),   /* DISPENSE button               */
           framebuffer_peek(400u, 430u),   /* dialog box                    */
           (unsigned long)moves,
           (unsigned long)seen,
           (unsigned long)frames,
           (unsigned long)(frames - last_frames));

    printf("            LTDCEN=%lu LEN=%lu CFBAR=%08lX PF=%lu CACR=%lX "
           "BFCR=%lX | pixclk=%luHz | SSCR=%08lX BPCR=%08lX AWCR=%08lX "
           "TWCR=%08lX GCR=%08lX | ONOFF=%lu BL=%lu DE=%lu\r\n",
           (unsigned long)(LTDC->GCR & LTDC_GCR_LTDCEN),
           (unsigned long)(LTDC_Layer1->CR & LTDC_LxCR_LEN),
           (unsigned long)LTDC_Layer1->CFBAR,
           (unsigned long)LTDC_Layer1->PFCR,
           (unsigned long)LTDC_Layer1->CACR,
           (unsigned long)LTDC_Layer1->BFCR,
           (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_LTDC),
           (unsigned long)LTDC->SSCR,
           (unsigned long)LTDC->BPCR,
           (unsigned long)LTDC->AWCR,
           (unsigned long)LTDC->TWCR,
           (unsigned long)LTDC->GCR,
           (unsigned long)HAL_GPIO_ReadPin(GPIOQ, GPIO_PIN_3),
           (unsigned long)HAL_GPIO_ReadPin(GPIOQ, GPIO_PIN_6),
           (unsigned long)HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_13));

    /* Layer geometry. A layer can be enabled, correctly formatted and
     * scanning, and still put nothing on the glass if its window is empty or
     * its line count is zero - none of which the registers above would show.
     *
     * BCCR is worth having for a different reason: LCD_Init() sets the LTDC
     * background colour to bright GREEN (Backcolor.Green = 0xFF). So if the
     * layer were not being composited at all, the panel would be green, not
     * grey. "Grey" already tells us this is not a compositing problem - but
     * print it so that stays a measurement rather than an inference. */
    printf("            WHPCR=%08lX WVPCR=%08lX CFBLR=%08lX CFBLNR=%08lX "
           "BCCR=%06lX L2CR=%lX SCR=%lX\r\n",
           (unsigned long)LTDC_Layer1->WHPCR,
           (unsigned long)LTDC_Layer1->WVPCR,
           (unsigned long)LTDC_Layer1->CFBLR,
           (unsigned long)LTDC_Layer1->CFBLNR,
           (unsigned long)(LTDC->BCCR & 0xFFFFFFu),
           (unsigned long)(LTDC_Layer2->CR & LTDC_LxCR_LEN),
           /* SCR bit 2 is SLEEPDEEP. If it is set, WFI enters Stop mode
            * rather than Sleep, stopping the bus clocks the LTDC needs.
            * Nothing in the tree sets it; measure it, do not assume. */
           (unsigned long)SCB->SCR);

    last_sum    = sum;
    last_frames = frames;
    primed      = true;
}
#endif /* MS_DISPLAY_WATCH */

/* ════════════════════════════════════════════════════════════════════════
 * ms_configure_sleep_clocks() - what has to keep running while the CPU sleeps
 *
 * THE BUG THIS FIXES. Adding a WFI to the kernel idle path broke the
 * display on this board: from a cold boot the home screen drew,
 * glitched, and greyed out within a couple of seconds, while UART, touch, the
 * SD card, the NPU and every task carried on perfectly.
 *
 * WFI on the STM32N6 enters CSleep, which stops the CPU - and, for every
 * peripheral, bus and memory whose LPEN bit is clear, stops its clock too.
 * The framebuffer lives at 0x34200000, in AXISRAM3-6. So each time the idle
 * task slept, the LTDC's DMA lost either its own clock, the AXI bus matrix
 * clock, or the RAM it was reading from. It kept scanning and kept driving
 * sync - every register we measured said the display was healthy - but it was
 * fetching nothing, so the panel starved and went grey. With the CPU idle
 * ~85% of the time, that is almost continuously.
 *
 * Why it looked like a hardware fault for six rounds:
 *   - The CPU's own reads of the framebuffer were always correct, because the
 *     CPU only reads when it is awake. The picture really was in RAM.
 *   - Every LTDC register read correct, for the same reason.
 *   - It vanished under the debugger: a halted core never executes WFI.
 *   - It vanished on a warm re-run, because those runs were being driven from
 *     the debugger too.
 * The measurement that finally isolated it was removing the WFI and changing
 * nothing else.
 *
 * WHAT THIS SETS. Only what a bus master genuinely needs while the CPU is
 * asleep - every extra bit here is power the device burns for nothing:
 *
 *   BUSLPENR   ACLKN, ACLKNC   the AXI bus matrix itself. Without this no
 *                              master reaches memory at all during sleep.
 *   MEMLPENR   AXISRAM3-6      the framebuffer (0x34200000) and the NPU
 *                              activation pools that share it.
 *   APB5LPENR  LTDC            the display controller.
 *              DCMIPP, CSI     the camera pipeline: it DMAs into the same
 *                              buffer during preview, while the UI task is
 *                              blocked and the CPU is therefore asleep.
 *   AHB5LPENR  DMA2D           the mascot blitter.
 *              SDMMC2          audit-log writes complete while the logger
 *                              task waits and the CPU sleeps.
 *              NPU             inference runs for hundreds of ms with every
 *                              task blocked on the AI event flag.
 *
 * Each of those is a master that moves data with no CPU involvement. Any one
 * of them left gated would produce the same class of fault as the display
 * did - silent, intermittent, and impossible to see in a register dump.
 *
 * DO NOT reduce this set without re-testing the flow that uses it, from a
 * COLD boot. That is the only condition under which the original fault ever
 * appeared.
 *
 * A CORRECTION. This note used to say that
 * GUI_BUFFER_ADDRESS was safe because "the NPU's activation scratch only
 * ever overlaps BUFFER_ADDRESS". That was wrong, and the hardware said so
 * on the first capture: the
 * canary found both ends of the 750 KB second framebuffer clobbered with
 * float bit patterns. The second framebuffer was reverted and is unused.
 *
 * The actual extent was established by extracting every address
 * literal from the generated networks (the technical reference): the two
 * models' activations occupy ONE contiguous block, 0x34200000-0x34387FFF,
 * 1,605,632 bytes. GUI_BUFFER_ADDRESS (0x342BB800-0x34376FFF) is entirely
 * inside it, which is the arithmetic behind the canary failure.
 *
 * The LPEN table above is unaffected either way: every one of those
 * addresses is inside AXISRAM3-6, which these bits already ungate. So is
 * the AI_ARENA at 0x34388000. NO NEW LPEN BIT WAS NEEDED HERE
 *  EITHER, and that was checked rather than assumed.
 *
 * The rule for whoever comes next is unchanged and is the important part:
 * if future work places any buffer OUTSIDE AXISRAM3-6, or adds a DMA
 * path that writes into one (a second LTDC layer, a DMA2D destination
 * elsewhere, a peripheral's DMA buffer in.bss), re-derive this table from
 * scratch and re-test from a COLD BOOT. Do not assume coverage.
 *
 * Worth knowing while you are here: AXISRAM5 and AXISRAM6 are not powered
 * at all until npu_init.c's SystemInit_POST() runs, on the AI task, after
 * the scheduler starts. stm32n6xx_hal_msp.c brings up only AXISRAM3 and
 * AXISRAM4. the technical reference has the consequences.
 * ════════════════════════════════════════════════════════════════════════ */
static void ms_configure_sleep_clocks(void)
{
    RCC->BUSLPENR  |= RCC_BUSLPENR_ACLKNLPEN
                    | RCC_BUSLPENR_ACLKNCLPEN;

    RCC->MEMLPENR  |= RCC_MEMLPENR_AXISRAM3LPEN
                    | RCC_MEMLPENR_AXISRAM4LPEN
                    | RCC_MEMLPENR_AXISRAM5LPEN
                    | RCC_MEMLPENR_AXISRAM6LPEN;

    RCC->APB5LPENR |= RCC_APB5LPENR_LTDCLPEN
                    | RCC_APB5LPENR_DCMIPPLPEN
                    | RCC_APB5LPENR_CSILPEN;

    RCC->AHB5LPENR |= RCC_AHB5LPENR_DMA2DLPEN
                    | RCC_AHB5LPENR_SDMMC2LPEN
                    | RCC_AHB5LPENR_NPULPEN
#if MEDSIGHT_ACTION_RECOGNITION
    /* ── The first new DMA destination in the project.
     *
     * Action recognition points the DCMIPP at external PSRAM (XSPI1) at
     * 0x90400000 so the camera can run while the UI keeps drawing the confirm
     * screen - see Inc/ai/intake_camera.h. That destination is outside
     * AXISRAM3-6, so the standing rule above applies in full and this is the
     * "in the same change" it demands.
     *
     * TWO bits, not one, and the second is the one easy to miss:
     *   XSPI1LPEN   the PSRAM controller itself.
     *   XSPIMLPEN   the XSPI MANAGER. Every memory-mapped access to either
     *               external memory is routed through it, so leaving it gated
     *               stalls the DCMIPP's writes just as completely as gating
     *               the controller would - and with the identical silent,
     *               intermittent, register-invisible signature.
     *
     * The failure this prevents would look like: a confirm screen that draws
     * perfectly, a camera that reports healthy, and an intake verdict that is
     * UNCERTAIN or plain wrong on a device that is idle ~88% of the time and
     * therefore asleep for most of the observation window. In other words,
     * Addendum 9 again, one peripheral over.
     *
     * MUST be verified from a COLD BOOT. A warm re-flash inherits state and
     * will hide it, exactly as it hid the display fault for six rounds. */
                    | RCC_AHB5LPENR_XSPI1LPEN
                    | RCC_AHB5LPENR_XSPIMLPEN
#endif
                    ;

    /* Read back so the writes have landed before the first WFI can happen. */
    (void)RCC->AHB5LPENR;

    printf("sleep clocks: BUSLPENR=%08lX MEMLPENR=%08lX "
           "APB5LPENR=%08lX AHB5LPENR=%08lX\r\n",
           (unsigned long)RCC->BUSLPENR,  (unsigned long)RCC->MEMLPENR,
           (unsigned long)RCC->APB5LPENR, (unsigned long)RCC->AHB5LPENR);
}

static void task_heartbeat_fn(void *arg)
{
    (void)arg;
    printf("task_heartbeat: started.\r\n");

    /* This task also carries the power-saving measurement, since
     * it is the one task in the system with a fixed, known period and no
     * deadline of its own. Every MS_IDLE_REPORT_MS it prints what proportion
     * of wall-clock time the Cortex-M55 spent inside WFI in µT-Kernel's idle
     * path - a real number for the "power saving" criterion,
     * rather than the claim "we call WFI".
     *
     * Deliberately printed from TASK context on a 10 s period, never from
     * the idle hook itself, because of
     * what putting a printf on a per-tick path costs on this hardware. */
    uint32_t last_report_tick   = HAL_GetTick();
    uint64_t last_idle_cycles   = 0u;
    uint32_t last_idle_entries  = 0u;

    for (;;) {
        BSP_LED_Toggle(LED_RED);
        osal_delay_ms(500u);

#if MS_DISPLAY_WATCH
        display_watch_poll();
#endif
#if MS_DISPLAY_RECOVER
        display_recover_poll();
#endif

        uint32_t now = HAL_GetTick();
        if ((now - last_report_tick) >= MS_IDLE_REPORT_MS) {
            uint64_t idle_cycles = 0u;
            uint32_t idle_entries = 0u;
            bool     cyccnt_ok = ms_osal_idle_stats(&idle_cycles, &idle_entries);

            uint32_t elapsed_ms = now - last_report_tick;
            uint64_t d_cycles   = idle_cycles - last_idle_cycles;
            uint32_t d_entries  = idle_entries - last_idle_entries;

            if (cyccnt_ok && elapsed_ms > 0u) {
                /* Busy cycles available in the window = CPU clock * seconds.
                 * Integer maths only - this toolchain's nano.specs printf has
                 * no %f, and a per-mille figure is precise enough to quote. */
                uint64_t window_cycles =
                    ((uint64_t)SystemCoreClock / 1000u) * (uint64_t)elapsed_ms;
                uint32_t permille = (window_cycles > 0u)
                    ? (uint32_t)((d_cycles * 1000u) / window_cycles) : 0u;
                if (permille > 1000u) permille = 1000u;
                printf("power: idle %u.%u%% of last %ums (%u WFI entries)\r\n",
                       (unsigned)(permille / 10u), (unsigned)(permille % 10u),
                       (unsigned)elapsed_ms, (unsigned)d_entries);
            } else {
                printf("power: %u WFI entries in last %ums "
                       "(cycle counter unavailable)\r\n",
                       (unsigned)d_entries, (unsigned)elapsed_ms);
            }

            last_report_tick  = now;
            last_idle_cycles  = idle_cycles;
            last_idle_entries = idle_entries;
        }
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int (never returns - ends with osal_scheduler_start())
  */
int main(void)
{
    /* USER CODE BEGIN 1 */
    ISP_AppliHelpersTypeDef appliHelpers = {0};
    /* USER CODE END 1 */

    /* Enable the CPU Cache */
    SCB_EnableICache();
    SCB_EnableDCache();

    /* MCU Configuration */
    HAL_Init();

    /* Configure the system clock */
    SystemClock_Config();

    /* Configure the peripherals common clocks */
    PeriphCommonClock_Config();

    /* USER CODE BEGIN SysInit */

    /* ════════════════════════════════════════════════════════════════════
     * Declare EVERY I/O supply domain valid before touching
     * a single GPIO.
     *
     * The STM32N6's high-numbered GPIO banks are split across separately
     * supplied I/O domains (VDDIO2..VDDIO5), and a bank whose domain has not
     * been declared valid does not drive its pins. This project hit that
     * early on, when a missing
     * HAL_PWREx_EnableVddIO5() left the SDMMC2 pins unpowered and the driver
     * hung forever waiting on hardware that could not answer.
     *
     * The same trap was still live for the display, and much harder to see.
     * Mapping the domains from the ST BSP's own call sites:
     *
     *     VDDIO2 -> GPIOO      (BSP_LED_Init)
     *     VDDIO3 -> GPION      (SD card-detect)
     *     VDDIO4 -> GPIOH      (I2C1, camera)
     *     VDDIO5 -> GPIOC/E    (SDMMC2)
     *
     * LTDC's MspInit configures PE11 (LCD_VSYNC), PE1 (touch NRST) and
     * PH3/PH4/PH6 (colour bits B4/R4/B5) - i.e. pins in the VDDIO5 and
     * VDDIO4 domains - and enables NEITHER. Nothing enabled VDDIO5 until the
     * SD card was initialised, long after the panel had been configured and
     * the LTDC had started scanning out.
     *
     * Why this hid for so long: the PWR SVMCR "supply valid" bits live in the
     * always-on power domain and are NOT cleared by a system reset - only by
     * actually removing power. So once any run had enabled them, every
     * subsequent flash-and-run inherited valid domains and the display came up
     * fine. The bug only appears on a true cold boot, which is exactly the
     * situation the display fails in: dark, ghosting, fading out, while UART
     * (USART1, on the main VDD domain) carries on perfectly.
     *
     * Enabling all four here, once, before any peripheral init, removes the
     * ordering dependency entirely. The bits are idempotent, every one of
     * these rails is populated and powered on the STM32N6570-DK, and the BSP's
     * own later calls become harmless no-ops.
     * ════════════════════════════════════════════════════════════════════ */
    HAL_PWREx_EnableVddIO2();
    HAL_PWREx_EnableVddIO3();
    HAL_PWREx_EnableVddIO4();
    HAL_PWREx_EnableVddIO5();

    BSP_LED_Init(LED_GREEN);
    BSP_LED_Init(LED_RED);

    /* Bring-up checkpoint 1 - see MS_BOOT_LED_CHECKPOINTS above.
     * Cache enable, HAL_Init(), SystemClock_Config(),
     * PeriphCommonClock_Config() and LED GPIO init all completed.
     * Solid GREEN = passed checkpoint 1. */
#if MS_BOOT_LED_CHECKPOINTS
    BSP_LED_On(LED_GREEN);
#endif

    /* UART log */
#if USE_COM_LOG
    COM_InitTypeDef COM_Init;
    COM_Init.BaudRate   = 115200;
    COM_Init.WordLength = COM_WORDLENGTH_8B;
    COM_Init.StopBits   = COM_STOPBITS_1;
    COM_Init.Parity     = COM_PARITY_NONE;
    COM_Init.HwFlowCtl  = COM_HWCONTROL_NONE;
    BSP_COM_Init(COM1, &COM_Init);
    if (BSP_COM_SelectLogPort(COM1) != BSP_ERROR_NONE) {
        Error_Handler();
    }
#endif
    /* USER CODE END SysInit */

    /* Checkpoint 2: UART/COM init (BSP_COM_Init, BSP_COM_SelectLogPort)
     * completed without Error_Handler firing. Solid GREEN+RED. */
#if MS_BOOT_LED_CHECKPOINTS
    BSP_LED_On(LED_RED);
#endif

    /* Per-step blink through the six camera/LCD bring-up calls below. RED
     * stays solid throughout (it only starts BLINKING if Error_Handler()
     * fires); count the GREEN blinks to see exactly how far boot gets.
     *
     * NOTE: this is now purely visual. The *timing* those blinks used to
     * provide has been separated out into _MS_SETTLE() below - see the
     * MS_BOOT_SETTLE_MS comment. Turning the blinks on or off no longer
     * changes whether the display comes up. */
#if MS_BOOT_LED_CHECKPOINTS
#define _MS_BLINK() do { BSP_LED_On(LED_GREEN); HAL_Delay(120); \
                          BSP_LED_Off(LED_GREEN); HAL_Delay(500); } while (0)
#else
#define _MS_BLINK() do { } while (0)
#endif

/* ════════════════════════════════════════════════════════════════════════
 * Boot settling - kept as a knob, set to zero, and here is why
 *
 * Earlier builds had ~4.7 s of accidental delay through the camera/LCD
 * bring-up below: six _MS_BLINK() calls at 620 ms plus a 1 s LED burst before
 * the scheduler, all of it bring-up instrumentation. Once the blinks were
 * gated off as dead weight, the display started failing from a
 * cold boot, and the obvious conclusion was that the delay had been
 * load-bearing. So it was reinstated here as an explicit, measured,
 * bisectable value.
 *
 * That conclusion was WRONG, and this is the record of it. The fault was
 * reproduced on hardware with 5.6 s of settling applied and it still failed.
 * The real cause was the WFI added to the kernel idle path gating the clocks
 * the LTDC needs - see ms_configure_sleep_clocks() below.
 * No part of the bring-up sequence needs any
 * settling time at all, which is why both values are now zero: boot goes
 * straight through in a few hundred milliseconds.
 *
 * The knobs stay because they cost nothing and because a future bring-up on
 * different hardware may genuinely want them. But do not reach for them to
 * explain a fault. The lesson here is the opposite one: when
 * something that worked stops working, suspect what you changed, and measure
 * before theorising. Four hardware-shaped theories were wrong here before a
 * single measurement settled it.
 * ════════════════════════════════════════════════════════════════════════ */
#ifndef MS_BOOT_SETTLE_MS
#define MS_BOOT_SETTLE_MS          0u     /* none needed - see the note above */
#endif
#ifndef MS_BOOT_PRESCHED_SETTLE_MS
#define MS_BOOT_PRESCHED_SETTLE_MS 0u     /* none needed - see the note above */
#endif

/* Boot progress over UART, with timings. Cheap, one line per step, and it is
 * what makes MS_BOOT_SETTLE_MS tunable instead of guessed. On by default
 * while the cold-boot display fault is being confirmed fixed. */
#ifndef MS_BOOT_TRACE
#define MS_BOOT_TRACE 0
#endif

#if MS_BOOT_TRACE
#define _MS_SETTLE(step)                                                      \
    do {                                                                      \
        uint32_t _t_done = HAL_GetTick();                                     \
        printf("boot: %-22s done at %lums (step took %lums), settling %ums\r\n", \
               (step), (unsigned long)_t_done,                                \
               (unsigned long)(_t_done - _ms_boot_mark), MS_BOOT_SETTLE_MS);  \
        _MS_BLINK();                                                          \
        HAL_Delay(MS_BOOT_SETTLE_MS);                                         \
        _ms_boot_mark = HAL_GetTick();                                        \
    } while (0)
#else
#define _MS_SETTLE(step)                                                      \
    do { _MS_BLINK(); HAL_Delay(MS_BOOT_SETTLE_MS); } while (0)
#endif

    /* Timing mark used by _MS_SETTLE()'s trace. */
    uint32_t _ms_boot_mark = HAL_GetTick();
    (void)_ms_boot_mark;

#if MS_BOOT_TRACE
    printf("boot: starting camera/LCD bring-up at %lums "
           "(settle %ums/step, %ums pre-scheduler)\r\n",
           (unsigned long)_ms_boot_mark, MS_BOOT_SETTLE_MS,
           MS_BOOT_PRESCHED_SETTLE_MS);
#endif

    /* Let the board's supplies settle before the first peripheral touches a
     * pin. On a cold boot everything below runs within milliseconds of VDD
     * coming up; on a warm re-run the rails have been stable for minutes,
     * which is a large part of why this fault only ever appeared cold. */
    HAL_Delay(MS_BOOT_SETTLE_MS);
    _ms_boot_mark = HAL_GetTick();

    /* Initialize all configured peripherals */
    MX_DCMIPP_Init();
    _MS_SETTLE("MX_DCMIPP_Init");

    /* Initialize the IMX335 Sensor */
    IMX335_Probe(IMX335_R2592_1944, IMX335_RAW_RGGB10);
    _MS_SETTLE("IMX335_Probe");

    /* USER CODE BEGIN 2 */
    LCD_Init(FRAME_WIDTH, FRAME_HEIGHT);
    _MS_SETTLE("LCD_Init");

    /* Fill init struct with Camera driver helpers */
    appliHelpers.GetSensorInfo     = GetSensorInfoHelper;
    appliHelpers.SetSensorGain     = SetSensorGainHelper;
    appliHelpers.GetSensorGain     = GetSensorGainHelper;
    appliHelpers.SetSensorExposure = SetSensorExposureHelper;
    appliHelpers.GetSensorExposure = GetSensorExposureHelper;

    /* Initialize the Image Signal Processing middleware */
    if (ISP_Init(&hcamera_isp, &hdcmipp, 0, &appliHelpers, ISP_IQParamCacheInit[0]) != ISP_OK) {
        Error_Handler();
    }
    _MS_SETTLE("ISP_Init");

    if (HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0,
                                   BUFFER_ADDRESS, DCMIPP_MODE_CONTINUOUS) != HAL_OK) {
        Error_Handler();
    }
    _MS_SETTLE("DCMIPP_PIPE_Start");

    /* Start the Image Signal Processing */
    if (ISP_Start(&hcamera_isp) != ISP_OK) {
        Error_Handler();
    }
    _MS_SETTLE("ISP_Start");

    /* Initialise DMA2D for mascot alpha-blending */
    hdma2d.Instance          = DMA2D;
    hdma2d.Init.Mode         = DMA2D_M2M_BLEND;
    hdma2d.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
    hdma2d.Init.OutputOffset = 0;
    if (HAL_DMA2D_Init(&hdma2d) != HAL_OK) {
        Error_Handler();
    }

    /* Register the DMA2D completion callback for anime_ui */
    extern void HAL_DMA2D_XferCpltCallback(DMA2D_HandleTypeDef *hdma2d);
    hdma2d.XferCpltCallback = HAL_DMA2D_XferCpltCallback;
    /* DMA2D priority must be NUMERICALLY >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5)
     * to sit below FreeRTOS's BASEPRI mask. Priority 6 = safe, never calls FreeRTOS API. */
    HAL_NVIC_SetPriority(DMA2D_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA2D_IRQn);

    /* Initialise the Lumio mascot animation layer. Was
     * defaulted to GUI_BUFFER_ADDRESS here; reverted alongside LCD_Init()'s
     * revert above (see that comment) - this runs too early to safely be
     * the first touch of unverified memory. state_machine_init() switches
     * it to GUI_BUFFER_ADDRESS once that address has been confirmed safe. */
    anime_ui_init(&hdma2d, BUFFER_ADDRESS, FRAME_WIDTH);

    /* SDMMC hardware init (mounting moved to task_logger_fn) */
    MX_SDMMC2_SD_Init();

    /* The RTC behind the dose schedule.
     *
     * Here, in main(), and not from a task, for two reasons. The backup
     * domain has to be unlocked and the low-speed oscillator started before
     * anything asks what time it is, and the LSE start-up is a blocking wait
     * of up to a second that has no business happening inside a task with a
     * deadline. It is also survivable: if no low-speed oscillator will run,
     * this reports it and returns false, and the device keeps working as a
     * dispense-on-demand machine with reminders switched off. That is why
     * the return value is deliberately not fatal. */
    (void)time_source_init();

    /* Initialise the async log queue (must be before task creation) */
    SD_Logger_Queue_Init();

    /* Create the AI service's event flag. Deliberately here, in
     * main(), alongside every other osal_*_create() call - ms_osal.c's
     * "create after the kernel is already running" branch exists but has
     * never been exercised on hardware, and this is not the place to
     * become the first thing that depends on it. */
    ai_vision_service_init();

    /* The dose-schedule alarm and its event flag, created here
     * for exactly the same reason as the AI service's flag above - the UI
     * task arms the alarm as soon as it has read the gallery, so the object
     * has to be real before any task starts. */
    state_machine_service_init();

    /* The mutex that serialises RTC calendar reads. The logger
     * task stamps every line while the UI task reads the clock to draw it,
     * and the HAL's GetTime/GetDate pair is not safe to interleave. This is
     * the OSAL's mutex primitive finding its first consumer since it was
     * defined. */
    time_source_service_init();

    /* The physical dispenser - four coil GPIOs to the ULN2003
     * and the IR counter's EXTI line.
     *
     * Here, in main(), for the same reason time_source_init() is: it samples
     * the IR module's idle level to derive its break polarity, and that has
     * to be settled before any dose can be dispensed. It configures GPIO and
     * one NVIC line and nothing else - no allocation, no blocking wait, no
     * kernel object - so it is safe this early. With
     * MEDSIGHT_PHYSICAL_DISPENSER=0 it prints one line and returns, and
     * state_machine.c keeps the earlier simulation. */
    dispenser_init();

    /* The buzzer GPIO and the Alert task's event flag.
     * Same placement rationale as every other service init above. */
    buzzer_init();
    buzzer_service_init();

    /* ── Create all RTOS tasks via OSAL ─────────────────────────────────── */
    /*
     * Stack sizes are in 32-bit WORDS.
     * Priorities:  5 = camera/ISP  4 = UI/touch  3 = AI/NPU  2 = logger
     *              1 = heartbeat
     * See the task-definition block above for the derivation.
     */
    osal_task_create(task_camera_isp_fn, "cam_isp",   1024u, NULL, 5u);
    osal_task_create(task_ui_fn,         "ui",         2048u, NULL, 4u);
    /* The NPU pipeline is a task again - but unlike the original
     * free-running 1 Hz demo task, this one is idle until the UI asks it for
     * a capture over an event flag. See ai_vision.h. */
    osal_task_create(task_ai_fn,         "ai",         2048u, NULL, 3u);
    osal_task_create(task_logger_fn,     "logger",     1024u, NULL, 2u);
    osal_task_create(task_heartbeat_fn,  "heartbeat",  256u,  NULL, 1u);
    /* The Program Plan's Alert Task, finally built.
     * Priority 1 - alongside heartbeat, and for the same reason: a tone has
     * no deadline, nothing waits on it, and every other task in this table
     * has a reason to preempt it. The rate-monotonic derivation above
     * is untouched; nothing above it moved. Stack is small because
     * the body is a delay loop over a table of durations. */
    osal_task_create(buzzer_task_fn,     "alert",      256u,  NULL, 1u);

    /* Checkpoint 4: every osal_task_create() call and
     * SD_Logger_Queue_Init()/MX_SDMMC2_SD_Init() returned - everything
     * main() does is finished, the very next line hands off to
     * osal_scheduler_start() -> knl_start_mtkernel(). Fast alternating
     * GREEN/RED blink for ~1 s. If you see this blink but nothing after, the
     * hang is inside knl_start_mtkernel()/usermain() itself. If you never
     * see this blink, the hang is somewhere in the camera/DMA2D/SD bring-up
     * above (i.e. unrelated to the kernel). */
#if MS_BOOT_LED_CHECKPOINTS
    for (int _diag_i = 0; _diag_i < 10; _diag_i++) {
        BSP_LED_Toggle(LED_GREEN);
        BSP_LED_Toggle(LED_RED);
        HAL_Delay(100);
    }
#else
    /* The settle the LED burst above used to provide - see MS_BOOT_SETTLE_MS. */
    HAL_Delay(MS_BOOT_PRESCHED_SETTLE_MS);
#endif
    /* Must run before the scheduler, because the very first thing the idle
     * task does is sleep. See the comment on this function. */
    ms_configure_sleep_clocks();

#if MS_BOOT_TRACE
    printf("boot: handing off to the scheduler at %lums\r\n",
           (unsigned long)HAL_GetTick());
#endif

    /* ── Hand control to the RTOS scheduler - does not return ───────────── */
    osal_scheduler_start();

    /* USER CODE END 2 */

    /* Execution never reaches here. osal_scheduler_start() triggers Error_Handler
     * via configASSERT if heap is too small to start the scheduler. */
    while (1) {}
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};

    if (HAL_PWREx_ConfigSupply(PWR_EXTERNAL_SOURCE_SUPPLY) != HAL_OK) {
        Error_Handler();
    }

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_NONE;
    RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_NONE;
    RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_NONE;
    RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    HAL_RCC_GetClockConfig(&RCC_ClkInitStruct);
    if ((RCC_ClkInitStruct.CPUCLKSource == RCC_CPUCLKSOURCE_IC1) ||
        (RCC_ClkInitStruct.SYSCLKSource == RCC_SYSCLKSOURCE_IC2_IC6_IC11))
    {
        RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK);
        RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_HSI;
        RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
        if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK) {
            Error_Handler();
        }
    }

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_NONE;
    RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL1.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL1.PLLM = 4;
    RCC_OscInitStruct.PLL1.PLLN = 75;
    RCC_OscInitStruct.PLL1.PLLFractional = 0;
    RCC_OscInitStruct.PLL1.PLLP1 = 1;
    RCC_OscInitStruct.PLL1.PLLP2 = 1;
    RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_NONE;
    RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_NONE;
    RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_HCLK
                                | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1
                                | RCC_CLOCKTYPE_PCLK2  | RCC_CLOCKTYPE_PCLK5
                                | RCC_CLOCKTYPE_PCLK4;
    RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_IC1;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;
    RCC_ClkInitStruct.APB5CLKDivider = RCC_APB5_DIV1;
    RCC_ClkInitStruct.IC1Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    RCC_ClkInitStruct.IC1Selection.ClockDivider = 2;
    RCC_ClkInitStruct.IC2Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    RCC_ClkInitStruct.IC2Selection.ClockDivider = 3;
    RCC_ClkInitStruct.IC6Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    RCC_ClkInitStruct.IC6Selection.ClockDivider = 3;
    RCC_ClkInitStruct.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
    RCC_ClkInitStruct.IC11Selection.ClockDivider = 3;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    /* ── Pin the XSPI kernel clocks, do not inherit them ──────
     *
     * This function used to select CKPER and nothing else, leaving the XSPI1
     * and XSPI2 kernel clocks at whatever the RCC happened to hold. From a
     * debugger that is harmless: connect_under_reset means the RCC is at its
     * reset defaults and the BSP's prescaler maths works out.
     *
     * Booting from external flash it is not harmless. The boot ROM and the
     * first-stage loader both reconfigure the XSPI clock to read the
     * application out of flash quickly, and we inherit whatever they left.
     * The BSP then applies a prescaler derived from an assumed source, the
     * real timing is wrong, and the flash stops answering: every command
     * "succeeds" (nothing verifies a write) while every READ fails, in SPI
     * and in Octal-DTR alike, on a chip that had served up 899 KB seconds
     * earlier. That asymmetry - writes fine, reads dead, both modes - is what
     * points at clocking rather than protocol.
     *
     * HCLK is the right source, not CLKP. The BSP states its own assumption
     * in stm32n6570_discovery_xspi.c - "XSPI clock = 200MHz / ClockPrescaler"
     * - and on this board SYSCLK is 400 MHz with an AHB prescaler of 2, so
     * HCLK IS 200 MHz. That is also what the reset default feeds the XSPI,
     * which is why development boot has always worked. Selecting CLKP (HSI,
     * 64 MHz) was deterministic but wrong: it made the BSP prescaler maths
     * produce a frequency the flash could not be read at. Pinning HCLK gives
     * both boot paths the same 200 MHz the driver was written against, and it
     * matches the workaround ST give for XSPI trouble in load-and-run mode. */
    /* Quiesce the XSPIs BEFORE touching their clock source.
     *
     * Changing a peripheral kernel clock while that peripheral is enabled is
     * not allowed, and booting from external flash is exactly the case where
     * it IS enabled: the first-stage loader has just finished using XSPI2 to
     * read this application out of flash and hands it over still running.
     * Selecting a new source underneath it wedges the interface - after which
     * every command appears to succeed and every read returns nothing, in any
     * mode, on a chip that worked seconds earlier.
     *
     * From the debugger none of this applies, because connect_under_reset
     * means both XSPIs are already idle at their reset defaults. That is why
     * development boot never needed this and standalone boot cannot work
     * without it. XSPIM is the shared I/O manager and goes first. */
    __HAL_RCC_XSPIM_FORCE_RESET();
    __HAL_RCC_XSPI1_FORCE_RESET();
    __HAL_RCC_XSPI2_FORCE_RESET();
    __HAL_RCC_XSPI2_RELEASE_RESET();
    __HAL_RCC_XSPI1_RELEASE_RESET();
    __HAL_RCC_XSPIM_RELEASE_RESET();

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CKPER
                                             | RCC_PERIPHCLK_XSPI1
                                             | RCC_PERIPHCLK_XSPI2;
    PeriphClkInitStruct.CkperClockSelection  = RCC_CLKPCLKSOURCE_HSI;
    PeriphClkInitStruct.Xspi1ClockSelection  = RCC_XSPI1CLKSOURCE_HCLK;
    PeriphClkInitStruct.Xspi2ClockSelection  = RCC_XSPI2CLKSOURCE_HCLK;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief DCMIPP Initialization Function
  */
static void MX_DCMIPP_Init(void)
{
    DCMIPP_PipeConfTypeDef pPipeConf = {0};
    DCMIPP_CSI_PIPE_ConfTypeDef pCSIPipeConf = {0};
    DCMIPP_CSI_ConfTypeDef csiconf = {0};
    DCMIPP_DownsizeTypeDef DonwsizeConf = {0};

    hdcmipp.Instance = DCMIPP;
    if (HAL_DCMIPP_Init(&hdcmipp) != HAL_OK) {
        Error_Handler();
    }

    csiconf.DataLaneMapping = DCMIPP_CSI_PHYSICAL_DATA_LANES;
    csiconf.NumberOfLanes   = DCMIPP_CSI_TWO_DATA_LANES;
    csiconf.PHYBitrate      = DCMIPP_CSI_PHY_BT_1600;
    if (HAL_DCMIPP_CSI_SetConfig(&hdcmipp, &csiconf) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_DCMIPP_CSI_SetVCConfig(&hdcmipp, DCMIPP_VIRTUAL_CHANNEL0, DCMIPP_CSI_DT_BPP10) != HAL_OK) {
        Error_Handler();
    }

    pCSIPipeConf.DataTypeMode = DCMIPP_DTMODE_DTIDA;
    pCSIPipeConf.DataTypeIDA  = DCMIPP_DT_RAW10;
    pCSIPipeConf.DataTypeIDB  = DCMIPP_DT_RAW10;
    if (HAL_DCMIPP_CSI_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pCSIPipeConf) != HAL_OK) {
        Error_Handler();
    }

    pPipeConf.FrameRate  = DCMIPP_FRAME_RATE_ALL;
    pPipeConf.PixelPackerFormat = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
    pPipeConf.PixelPipePitch  = 1600;
    if (HAL_DCMIPP_PIPE_SetConfig(&hdcmipp, DCMIPP_PIPE1, &pPipeConf) != HAL_OK) {
        Error_Handler();
    }

    DonwsizeConf.HSize      = 800;
    DonwsizeConf.VSize      = 480;
    DonwsizeConf.HRatio     = DOWNSCALE_RATIO(IMX335_WIDTH, DonwsizeConf.HSize);
    DonwsizeConf.VRatio     = DOWNSCALE_RATIO(IMX335_HEIGHT, DonwsizeConf.VSize);
    DonwsizeConf.HDivFactor = DIV_FACTOR(IMX335_WIDTH, DonwsizeConf.HSize);
    DonwsizeConf.VDivFactor = DIV_FACTOR(IMX335_HEIGHT, DonwsizeConf.VSize);
    if (HAL_DCMIPP_PIPE_SetDownsizeConfig(&hdcmipp, DCMIPP_PIPE1, &DonwsizeConf) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_DCMIPP_PIPE_EnableDownsize(&hdcmipp, DCMIPP_PIPE1) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief SDMMC2 SD Initialization
  */
static void MX_SDMMC2_SD_Init(void)
{
    hsd2.Instance = SDMMC2;
    hsd2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd2.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd2.Init.ClockDiv = 2;
}

/**
  * @brief LCD Init
  */
static void LCD_Init(uint32_t Width, uint32_t Height)
{
    LTDC_LayerCfgTypeDef pLayerCfg = {0};

    hltdc.Instance = LTDC;
    hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
    hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
    hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
    hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
    hltdc.Init.HorizontalSync     = RK050HR18_HSYNC - 1;
    hltdc.Init.AccumulatedHBP     = RK050HR18_HSYNC + RK050HR18_HBP - 1;
    hltdc.Init.AccumulatedActiveW = RK050HR18_HSYNC + Width + RK050HR18_HBP - 1;
    hltdc.Init.TotalWidth         = RK050HR18_HSYNC + Width + RK050HR18_HBP + RK050HR18_HFP - 1;
    hltdc.Init.VerticalSync       = RK050HR18_VSYNC - 1;
    hltdc.Init.AccumulatedVBP     = RK050HR18_VSYNC + RK050HR18_VBP - 1;
    hltdc.Init.AccumulatedActiveH = RK050HR18_VSYNC + Height + RK050HR18_VBP - 1;
    hltdc.Init.TotalHeigh         = RK050HR18_VSYNC + Height + RK050HR18_VBP + RK050HR18_VFP - 1;
    /* Was pure green. The layer is now deliberately disabled for
     * the ~750 ms of each face capture (state_machine.c's display_blank(),
     * which is how the NPU corrupting the one framebuffer is hidden), and
     * this colour is what the panel shows while it is. Blush = the theme's
     * frame colour, so the pause reads as intentional.
     *
     * Trade-off worth knowing: a genuine "layer never came up" fault now
     * looks like this same blush rather than an alarming green. UART is the
     * diagnostic for that case, not the panel colour. */
    hltdc.Init.Backcolor.Blue  = 0xE3;
    hltdc.Init.Backcolor.Green = 0xE3;
    hltdc.Init.Backcolor.Red   = 0xF8;
    if (HAL_LTDC_Init(&hltdc) != HAL_OK) {
        Error_Handler();
    }

    /* Clean boot sequence - TRIED AND REVERTED.
     *
     * The first attempt here pointed the LTDC at GUI_BUFFER_ADDRESS from
     * this very early point in boot and pre-cleared it with a CPU-side
     * memset() before anything else had ever touched that address. On a
     * genuine cold boot this produced a solid green screen (the LTDC
     * Backcolor below, i.e. the layer never came up with valid pixel data)
     * and NO UART output at all - not even a fault report, which given
     * COM_Init() runs earlier in main() than LCD_Init(), means whatever
     * failed here failed hard enough that even the fault path never got a
     * chance to run.
     *
     * Per this project's own rule ("the most recently
     * added subsystem is the first suspect") and the plan's own explicit
     * caveat ("no document proves GUI_BUFFER_ADDRESS doesn't collide with
     * something, verify before relying on it") - this was exactly that
     * unverified access, made at the single hardest point in boot to
     * diagnose a failure. Reverted: the LTDC boots pointed at
     * BUFFER_ADDRESS again, unchanged. GUI_BUFFER_ADDRESS's
     * first real touch now happens in state_machine_init() (state_machine.c)
     * - after UART and the scheduler are definitely alive - bracketed by
     * unconditional printf()s so a repeat failure is actually diagnosable
     * instead of silent. Do not move this boot-time clear back here until
     * that later access has been confirmed to work on real hardware. */
    pLayerCfg.WindowX0       = 0;
    pLayerCfg.WindowX1       = Width;
    pLayerCfg.WindowY0       = 0;
    pLayerCfg.WindowY1       = Height;
    pLayerCfg.PixelFormat    = LTDC_PIXEL_FORMAT_RGB565;
    pLayerCfg.FBStartAdress  = BUFFER_ADDRESS;
    pLayerCfg.Alpha          = LTDC_LxCACR_CONSTA;
    pLayerCfg.Alpha0         = 0;
    pLayerCfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
    pLayerCfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
    pLayerCfg.ImageWidth     = Width;
    pLayerCfg.ImageHeight    = Height;
    pLayerCfg.Backcolor.Blue  = 0;
    pLayerCfg.Backcolor.Green = 0;
    pLayerCfg.Backcolor.Red   = 0;
    if (HAL_LTDC_ConfigLayer(&hltdc, &pLayerCfg, LTDC_LAYER_1)) {
        Error_Handler();
    }
}

/* USER CODE BEGIN 4 */

static void IMX335_Probe(uint32_t Resolution, uint32_t PixelFormat)
{
    IMX335_IO_t IOCtx;
    uint32_t    id;

    IOCtx.Address  = CAMERA_IMX335_ADDRESS;
    IOCtx.Init     = BSP_I2C1_Init;
    IOCtx.DeInit   = BSP_I2C1_DeInit;
    IOCtx.ReadReg  = BSP_I2C1_ReadReg16;
    IOCtx.WriteReg = BSP_I2C1_WriteReg16;
    IOCtx.GetTick  = BSP_GetTick;

    if (IMX335_RegisterBusIO(&IMX335Obj, &IOCtx) != IMX335_OK) {
        Error_Handler();
    } else if (IMX335_ReadID(&IMX335Obj, &id) != IMX335_OK) {
        Error_Handler();
    } else {
        if (id != (uint32_t)IMX335_CHIP_ID) {
            Error_Handler();
        } else {
            if (IMX335_Init(&IMX335Obj, Resolution, PixelFormat) != IMX335_OK) {
                Error_Handler();
            } else if (IMX335_SetFrequency(&IMX335Obj, IMX335_INCK_24MHZ) != IMX335_OK) {
                Error_Handler();
            } else if (IMX335_MirrorFlipConfig(&IMX335Obj, IMX335_MIRROR) != IMX335_OK) {
                Error_Handler();
            } else {
                return;
            }
        }
    }
}

static ISP_StatusTypeDef GetSensorInfoHelper(uint32_t Instance, ISP_SensorInfoTypeDef *SensorInfo)
{
    UNUSED(Instance);
    return (ISP_StatusTypeDef)IMX335_GetSensorInfo(&IMX335Obj, (IMX335_SensorInfo_t *)SensorInfo);
}

static ISP_StatusTypeDef SetSensorGainHelper(uint32_t Instance, int32_t Gain)
{
    UNUSED(Instance);
    isp_gain = Gain;
    return (ISP_StatusTypeDef)IMX335_SetGain(&IMX335Obj, Gain);
}

static ISP_StatusTypeDef GetSensorGainHelper(uint32_t Instance, int32_t *Gain)
{
    UNUSED(Instance);
    *Gain = isp_gain;
    return ISP_OK;
}

static ISP_StatusTypeDef SetSensorExposureHelper(uint32_t Instance, int32_t Exposure)
{
    UNUSED(Instance);
    isp_exposure = Exposure;
    return (ISP_StatusTypeDef)IMX335_SetExposure(&IMX335Obj, Exposure);
}

static ISP_StatusTypeDef GetSensorExposureHelper(uint32_t Instance, int32_t *Exposure)
{
    UNUSED(Instance);
    *Exposure = isp_exposure;
    return ISP_OK;
}

void HAL_DCMIPP_PIPE_FrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp, uint32_t Pipe)
{
    NbMainFrames++;
}

void HAL_DCMIPP_PIPE_VsyncEventCallback(DCMIPP_HandleTypeDef *hdcmipp, uint32_t Pipe)
{
    UNUSED(hdcmipp);
    switch (Pipe) {
        case DCMIPP_PIPE0:
            ISP_IncDumpFrameId(&hcamera_isp);
            break;
        case DCMIPP_PIPE1:
            ISP_IncMainFrameId(&hcamera_isp);
            ISP_GatherStatistics(&hcamera_isp);
            break;
        case DCMIPP_PIPE2:
            ISP_IncAncillaryFrameId(&hcamera_isp);
            break;
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    while (1) {
        HAL_Delay(250);
        BSP_LED_Toggle(LED_RED);
    }
    /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    printf("\r\n*** HAL ASSERT FAILED: %s:%lu ***\r\n",
           file ? (const char *)file : "?", (unsigned long)line);
    Error_Handler();
}
#endif /* USE_FULL_ASSERT */

/**
 * @brief  Replace newlib's assert() handler with one that says
 *         something useful.
 *
 * The ST Edge AI runtime asserts rather than returning an error when the NPU's
 * epoch-controller blobs in external OSPI flash do not match the ones the
 * running binary expects. Stock newlib prints the raw expression and aborts:
 *
 *     assertion "ret == 1" failed: file ".../ll_aton_runtime.c", line 454,
 *     function: LL_ATON_RT_Init_Network
 *
 * which says nothing about the actual cause. That exact line has now cost this
 * project real bench time three separate times: weights never flashed,
 * a second and undiscovered weight pool, and
 * the Release build laying
 * the blobs out in a different order than the flashed image). Every one of
 * them was the same underlying condition: **flash content does not match this
 * binary.**
 *
 * Providing this symbol overrides libc's copy at link time, because the
 * linker resolves objects before searching libraries.
 *
 * Ends in Error_Handler() (blinking RED) rather than a bare while(1), so a
 * board with no serial attached still shows that something failed rather than
 * appearing merely frozen.
 */
void __assert_func(const char *file, int line, const char *func,
                   const char *failedexpr)
{
    printf("\r\n"
           "*** ASSERTION FAILED ***\r\n"
           "  expr : %s\r\n"
           "  at   : %s:%d\r\n"
           "  in   : %s\r\n",
           failedexpr ? failedexpr : "?",
           file ? file : "?", line,
           func ? func : "?");

    /* Recognise the one that keeps happening and say what to do about it. */
    if ((func != NULL) && (strstr(func, "LL_ATON_RT_Init_Network") != NULL))
    {
        printf(
          "\r\n"
          "  DIAGNOSIS: the NPU epoch-controller blobs in external OSPI NOR\r\n"
          "  flash do not match this binary. The linker script marks OSPI_NOR\r\n"
          "  (NOLOAD), so a normal Debug/Run NEVER programs them - they are\r\n"
          "  flashed once, by hand, and must be re-flashed whenever the model\r\n"
          "  files or their link order change.\r\n"
          "\r\n"
          "  FIX: re-flash the five weight images from weights/ with\r\n"
          "  STM32_Programmer_CLI in UR mode, then FULLY POWER CYCLE the\r\n"
          "  board (unplug USB - a debugger reset is not enough):\r\n"
          "    weights/fd_data.xSPI2.bin      -> 0x70380000\r\n"
          "    weights/ec_blobs.xSPI2.bin     -> 0x71000000\r\n"
          "    weights/faceid_data.xSPI2.bin  -> 0x72000000\r\n"
          "    weights/hand_atonbuf.xSPI2.bin -> 0x73000000\r\n"
          "    weights/pill_atonbuf.xSPI2.bin -> 0x73400000\r\n"
          "  The handbook gives the command line in section 8.5.\r\n");
    }

    Error_Handler();

    /* Error_Handler() never returns; this satisfies the noreturn contract. */
    while (1) { }
}
