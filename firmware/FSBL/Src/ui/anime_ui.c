/* anime_ui.c - Lumio mascot layer
 *
 * This module's procedural renderer was replaced with the project
 * designer's actual artwork. Earlier builds approximated the character with
 * ~150 lines of circles, ovals and triangles - a reasonable stand-in while
 * no asset pipeline existed, but it never looked like the reference sheets.
 * tools/gen_ui_assets.py now crops the designer's PNGs and packs them into
 * 4bpp palettised sprites, and this file just places them.
 *
 * THE IDLE LOOP. The designer drew the mascot as a two-frame animation
 * (arms down, arms up) surrounded by twinkling sparkles. Both frames are
 * used here, plus a blink frame synthesised from the arms-down pose in the
 * same style, giving an awake, active character rather than a static one:
 *
 *      frame 0 1 2 3 4 5 6 7      (8 frames at 4 FPS = a 2 second loop)
 *      pose  A A K A B B A K      A = arms down, B = arms up, K = blink
 *      spark... o O O o.      sparkles twinkle around the wave
 *
 * plus a slow breathing bob applied to every frame.
 *
 * MASCOT STATES. Earlier builds originally kept the mascot idle-only,
 * on the reasoning that a full-screen state change already carries the same
 * information. That was overridden by the project owner:
 * MASCOT_ERROR is now a real, animated state, drawn from the three frames of
 * the designer's error GIF (a crying pose) on the face-not-recognised
 * screen. anime_ui_set_state() is live and called from state_machine.c.
 *
 * Each state owns its own box, because the poses are used at different sizes
 * and positions:
 *   MASCOT_IDLE  - 214x213 at MASCOT_BG_*,  8-frame wave/blink loop
 *   MASCOT_ERROR - 150x153 at MASCOT_SAD_*, 3-frame crying loop
 * MASCOT_ACTIVE and MASCOT_SUCCESS still fall back to the idle loop; no
 * artwork has been cut for them.
 */

#include "ui/anime_ui.h"
#include "ui/gui_draw.h"
#include "ui/ui_assets.h"
#include "stm32n6xx_hal.h"

/* ── Module state ───────────────────────────────────────────────────────── */
static DMA2D_HandleTypeDef *s_hdma2d    = NULL;
static uint32_t             s_fb_addr   = 0;
static uint32_t             s_fb_width  = 800;
static mascot_state_t       s_state     = MASCOT_IDLE;
static uint8_t              s_frame_idx = 0;
static uint32_t             s_last_tick = 0;
static volatile uint8_t     s_busy      = 0;
static uint16_t             s_bg_color  = 0xFFFF;

/* Where the MASCOT_ERROR frames go. Defaults to the two-choice screen's
 * reserved box, which is where they were hard-coded before
 * the home screen a reason to show them too. */
static uint16_t s_sad_x = MASCOT_SAD_X;
static uint16_t s_sad_y = MASCOT_SAD_Y;
static uint16_t s_sad_w = MASCOT_SAD_W;
static uint16_t s_sad_h = MASCOT_SAD_H;

void anime_ui_set_error_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    s_sad_x = x; s_sad_y = y; s_sad_w = w; s_sad_h = h;
}

/* Per-frame pose: 0 = arms down, 1 = arms up, 2 = blink. */
static const uint8_t s_pose[LUMIO_IDLE_FRAMES] = { 0, 0, 2, 0, 1, 1, 0, 2 };

/* Breathing bob (pixels, negative = up). */
static const int8_t s_bob[LUMIO_IDLE_FRAMES]  = { 0, -2, -3, -4, -5, -4, -2, -1 };

/* Sparkle twinkle: bit 0 = big top-left, bit 1 = small top-right,
 * bit 2 = small mid-right. Brightest during the wave (frames 4-5). */
static const uint8_t s_spark[LUMIO_IDLE_FRAMES] = { 0, 0, 0, 0x01, 0x07, 0x07, 0x02, 0 };

/* MASCOT_ERROR: the designer's three error frames, held two ticks each so
 * the cry reads at 4 FPS rather than flickering. */
#define LUMIO_SAD_FRAMES  6u
static const uint8_t s_sad_pose[LUMIO_SAD_FRAMES] = { 0, 0, 1, 1, 2, 2 };

static const ui_sprite_t *sad_sprite(uint8_t f)
{
    switch (s_sad_pose[f % LUMIO_SAD_FRAMES]) {
        case 1:  return &ui_sprite_mascot_sad1;
        case 2:  return &ui_sprite_mascot_sad2;
        default: return &ui_sprite_mascot_sad0;
    }
}

static void draw_sad_frame(uint8_t frame)
{
    if (!s_fb_addr) return;
    gui_draw_init(s_fb_addr, (uint16_t)s_fb_width, 480u);
    /* Clear the whole box, then centre the 150x153 sprite in it. Centring
     * rather than top-left anchoring is what lets the same three frames sit
     * correctly in the two-choice screen's 150x158 slot AND in the home
     * screen's larger 214x236 mascot box. */
    const ui_sprite_t *sp = sad_sprite(frame);
    uint16_t bx = (s_sad_w > sp->w) ? (uint16_t)(s_sad_x + (s_sad_w - sp->w) / 2u) : s_sad_x;
    uint16_t by = (s_sad_h > sp->h) ? (uint16_t)(s_sad_y + (s_sad_h - sp->h) / 2u) : s_sad_y;

    gui_draw_rect(s_sad_x, s_sad_y, s_sad_w, s_sad_h, s_bg_color);
    gui_blit_sprite(bx, by, sp);
    gui_draw_flush_rows(s_sad_y, (uint16_t)(s_sad_y + s_sad_h));
}

static const ui_sprite_t *pose_sprite(uint8_t pose)
{
    switch (pose) {
        case 1:  return &ui_sprite_mascot_b;
        case 2:  return &ui_sprite_mascot_blink;
        default: return &ui_sprite_mascot_a;
    }
}

static void draw_lumio_frame(uint8_t frame)
{
    if (!s_fb_addr) return;
    frame %= LUMIO_IDLE_FRAMES;

    /* anime_ui draws through gui_draw's primitives, so re-point gui_draw at
     * our destination in case a screen switched it. */
    gui_draw_init(s_fb_addr, (uint16_t)s_fb_width, 480u);

    /* Repaint the whole reserved box first: the sprite moves within it and
     * the sparkles come and go, so anything not covered this frame has to go
     * back to the background. */
    gui_draw_rect(LUMIO_X_POS, LUMIO_Y_BASE, MASCOT_BG_W, MASCOT_BG_H, s_bg_color);

    /* Sparkles sit in the transparent corners above the cat's ears. */
    uint8_t sp = s_spark[frame];
    if (sp & 0x01)
        gui_blit_sprite((uint16_t)(LUMIO_X_POS + 2u),
                        (uint16_t)(LUMIO_Y_BASE + 2u), &ui_sprite_sparkle_lg);
    if (sp & 0x02)
        gui_blit_sprite((uint16_t)(LUMIO_X_POS + MASCOT_BG_W - 30u),
                        (uint16_t)(LUMIO_Y_BASE + 8u), &ui_sprite_sparkle_sm);
    if (sp & 0x04)
        gui_blit_sprite((uint16_t)(LUMIO_X_POS + MASCOT_BG_W - 22u),
                        (uint16_t)(LUMIO_Y_BASE + 70u), &ui_sprite_sparkle_sm);

    gui_blit_sprite(LUMIO_X_POS,
                    (uint16_t)((int32_t)LUMIO_Y_BASE + LUMIO_BOB_PX + s_bob[frame]),
                    pose_sprite(s_pose[frame]));

    /* Flush only the mascot's own rows - flushing the whole framebuffer here
     * used to make the rest of the UI tear while the LTDC was mid-scan. */
    gui_draw_flush_rows(LUMIO_Y_BASE, (uint16_t)(LUMIO_Y_BASE + MASCOT_BG_H));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */
void anime_ui_init(DMA2D_HandleTypeDef *hdma2d,
                   uint32_t             fb_addr,
                   uint32_t             fb_width)
{
    s_hdma2d    = hdma2d;
    s_fb_addr   = fb_addr;
    s_fb_width  = fb_width;
    s_state     = MASCOT_IDLE;
    s_frame_idx = 0u;
    s_last_tick = 0u;
    s_busy      = 0u;
}

void anime_ui_set_dest_buffer(uint32_t dest_buffer) { s_fb_addr  = dest_buffer; }
void anime_ui_set_bg_color(uint16_t color)          { s_bg_color = color;       }

void anime_ui_set_state(mascot_state_t state)
{
    if (state != s_state) { s_state = state; s_frame_idx = 0u; }
}

void anime_ui_update(uint32_t tick_ms)
{
    if ((tick_ms - s_last_tick) < LUMIO_FRAME_MS) return;
    s_last_tick = tick_ms;
    if (s_busy) return;
    s_busy = 1u;

    if (s_state == MASCOT_ERROR)
    {
        draw_sad_frame(s_frame_idx);
        s_frame_idx = (uint8_t)((s_frame_idx + 1u) % LUMIO_SAD_FRAMES);
    }
    else
    {
        draw_lumio_frame(s_frame_idx);
        s_frame_idx = (uint8_t)((s_frame_idx + 1u) % LUMIO_IDLE_FRAMES);
    }

    s_busy = 0u;
}

void anime_ui_dma2d_cplt_cb(void) { s_busy = 0u; }

/* Direct renderers, kept for the states that have no artwork of their own.
 * MASCOT_ERROR has real frames; the other two reuse the idle pose. */
void anime_ui_state_active(void)  { draw_lumio_frame(s_frame_idx); }
void anime_ui_state_success(void) { draw_lumio_frame(0u); }
void anime_ui_state_error(void)   { draw_sad_frame(s_frame_idx); }
