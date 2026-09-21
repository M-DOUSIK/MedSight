#include "ui/touch_driver.h"
#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_ts.h"
#include "ms_osal.h"
#include <stdio.h>

/* GT911 reset timing.
 *
 * BSP_TS_Init() drives the controller's NRST high and then probes it over I2C
 * *immediately* - there is no delay between releasing reset and the first
 * GT911_ReadID(). The GT911 needs tens of milliseconds to boot before it
 * answers, so whether the probe succeeds comes down to how long the code
 * happens to take to get from one line to the next.
 *
 * That is why touch initialisation worked in the Debug build (-O0) and failed
 * in Release (-Os) with BSP_ERROR_NO_INIT (-1): the optimised build simply got
 * to the I2C read sooner than the part could answer.
 *
 * There is a second reason the reset needs handling here rather than being
 * left to the BSP: LTDC's own MspInit configures PE1 - which is the GT911's
 * NRST - as a push-pull output, and GPIO ODR resets to zero, so bringing the
 * display up leaves the touch controller held in reset. Nothing releases it
 * until BSP_TS_Init() runs.
 *
 * So do the reset properly and explicitly before handing over to the BSP:
 * assert, hold, release, then wait for the part to boot. Values are the
 * datasheet minimums with comfortable margin; this runs once at start-up, so
 * the milliseconds cost nothing. */
#define GT911_RESET_HOLD_MS   20u   /* NRST asserted low                     */
#define GT911_RESET_BOOT_MS  120u   /* after release, before first I2C access */

static void gt911_hardware_reset(void)
{
    GPIO_InitTypeDef gpio = {0};

    TS_NRST_GPIO_CLK_ENABLE();

    gpio.Pin   = TS_NRST_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(TS_NRST_GPIO_PORT, &gpio);

    /* Assert reset, hold, release. */
    HAL_GPIO_WritePin(TS_NRST_GPIO_PORT, TS_NRST_PIN, GPIO_PIN_RESET);
    osal_delay_ms(GT911_RESET_HOLD_MS);
    HAL_GPIO_WritePin(TS_NRST_GPIO_PORT, TS_NRST_PIN, GPIO_PIN_SET);

    /* Let the controller boot before anyone talks to it. osal_delay_ms()
     * rather than HAL_Delay() so the CPU is yielded - this runs from the UI
     * task, which is above the AI and logger tasks in priority. */
    osal_delay_ms(GT911_RESET_BOOT_MS);
}

void touch_driver_init(void)
{
    TS_Init_t ts_init;
    ts_init.Width = 800;
    ts_init.Height = 480;
    ts_init.Orientation = TS_SWAP_NONE;
    ts_init.Accuracy = 5;

    gt911_hardware_reset();

    int32_t status = BSP_TS_Init(0, &ts_init);
    if (status != BSP_ERROR_NONE)
    {
        /* One retry with a fresh reset. A GT911 that missed its boot window
         * answers perfectly well on a second attempt, and a touchscreen that
         * silently fails to initialise makes the whole device unusable. */
        printf("touch_driver: BSP_TS_Init failed (%ld), retrying after reset.\n",
               status);
        gt911_hardware_reset();
        status = BSP_TS_Init(0, &ts_init);
    }

    if (status != BSP_ERROR_NONE)
    {
        printf("Error: BSP_TS_Init failed with status %ld\n", status);
    }
    else
    {
        printf("BSP_TS_Init successful.\n");
    }
}

bool touch_driver_get_touch(uint32_t *x, uint32_t *y)
{
    TS_State_t ts_state = {0};
    if (BSP_TS_GetState(0, &ts_state) == BSP_ERROR_NONE)
    {
        if (ts_state.TouchDetected)
        {
            if (x) *x = ts_state.TouchX;
            if (y) *y = ts_state.TouchY;
            return true;
        }
    }
    return false;
}

void user_button_init(void)
{
    BSP_PB_Init(BUTTON_USER1, BUTTON_MODE_GPIO);
}

bool user_button_is_pressed(void)
{
    return (BSP_PB_GetState(BUTTON_USER1) == BUTTON_PRESSED);
}
