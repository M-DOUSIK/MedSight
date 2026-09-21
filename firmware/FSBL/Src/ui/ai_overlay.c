/* ai_overlay.c - see Inc/ui/ai_overlay.h for the frame-buffer rules, which
 * differ between the two views and are the only subtle thing in this file. */

#include "ui/ai_overlay.h"
#include "ui/gui_draw.h"
#include "ai_vision.h"
#include "ai/intake.h"
#include "ai/intake_camera.h"
#include "main.h"
#include "stm32n6xx_hal.h"
#include "ms_osal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#if MEDSIGHT_AI_OVERLAY

/* Where the preview window sits. Chosen to clear the title bar above and the
 * big confirm button below, both of which the elderly-friendly layout needs
 * left alone - the overlay is an addition to those screens, never a
 * replacement for them. */
/* The live pane lives on the confirm screen, whose split layout reserves it -
 * see CONFIRM_PV_* in gui_draw.h. Taking the geometry from there rather than
 * repeating numbers here is what keeps the pane and the button from drifting
 * into each other: the first version of this file hard-coded a window that sat
 * directly on top of the "I TOOK IT" button. */
#define PV_X     CONFIRM_PV_X
#define PV_Y     CONFIRM_PV_Y
#define PV_W     CONFIRM_PV_W
#define PV_H     CONFIRM_PV_H

/* ── WHAT SLICE OF THE CAMERA THIS PANE SHOWS ────────────────────────────
 *
 * The centred square, exactly as the face preview does.
 *
 * It used to map the whole 800x480 frame into the pane. Against a 300x300
 * window that is an ANAMORPHIC squash - 800 across compressed to 300 while
 * 480 down went to the same 300 - so the same face looked narrower here than
 * on the capture screens, from a wider field of view, at half the vertical
 * resolution because of PV_ROW_STEP. Three differences at once, which is why
 * it read as "not sure if it's the border or the shape or the resolution":
 * it was all of them.
 *
 * Sharing the crop means the three camera views are the same picture of the
 * same thing at three moments, which is the whole point of the shared
 * layout. It also matches what the models are given - ai_vision.c crops the
 * centred square for the detector, and the intake ROI is a window inside
 * it. */
#define PV_SRC   ((INTAKE_FRAME_H < INTAKE_FRAME_W) ? INTAKE_FRAME_H \
                                                    : INTAKE_FRAME_W)
#define PV_SRC_X ((INTAKE_FRAME_W - PV_SRC) / 2)
#define PV_SRC_Y ((INTAKE_FRAME_H - PV_SRC) / 2)

/* How often the live pane repaints, and how many source rows it actually
 * reads to do it.
 *
 * These two constants exist for one reason: PSRAM read bandwidth. Each
 * repaint copies source rows out of the camera frame at 0x90400000, which is
 * THROUGHPUT=MID LATENCY=HIGH, and at 252 rows x 1600 bytes that is 403 KB per
 * repaint. At the first setting (150 ms, every row) that came to ~2.7 MB/s of
 * PSRAM traffic sustained for the whole watch, and the measured cost was
 * plain: idle fell to 40% during an intake watch against 88% either side of
 * it. The overlay is a demonstration feature; it is not allowed to cost a
 * third of the power budget this project reports.
 *
 *   PV_ROW_STEP 2 reads every second pane row and writes it twice. Halves the
 *   PSRAM traffic. On a 252 px pane fed from a 480 px source the vertical
 *   sampling goes from 1:1.9 to 1:3.8, which on a 336x252 preview of a face is
 *   not a difference a viewer notices.
 *
 *   PV_PERIOD_MS 250 gives 4 Hz. Still unmistakably live - the box tracks a
 *   hand moving toward a mouth - at 60% of the repaint rate.
 *
 * Together: ~0.8 MB/s, a third of what it was. */
#define PV_PERIOD_MS  250u
/* Was 2, to halve PSRAM traffic while the NPU ran on every frame. Stage 1A
 * now runs on one frame in four (intake_service.c), which frees far more
 * than this ever saved - and skipping every other row made this pane
 * visibly coarser than the two face panels, which is the one thing the
 * shared layout was supposed to prevent. */
#define PV_ROW_STEP   1

static uint32_t s_last_draw;
static bool     s_frame_drawn;
static uint32_t s_n_drawn, s_n_inactive, s_n_invalid;

void ai_overlay_stats(uint32_t *drawn, uint32_t *skipped_inactive,
                      uint32_t *skipped_invalid)
{
    if (drawn)            *drawn            = s_n_drawn;
    if (skipped_inactive) *skipped_inactive = s_n_inactive;
    if (skipped_invalid)  *skipped_invalid  = s_n_invalid;
}

void ai_overlay_reset(void)
{
    s_last_draw   = 0u;
    s_frame_drawn = false;
    s_n_drawn = s_n_inactive = s_n_invalid = 0u;
}

/* Scale a rectangle from source-image space into preview-window space. */
static void map_rect(int sx, int sy, int sw, int sh, int src_w, int src_h,
                      uint16_t *ox, uint16_t *oy, uint16_t *ow, uint16_t *oh)
{
    if (src_w <= 0) src_w = 1;
    if (src_h <= 0) src_h = 1;
    int x0 = PV_X + ((sx * PV_W) / src_w);
    int y0 = PV_Y + ((sy * PV_H) / src_h);
    int w  = (sw * PV_W) / src_w;
    int h  = (sh * PV_H) / src_h;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (x0 < PV_X) x0 = PV_X;
    if (y0 < PV_Y) y0 = PV_Y;
    if ((x0 + w) > (PV_X + PV_W)) w = (PV_X + PV_W) - x0;
    if ((y0 + h) > (PV_Y + PV_H)) h = (PV_Y + PV_H) - y0;
    *ox = (uint16_t)x0; *oy = (uint16_t)y0;
    *ow = (uint16_t)w;  *oh = (uint16_t)h;
}

/* A hollow rectangle, drawn as four thin filled bars. */
static void stroke_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t colour, uint16_t t)
{
    gui_draw_rect(x, y, w, t, colour);
    gui_draw_rect(x, (uint16_t)(y + h - t), w, t, colour);
    gui_draw_rect(x, y, t, h, colour);
    gui_draw_rect((uint16_t)(x + w - t), y, t, h, colour);
}

static void marker(int px, int py, uint16_t colour)
{
    if ((px < PV_X + 2) || (px > PV_X + PV_W - 3)) return;
    if ((py < PV_Y + 2) || (py > PV_Y + PV_H - 3)) return;
    gui_draw_rect((uint16_t)(px - 2), (uint16_t)py, 5, 1, colour);
    gui_draw_rect((uint16_t)px, (uint16_t)(py - 2), 1, 5, colour);
}

/* Geometry of the capture RESULT SCREEN.
 *
 * -- WHY THIS IS A WHOLE SCREEN AND NOT A CORNER PANE --------------------
 *
 * The first version reused the live pane's rectangle (CONFIRM_PV_*) and drew
 * into whatever was already in the framebuffer. On hardware that produced a
 * small picture of the user in the lower left and unreadable garbage
 * everywhere else, and the garbage was not a drawing bug: BOTH face networks'
 * activation scratch is hardcoded by ST's codegen to overlap BUFFER_ADDRESS,
 * so at the instant a capture completes the framebuffer literally CONTAINS
 * NPU TENSORS. Unblanking the display at that moment shows them.
 *
 * So this function does not overlay anything. It OWNS the screen: full
 * repaint, product chrome, one large centred image, a title and a caption.
 * The rule is simple and worth stating because it was violated once already -
 * after a capture, the only safe thing to do with the framebuffer is write
 * ALL of it. */
#define CAP_SIZE   CAPRES_IMG_SZ
#define CAP_X      CAPRES_IMG_X
#define CAP_Y      CAPRES_IMG_Y

/* The preview uses the RESULT screen's geometry, to the pixel.
 *
 * These are two states of one screen, not two screens: the live view with
 * a NEXT button that is not yet available, and the same frame held still
 * with that button lit. Nothing moves at the transition - the image stays
 * exactly where it was, the caption line is replaced, and the button turns
 * from grey to green. A viewer reads that as the device having FINISHED
 * something, which is what actually happened.
 *
 * Sharing CAPRES_* rather than repeating numbers is what keeps that true
 * when someone moves the panel later. */
#define FV_SIZE    CAPRES_IMG_SZ
#define FV_X       CAPRES_IMG_X
#define FV_Y       CAPRES_IMG_Y
#define FV_RADIUS  22           /* image corner; the border adds 4 outside */
#define FV_BORDER  4
#define FV_PERIOD_MS 120u

/* Both screens put their one line of secondary text HERE, in the same font,
 * at the bottom centre of the SCREEN - not under the image.
 *
 * Centring it on the image column (CAPRES_IMG_X + SZ/2 = 206) ran it straight
 * over the bottom-left corner motif gui_draw_frame() draws at MOTIF_MARGIN.
 * The horizontal middle is the one part of the bottom band clear of BOTH
 * corner motifs, which is why every screen in this product that wants a
 * bottom line puts it there.
 *
 * y = 430 with ui_font_md (line_height 28) runs 430..458, clearing the frame
 * rule - FRAME_INSET 6 + FRAME_THICK 10 puts that at 464..474. Six pixels of
 * daylight, deliberately: the border IS the product's identity here, and text
 * touching it reads as a bug rather than as a layout.
 *
 * One font for both, so the live hint and the result caption are visibly the
 * same line of the same screen changing its words - which is the whole point
 * of sharing the layout in the first place. */
#define CAP_CAPTION_Y  430u
#define CAP_CAPTION_CX 400u
#define CAP_CAPTION_F  (&ui_font_md)

/* Horizontal inset of the rounded corner at row `dy` from the nearest
 * horizontal edge. Zero once dy >= FV_RADIUS, which is every row in the
 * middle of the viewport - so the per-row cost of the curve is a compare. */
static int corner_inset_r(int dy, int radius)
{
    if (dy >= radius) {
        return 0;
    }
    const float r = (float)radius;
    const float k = r - (float)dy - 0.5f;
    const float d = r - sqrtf((r * r) - (k * k));
    return (int)(d + 0.5f);
}

static inline int corner_inset(int dy) { return corner_inset_r(dy, FV_RADIUS); }


/* ──────────────────────────────────────────────────────────────────────
 * AN ANTI-ALIASED ROUNDED BORDER
 * ──────────────────────────────────────────────────────────────────────
 *
 * gui_stroke_round_rect() picks, for each row, a single integer inset from
 * sqrt() and fills a hard-edged span. That is right for the product's
 * buttons and panels, where the shapes are large and the eye reads them as
 * flat colour. It is visibly WRONG around a photograph: at radius 26 the
 * arc advances in whole-pixel steps and the staircase is the first thing you
 * see, because a camera image gives the eye a smooth reference to compare
 * the edge against.
 *
 * This draws the same ring with 4x4 supersampled coverage and blends into
 * whatever is already on the framebuffer. Only the four corner boxes need it
 * - the straight runs are exact - so the cost is 4 x 30 x 30 x 16 samples,
 * once per screen, on a path that is already doing a 90,000-pixel blit.
 *
 * It blends against the DESTINATION rather than a known background colour,
 * which is what lets the same function sit correctly on the page colour
 * outside the image and on the photograph inside it. */

static inline uint16_t blend565(uint16_t dst, uint16_t src, int a)
{
    /* a is 0..256. Channels stay in their own lanes; no 8-bit round trip. */
    const int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    const int r = dr + (((sr - dr) * a) >> 8);
    const int g = dg + (((sg - dg) * a) >> 8);
    const int b = db + (((sb - db) * a) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Signed distance from p to a rounded rect centred at (cx,cy) with half
 * extents (hx,hy) and corner radius r. Negative inside. */
static inline float rr_sdf(float px, float py, float cx, float cy,
                            float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - (hx - r);
    float qy = fabsf(py - cy) - (hy - r);
    if (qx < 0.0f) qx = 0.0f;
    if (qy < 0.0f) qy = 0.0f;
    return sqrtf((qx * qx) + (qy * qy)) - r;
}

/* Stroke a rounded border just OUTSIDE the `iw` x `ih` image at (ix,iy),
 * `t` pixels thick, with the image's corner radius `ir`.
 *
 * Rectangular rather than square because the intake pane is 336x252 and
 * the two face panels are 300x300, and all three have to look like the
 * same component - that is the whole reason this is one function. */
static void aa_border(int ix, int iy, int iw, int ih_, int ir, int t,
                       uint16_t colour)
{
    volatile uint16_t *fb = (volatile uint16_t *)BUFFER_ADDRESS;

    const int   ox = ix - t, oy = iy - t;
    const int   ow = iw + (2 * t), oht = ih_ + (2 * t);
    const float ocx = (float)ox + (float)ow * 0.5f;
    const float ocy = (float)oy + (float)oht * 0.5f;
    const float ohx = (float)ow * 0.5f;
    const float ohy = (float)oht * 0.5f;
    const float orad = (float)(ir + t);

    const float icx = (float)ix + (float)iw * 0.5f;
    const float icy = (float)iy + (float)ih_ * 0.5f;
    const float ihx = (float)iw * 0.5f;
    const float ihy = (float)ih_ * 0.5f;
    const float irad = (float)ir;

    const int corner = ir + t;   /* the box each corner arc lives in */

    /* The straight runs, exact and cheap. */
    gui_draw_rect((uint16_t)(ox + corner), (uint16_t)oy,
                  (uint16_t)(ow - (2 * corner)), (uint16_t)t, colour);
    gui_draw_rect((uint16_t)(ox + corner), (uint16_t)(oy + oht - t),
                  (uint16_t)(ow - (2 * corner)), (uint16_t)t, colour);
    gui_draw_rect((uint16_t)ox, (uint16_t)(oy + corner),
                  (uint16_t)t, (uint16_t)(oht - (2 * corner)), colour);
    gui_draw_rect((uint16_t)(ox + ow - t), (uint16_t)(oy + corner),
                  (uint16_t)t, (uint16_t)(oht - (2 * corner)), colour);

    /* The four arcs, supersampled. */
    for (int c = 0; c < 4; c++) {
        const int bx = (c & 1) ? (ox + ow - corner) : ox;
        const int by = (c & 2) ? (oy + oht - corner) : oy;
        for (int y = 0; y < corner; y++) {
            for (int x = 0; x < corner; x++) {
                int hits = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        const float px = (float)(bx + x) + ((float)sx + 0.5f) * 0.25f;
                        const float py = (float)(by + y) + ((float)sy + 0.5f) * 0.25f;
                        if ((rr_sdf(px, py, ocx, ocy, ohx, ohy, orad) <= 0.0f) &&
                            (rr_sdf(px, py, icx, icy, ihx, ihy, irad) > 0.0f)) {
                            hits++;
                        }
                    }
                }
                if (hits == 0) {
                    continue;
                }
                volatile uint16_t *d =
                    &fb[((uint32_t)(by + y) * FRAME_WIDTH) + (uint32_t)(bx + x)];
                *d = (hits >= 16) ? colour
                                  : blend565(*d, colour, (hits * 256) / 16);
            }
        }
    }
}

/* Map a rect from crop space into the capture panel. */static void cap_map(int sx, int sy, int sw, int sh, int src,
                     uint16_t *ox, uint16_t *oy, uint16_t *ow, uint16_t *oh)
{
    if (src <= 0) src = 1;
    int x0 = CAP_X + ((sx * CAP_SIZE) / src);
    int y0 = CAP_Y + ((sy * CAP_SIZE) / src);
    int w  = (sw * CAP_SIZE) / src;
    int h  = (sh * CAP_SIZE) / src;
    if (w < 3) w = 3;
    if (h < 3) h = 3;
    if (x0 < CAP_X) x0 = CAP_X;
    if (y0 < CAP_Y) y0 = CAP_Y;
    if ((x0 + w) > (CAP_X + CAP_SIZE)) w = (CAP_X + CAP_SIZE) - x0;
    if ((y0 + h) > (CAP_Y + CAP_SIZE)) h = (CAP_Y + CAP_SIZE) - y0;
    *ox = (uint16_t)x0; *oy = (uint16_t)y0;
    *ow = (uint16_t)w;  *oh = (uint16_t)h;
}

/* A landmark cross, big enough to see from arm's length. */
static void cap_marker(int px, int py, uint16_t colour)
{
    if ((px < CAP_X + 5) || (px > CAP_X + CAP_SIZE - 6)) return;
    if ((py < CAP_Y + 5) || (py > CAP_Y + CAP_SIZE - 6)) return;
    gui_draw_rect((uint16_t)(px - 5), (uint16_t)(py - 1), 11, 3, colour);
    gui_draw_rect((uint16_t)(px - 1), (uint16_t)(py - 5), 3, 11, colour);
}

/* A title bar whose font shrinks rather than running off the screen.
 *
 * PATIENT_NAME_MAX is 32, and these titles greet the patient BY NAME, so a
 * real enrolment can produce a string well past what ui_font_lg fits across
 * 800 px. gui_draw_title_bar() hardcodes ui_font_lg and would simply have run
 * a long name off both edges - centred, so it would lose the beginning AND
 * the end. Measuring first costs one call. */
static void fitted_title(const char *title, uint16_t accent)
{
    if ((title == NULL) || (*title == '\0')) {
        return;
    }
    const ui_font_t *f = &ui_font_lg;
    if (gui_font_width(f, title) > 720u) {
        f = &ui_font_sm;
    }
    gui_font_text_centered(400u, TITLE_Y, title, accent, f);
    gui_fill_round_rect((800u - 140u) / 2u, (uint16_t)(TITLE_Y + TITLE_H + 4u),
                        140u, 5u, 2u, accent);
}

bool ai_overlay_draw_capture(const char *title)
{
    ai_capture_view_t v;
    if (!ai_vision_get_capture_view(&v) || (v.frame == NULL) ||
        (v.frame_size <= 0)) {
        return false;
    }

    /* Repaint EVERYTHING first - see the comment on CAP_SIZE above. This one
     * call is the difference between a result screen and a screenful of
     * tensors with a photograph in the corner. */
    gui_draw_frame();
    fitted_title((title != NULL) ? title : "WHAT THE CAMERA SAW",
                 THEME_GREEN_EDGE);

    /* The crop the NPU actually ran on - not a fresh camera frame. That is the
     * point: these are the exact pixels the model was given, so the box and
     * the landmarks line up with them by construction rather than by luck.
     *
     * Written straight through a framebuffer pointer. The earlier version
     * called gui_draw_rect() once per pixel to place a single pixel, 130k
     * times; the same mistake, and the same fix, as the live pane. */
    {
        volatile uint16_t *fb = (volatile uint16_t *)BUFFER_ADDRESS;
        const int src = v.frame_size;
        for (int y = 0; y < CAP_SIZE; y++) {
            const uint16_t *srow = &v.frame[((y * src) / CAP_SIZE) * src];
            volatile uint16_t *dst =
                &fb[((uint32_t)(CAP_Y + y) * FRAME_WIDTH) + CAP_X];
            /* Round the corners exactly as the live view does. Without
             * this the STILL image kept its square corners while the
             * border around it curved, so four sharp points poked out
             * from under the frame - most obvious at the bottom, where
             * there is nothing else for the eye to look at. */
            const int dy  = (y < (CAP_SIZE - 1 - y)) ? y : (CAP_SIZE - 1 - y);
            const int ins = corner_inset(dy);
            for (int x = ins; x < (CAP_SIZE - ins); x++) {
                dst[x] = srow[(x * src) / CAP_SIZE];
            }
        }
    }

    uint16_t bx, by, bw, bh;
    cap_map(v.box_x, v.box_y, v.box_w, v.box_h, v.frame_size,
            &bx, &by, &bw, &bh);
    stroke_rect(bx, by, bw, bh, THEME_GREEN_EDGE, 3);

    /* Landmarks: eyes and nose in amber, the two MOUTH corners in rose,
     * because the mouth pair is the half this project actually consumes. */
    for (int j = 0; j < FACE_LANDMARK_COUNT; j++) {
        const int lx = CAP_X + ((v.lm.x[j] * CAP_SIZE) / v.frame_size);
        const int ly = CAP_Y + ((v.lm.y[j] * CAP_SIZE) / v.frame_size);
        cap_marker(lx, ly, (j >= 3) ? THEME_ROSE_EDGE : THEME_AMBER_EDGE);
    }

    /* The same curvy border the live preview uses, so the still and the
     * moving view are visibly the same object at two moments. */
    aa_border(CAP_X, CAP_Y, CAP_SIZE, CAP_SIZE, FV_RADIUS, FV_BORDER,
              THEME_GREEN_EDGE);

    {
        char line[48];
        const int c = (int)(v.confidence * 100.0f);
        snprintf(line, sizeof(line), "FACE FOUND  %d%% CONFIDENT", c);
        gui_font_text_centered(CAP_CAPTION_CX, CAP_CAPTION_Y,
                               line, THEME_INK, CAP_CAPTION_F);
    }

    gui_draw_button(CAPRES_BTN_X, CAPRES_BTN_Y, CAPRES_BTN_W, CAPRES_BTN_H,
                    THEME_GREEN_EDGE, "NEXT", "");

    gui_draw_flush_rows(0u, 480u);
    return true;
}
/* Second pass: this used to draw the result and then block the UI
 * task in osal_delay_ms() for 1.8 s.
 *
 * That was wrong twice over. A viewer cannot study a photograph in 1.8 s, and
 * lengthening the dwell would make every enrolment slower for everyone
 * including people who do not care. Worse, a blocking dwell means the screen
 * is UNRESPONSIVE for its whole duration - USER1 does nothing, a tap does
 * nothing - which on a device for elderly users reads as a freeze.
 *
 * The dwell is now STATE_CAPTURE_RESULT in state_machine.c, which draws this
 * screen and waits for the NEXT button. `ms` is kept in the signature and
 * ignored so the two call sites stay readable as "show the result"; the
 * parameter is documented as advisory in ai_overlay.h. */
bool ai_overlay_show_capture(const char *title, uint32_t ms)
{
    (void)ms;
    return ai_overlay_draw_capture(title);
}
static const char *state_word(intake_state_t s)
{
    switch (s) {
        case INTAKE_SEARCHING:              return "SEARCHING";
        case INTAKE_LOCKED:                 return "LOCKED";
        case INTAKE_APPROACHING:            return "APPROACHING";
        case INTAKE_AT_MOUTH:               return "AT MOUTH";
        case INTAKE_RETREATING_CHECK:       return "VERIFYING";
        case INTAKE_CONSUMED:               return "CONSUMED";
        case INTAKE_ALL_CONSUMED:           return "ALL TAKEN";
        case INTAKE_NOT_CONSUMED_DROPPED:   return "DROPPED";
        case INTAKE_NOT_CONSUMED_RETREATED: return "WITHDRAWN";
        default:                            return "UNCERTAIN";
    }
}

bool ai_overlay_tick(void)
{
    if (!intake_is_active()) {
        s_n_inactive++;
        return false;
    }
    const uint32_t now = HAL_GetTick();
    if (s_frame_drawn && ((now - s_last_draw) < PV_PERIOD_MS)) {
        return false;
    }
    s_last_draw = now;

    const intake_view_t *v = intake_view();
    if (!v->valid) {
        s_n_invalid++;
        return false;
    }

    /* ── READ PSRAM SEQUENTIALLY, WRITE THE FRAMEBUFFER DIRECTLY ─────────
     *
     * The first version of this did neither, and idle collapsed from ~86% to
     * 33% during a watch - which would have quietly destroyed the 89.6% power
     * figure this project reports. Two causes, both about access patterns
     * rather than about the amount of data:
     *
     *  1. It sampled PSRAM pixel by pixel with a nearest-neighbour stride.
     *     PSRAM is THROUGHPUT=MID LATENCY=HIGH (the technical referenceb); scattered
     *     single-halfword reads are close to the worst thing you can do to it.
     *     Now each source row is copied ONCE, sequentially, into a small
     *     line buffer, and the subsampling happens in fast internal RAM.
     *
     *  2. It called gui_draw_rect() once per pixel - 54,000 function calls
     *     with bounds checks, to place single pixels. Now it writes the
     *     framebuffer directly through one pointer.
     *
     * Also: only the rows actually read are invalidated, not the whole
     * 768,000-byte frame. */
    {
        static uint16_t line[INTAKE_FRAME_W];   /* one source row, in AXISRAM */
        volatile uint16_t *fb = (volatile uint16_t *)BUFFER_ADDRESS;
        const uint16_t *frame = (const uint16_t *)INTAKE_FRAME_ADDR;
        int last_sy = -1;

        /* Round the pane's corners exactly as the two face panels do.
         * These three screens show the same thing - a camera, framed -
         * at three moments of the same flow, and they are supposed to
         * read as one component. */
        for (int y = 0; y < PV_H; y += PV_ROW_STEP) {
            const int edge_y = (y < (PV_H - 1 - y)) ? y : (PV_H - 1 - y);
            const int cin    = corner_inset_r(edge_y, FV_RADIUS);
            const int sy = PV_SRC_Y + ((y * PV_SRC) / PV_H);
            if (sy != last_sy) {
                const uint32_t row_addr =
                    (uint32_t)&frame[(uint32_t)sy * INTAKE_FRAME_W];
                SCB_InvalidateDCache_by_Addr((uint32_t *)(row_addr & ~31u),
                                              (INTAKE_FRAME_W * 2) + 64);
                memcpy(line, &frame[(uint32_t)sy * INTAKE_FRAME_W],
                       INTAKE_FRAME_W * sizeof(uint16_t));
                last_sy = sy;
            }
            volatile uint16_t *dst = &fb[((uint32_t)(PV_Y + y) * FRAME_WIDTH) + PV_X];
            for (int x = cin; x < (PV_W - cin); x++) {
                dst[x] = line[PV_SRC_X + ((x * PV_SRC) / PV_W)];
            }
            /* Repeat the row rather than fetching another one. The copy is
             * AXISRAM-to-AXISRAM, which is free next to the PSRAM read it
             * replaces. */
            for (int r = 1; (r < PV_ROW_STEP) && ((y + r) < PV_H); r++) {
                volatile uint16_t *rep =
                    &fb[((uint32_t)(PV_Y + y + r) * FRAME_WIDTH) + PV_X];
                for (int x = cin; x < (PV_W - cin); x++) {
                    rep[x] = dst[x];
                }
            }
        }
    }

    uint16_t rx, ry, rw, rh;

    /* The region the detector actually searched - amber. Seeing this is what
     * would have made the field-of-view mistake in Addendum 8 obvious in one
     * glance instead of two hardware rounds. */
    if (v->roi_size > 0) {
        map_rect(v->roi_x - PV_SRC_X, v->roi_y - PV_SRC_Y,
                 v->roi_size, v->roi_size,
                 PV_SRC, PV_SRC, &rx, &ry, &rw, &rh);
        stroke_rect(rx, ry, rw, rh, THEME_AMBER_EDGE, 1);
    }

    /* The mouth, from the CenterFace landmarks - rose. */
    if (v->mouth.detected) {
        marker(PV_X + (((v->mouth.cx - PV_SRC_X) * PV_W) / PV_SRC),
               PV_Y + (((v->mouth.cy - PV_SRC_Y) * PV_H) / PV_SRC),
               THEME_ROSE_EDGE);
    }

    /* The object the state machine is actually following - green, thicker,
     * because it is the thing a viewer is looking for. Since Stage 1C it
     * is usually the HAND rather than the pill, and the caption below says
     * which, so that what is on the screen and what is in the log agree
     * about the same frame. */
    if (v->pill.detected) {
        map_rect(v->pill.x - PV_SRC_X, v->pill.y - PV_SRC_Y,
                 v->pill.w, v->pill.h,
                 PV_SRC, PV_SRC, &rx, &ry, &rw, &rh);
        stroke_rect(rx, ry, rw, rh, THEME_GREEN_EDGE, 2);
    }

    /* The border, once. It has to go on AFTER the first blit so its inner
     * edge blends against the image, and it must NOT be redrawn every
     * tick: the blit skips the corner pixels precisely so the blend
     * survives, and repainting would only cost time. */
    if (!s_frame_drawn) {
        aa_border(PV_X, PV_Y, PV_W, PV_H, FV_RADIUS, FV_BORDER,
                  THEME_GREEN_EDGE);
    }

    {
        char line[48];
        snprintf(line, sizeof(line), "%s  %s  %lu/%lu",
                 state_word(v->state),
                 v->tracked_is_hand ? "HAND" : "PILL",
                 (unsigned long)v->pill_frames, (unsigned long)v->frames);
        /* The same slot, the same font, as the two face screens: bottom
         * centre of the SCREEN, clear of the corner motifs. */
        gui_draw_rect(120u, (uint16_t)(CAP_CAPTION_Y - 2), 560u, 32u,
                      THEME_BG);
        gui_font_text_centered(CAP_CAPTION_CX, CAP_CAPTION_Y,
                               line, THEME_INK_SOFT, CAP_CAPTION_F);
    }

    gui_draw_flush_rows((uint16_t)(PV_Y - FV_BORDER),
                        (uint16_t)(CAP_CAPTION_Y + 32));
    s_frame_drawn = true;
    s_n_drawn++;
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 * THE LIVE FACE PREVIEW
 * ══════════════════════════════════════════════════════════════════════
 *
 * Until now the seconds before a face capture were a RAW CAMERA DUMP: the
 * DCMIPP wrote all 800x480 of the framebuffer continuously, so no chrome, no
 * title and no text could survive a single frame. The device went from a
 * designed screen to an undesigned one and back, and nothing could be said to
 * the user in between - which is exactly the moment they most need telling
 * what to do.
 *
 * The fix is the trick already proved for the intake watch: point
 * the camera at PSRAM instead of at the screen. The UI then owns
 * BUFFER_ADDRESS outright and can draw a real screen - frame chrome, a
 * greeting that uses the patient's name, a rounded viewport - and blit the
 * live image INTO the viewport as one element of a composed page.
 *
 * -- WHAT THE VIEWPORT SHOWS, AND WHY IT IS SQUARE ------------------------
 *
 * ai_vision_run_pipeline() does not analyse the whole frame. It crops the
 * centred CROP_SIZE x CROP_SIZE square out of BUFFER_ADDRESS and hands the
 * detector that. So the viewport deliberately shows THAT SQUARE and nothing
 * else: what the user sees framed is precisely what the model will be given.
 * A wide preview would let someone centre themselves perfectly in a picture
 * whose edges the NPU never receives.
 *
 * -- THE HANDOVER BACK, WHICH IS THE ONE FRAGILE PART ---------------------
 *
 * The capture reads BUFFER_ADDRESS. While the preview runs, the camera is
 * NOT writing there. So preview_end() must repoint the pipe and then the
 * caller must let at least one whole frame land before requesting a capture,
 * or the NPU is handed the stale contents of a screen. That settle window is
 * PREVIEW_SETTLE_MS in state_machine.c, and it is not optional.
 */


static bool     s_pv_open;
static uint32_t s_pv_last;

void ai_overlay_preview_begin(const char *greeting, const char *hint,
                               uint16_t accent)
{
    /* Compose the page ONCE. Only the viewport is repainted per tick, so the
     * text and the chrome cost nothing after this call. */
    gui_draw_frame();
    fitted_title(greeting, accent);
    if (hint && *hint) {
        gui_font_text_centered(CAP_CAPTION_CX, CAP_CAPTION_Y, hint,
                               THEME_INK_SOFT, CAP_CAPTION_F);
    }

    /* The curvy border the viewport sits inside. Stroked OUTSIDE the image
     * area so it never eats a row of camera. */
    aa_border(FV_X, FV_Y, FV_SIZE, FV_SIZE, FV_RADIUS, FV_BORDER, accent);

    /* NEXT, drawn but NOT yet available: THEME_INK_SOFT as the edge gives
     * a grey outline on the page colour (fill_for_edge maps it to
     * THEME_BG), against the saturated green fill the same button gets on
     * the result screen. It occupies its final position from the first
     * frame, so the transition is a colour change and not a button
     * appearing out of nowhere under the user's thumb. */
    gui_draw_button(CAPRES_BTN_X, CAPRES_BTN_Y, CAPRES_BTN_W, CAPRES_BTN_H,
                    THEME_INK_SOFT, "NEXT", "");

    gui_draw_flush_rows(0u, 480u);

    s_pv_open = intake_camera_start();
    s_pv_last = 0u;
    if (!s_pv_open) {
        /* The camera would not repoint. Say so on the screen rather than
         * leaving a viewport that never fills - a blank hole reads as a dead
         * device, and the capture itself may still succeed. */
        gui_font_text_centered(400u, (uint16_t)(FV_Y + (FV_SIZE / 2)),
                               "PREPARING CAMERA...", THEME_INK_SOFT,
                               &ui_font_sm);
        gui_draw_flush_rows((uint16_t)(FV_Y + (FV_SIZE / 2) - 4),
                            (uint16_t)(FV_Y + (FV_SIZE / 2) + 30));
    }
}

bool ai_overlay_preview_tick(void)
{
    if (!s_pv_open) {
        return false;
    }
    const uint32_t now = HAL_GetTick();
    if ((s_pv_last != 0u) && ((now - s_pv_last) < FV_PERIOD_MS)) {
        return false;
    }
    s_pv_last = now;

    /* The centred square the detector will actually be given. */
    const int src   = (INTAKE_FRAME_H < INTAKE_FRAME_W) ? INTAKE_FRAME_H
                                                        : INTAKE_FRAME_W;
    const int src_x = (INTAKE_FRAME_W - src) / 2;
    const int src_y = (INTAKE_FRAME_H - src) / 2;

    static uint16_t line[INTAKE_FRAME_W];
    volatile uint16_t *fb = (volatile uint16_t *)BUFFER_ADDRESS;
    const uint16_t *frame = (const uint16_t *)INTAKE_FRAME_ADDR;
    int last_sy = -1;

    for (int y = 0; y < FV_SIZE; y++) {
        const int sy = src_y + ((y * src) / FV_SIZE);
        if (sy != last_sy) {
            const uint32_t row_addr =
                (uint32_t)&frame[(uint32_t)sy * INTAKE_FRAME_W];
            SCB_InvalidateDCache_by_Addr((uint32_t *)(row_addr & ~31u),
                                          (INTAKE_FRAME_W * 2) + 64);
            memcpy(line, &frame[(uint32_t)sy * INTAKE_FRAME_W],
                   INTAKE_FRAME_W * sizeof(uint16_t));
            last_sy = sy;
        }

        /* Round the corners by painting the page colour back over them.
         * Cheaper and more exact than clipping the blit, and it means the
         * curve matches gui_stroke_round_rect's by construction. */
        const int dy  = (y < (FV_SIZE - 1 - y)) ? y : (FV_SIZE - 1 - y);
        const int ins = corner_inset(dy);

        volatile uint16_t *dst = &fb[((uint32_t)(FV_Y + y) * FRAME_WIDTH) + FV_X];
        /* SKIP the corner pixels rather than painting them THEME_BG.
         * aa_border() blended its inner edge into exactly those pixels
         * when the screen was composed; repainting them flat every tick
         * would scrub the anti-aliasing off four times a second and put
         * the staircase straight back. Nothing else writes there, so
         * leaving them alone is correct as well as cheaper. */
        for (int x = ins; x < (FV_SIZE - ins); x++) {
            dst[x] = line[src_x + ((x * src) / FV_SIZE)];
        }
    }

    gui_draw_flush_rows(FV_Y, (uint16_t)(FV_Y + FV_SIZE));
    return true;
}

void ai_overlay_preview_end(void)
{
    if (s_pv_open) {
        intake_camera_stop();
        s_pv_open = false;
    }
}

#else  /* !MEDSIGHT_AI_OVERLAY */

bool ai_overlay_draw_capture(const char *t)        { (void)t; return false; }
bool ai_overlay_show_capture(const char *t, uint32_t ms)
{ (void)t; (void)ms; return false; }
bool ai_overlay_tick(void)                { return false; }
void ai_overlay_reset(void)        { }
void ai_overlay_preview_begin(const char *g, const char *h, uint16_t a)
{ (void)g; (void)h; (void)a; }
bool ai_overlay_preview_tick(void)         { return false; }
void ai_overlay_preview_end(void)          { }
void ai_overlay_stats(uint32_t *d, uint32_t *a, uint32_t *b)
{ if (d) *d = 0u; if (a) *a = 0u; if (b) *b = 0u; }

#endif /* MEDSIGHT_AI_OVERLAY */
