/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ui/anime_ui.h
  * @brief   Lumio mascot animation layer - public API.
  *
  *          Implements MASCOT_IDLE state with DMA2D alpha-blending
  *          over the live camera framebuffer. All other states are stubbed and
  *          will be filled in during Earlier builds.
  *
  *          IP-compliance: Lumio is an original character. No existing franchise,
  *          trademark, or copyrighted IP is referenced anywhere in this module.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef UI_ANIME_UI_H
#define UI_ANIME_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32n6xx_hal.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Mascot state enumeration
 * Defined here once; used by interactive_gui, state_machine
 *, and this renderer. The renderer never decides *why* a state
 * changes - it only decides *how* to render it.
 * --------------------------------------------------------------------------- */
typedef enum
{
    MASCOT_IDLE    = 0,  /**< Awake wave + blink loop, 8 frames      */
    MASCOT_ACTIVE  = 1,  /**< No artwork of its own; renders the idle loop        */
    MASCOT_SUCCESS = 2,  /**< No artwork of its own; renders the idle loop        */
    MASCOT_ERROR   = 3,  /**< Crying loop, 3 frames, at MASCOT_SAD_* */
} mascot_state_t;

/* ---------------------------------------------------------------------------
 * Sprite geometry.
 *
 * Lumio is no longer drawn from circles and triangles - the
 * renderer blits the project designer's own artwork, cropped and packed into
 * ui_sprite_mascot_idle by tools/gen_ui_assets.py. The geometry below tracks
 * that sprite and gui_draw.h's MASCOT_BG_* box, which is where every screen
 * reserves space for it.
 *
 * MASCOT_IDLE is an 8-frame wave/blink loop with a breathing bob.
 * MASCOT_ERROR is a 3-frame crying loop drawn at MASCOT_SAD_* (gui_draw.h),
 * selected by state_machine.c on the face-not-recognised screen.
 * --------------------------------------------------------------------------- */
#define LUMIO_DRAW_W     214u          /**< matches ui_sprite_mascot_a.w         */
#define LUMIO_DRAW_H     213u          /**< matches ui_sprite_mascot_a.h         */
#define LUMIO_BOB_PX       5u          /**< breathing travel, in pixels          */
#define LUMIO_IDLE_FRAMES  8u          /**< Number of frames in the idle loop    */
#define LUMIO_FPS          4u          /**< Target animation frame rate (4 FPS)  */
#define LUMIO_FRAME_MS   (1000u / LUMIO_FPS)  /**< ms per frame = 250 ms        */

/** Right side of screen, beside the buttons (matches MASCOT_BG_X/Y). */
#define LUMIO_X_POS      536u   /* pixels from left edge */
#define LUMIO_Y_BASE     136u   /* pixels from top       */

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
  * @brief  Initialise the animation layer.
  * @param  hdma2d_handle  Pointer to the DMA2D handle initialised by main.c.
  *                        The handle must already have been configured for
  *                        Memory-to-Memory-with-blending mode before this call.
  * @param  fb_addr        Base address of the RGB565 LCD framebuffer (e.g. 0x34200000).
  * @param  fb_width       Framebuffer stride in pixels (e.g. 800).
  */
void anime_ui_init(DMA2D_HandleTypeDef *hdma2d_handle,
                   uint32_t             fb_addr,
                   uint32_t             fb_width);

/**
  * @brief  Advance the animation and trigger a DMA2D blend if a new frame is due.
  *         Call this from the main loop on every iteration; it self-throttles to
  *         LUMIO_FPS using HAL_GetTick(). Non-blocking - returns immediately after
  *         kicking off the DMA2D transfer.
  * @param  tick_ms  Current timestamp in milliseconds, e.g. HAL_GetTick().
  */
void anime_ui_update(uint32_t tick_ms);

/**
  * @brief  Switch the mascot to a new state.
  *         Safe to call from any context; the renderer picks up the new state
  *         on the next anime_ui_update() call.
  * @param  state  Target mascot state.
  */
void anime_ui_set_state(mascot_state_t state);

/**
  * @brief  DMA2D transfer-complete callback hook.
  *         Call this from HAL_DMA2D_XferCpltCallback() in stm32n6xx_it.c or
  *         main.c so the animation layer knows the GPU is free for the next frame.
  */
void anime_ui_dma2d_cplt_cb(void);

/**
  * @brief  Override the destination framebuffer address at runtime.
  */
void anime_ui_set_dest_buffer(uint32_t dest_buffer);

/**
  * @brief  Set the rectangle the MASCOT_ERROR (crying) frames are drawn in.
  *
  *         Until now the crying pose was hard-coded to
  *         MASCOT_SAD_* in gui_draw.h, which is the box the two-choice
  *         screen reserves for it. A missed dose selects MASCOT_ERROR while
  *         the HOME screen is up, and that rectangle sits on top of the
  *         home screen's REGISTER and DISPENSE buttons - so the mascot was
  *         drawn over them.
  *
  *         Every screen that selects MASCOT_ERROR now says where it wants
  *         it. The sprite is centred in the box, so the same three frames
  *         work in the two-choice screen's 150x158 slot and in the home
  *         screen's 214x236 mascot box.
  */
void anime_ui_set_error_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
  * @brief  Set the background colour used to fill transparent sprite pixels.
  *         Call this whenever the screen background changes so the mascot
  *         bounding box is always redrawn atomically (no clear+draw flicker).
  *         Default: 0xADD6 (light sky-blue).
  */
void anime_ui_set_bg_color(uint16_t color);

/* ---------------------------------------------------------------------------
 * Direct state renderers - called internally by anime_ui_update(), and
 * exposed so a caller can force one frame without waiting for the tick.
 * --------------------------------------------------------------------------- */
void anime_ui_state_active(void);   /**< renders the idle pose  */
void anime_ui_state_success(void);  /**< renders the idle pose  */
void anime_ui_state_error(void);    /**< renders the crying pose */

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIME_UI_H */
