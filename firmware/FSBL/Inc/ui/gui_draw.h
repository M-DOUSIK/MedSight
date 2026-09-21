#ifndef GUI_DRAW_H
#define GUI_DRAW_H

#include <stdint.h>
#include <stdbool.h>
#include "ui/ui_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * MedSight design system
 *
 * The visual language comes from the project designer's own reference
 * artwork (tools/mascot_and_frames_design/): a white page inside a thick
 * blush-pink rounded card frame, a heart and a pill-capsule motif in
 * opposite corners, bold italic type, soft rounded buttons, and the Lumio
 * mascot as real artwork rather than procedural shapes.
 *
 * Colour rules, so this stays a system rather than a pile of hex:
 *   - Text is always THEME_INK (17:1 on white) or THEME_INK_SOFT (6.4:1).
 *     Nothing else is ever used for body text.
 *   - Buttons are a LIGHT fill with a SATURATED edge and INK text. This is
 *     what keeps contrast high measured the old white-on-pale
 *     buttons at 1.8-2.1:1, i.e. unreadable.
 *   - The blush frame and the corner motifs are decoration only; no text is
 *     ever placed on them, so they carry no contrast requirement.
 *   - Accent colour carries meaning: rose = register/neutral, green =
 *     dispense/success, amber = warning, red = alert.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define THEME_BG           0xFFFF   /* white page                            */
#define THEME_INK          0x18C3   /* near-black text        (~17:1 on white)*/
#define THEME_INK_SOFT     0x5AEB   /* secondary grey text    (~6.4:1)        */
#define THEME_FRAME        0xFF1C   /* blush card border (sampled from art)   */

#define THEME_ROSE_FILL    0xFF3C   /* light rose button fill                 */
#define THEME_ROSE_EDGE    0xB1CB   /* deep rose edge    (5.8:1 vs white)     */
#define THEME_GREEN_FILL   0xE79C   /* light mint fill                        */
#define THEME_GREEN_EDGE   0x0326   /* deep green edge   (7.2:1 vs white)     */
#define THEME_AMBER_FILL   0xFEF0   /* light amber fill                       */
#define THEME_AMBER_EDGE   0xC240   /* amber edge        (4.9:1 vs white)     */
#define THEME_RED_FILL     0xFDD5   /* light red fill                         */
#define THEME_RED_EDGE     0xB8E3   /* deep red edge     (6.6:1 vs white)     */

/* Accent selector passed to title bars / screens: meaning, not decoration. */
#define ACCENT_NEUTRAL     THEME_ROSE_EDGE
#define ACCENT_DISPENSE    THEME_GREEN_EDGE
#define ACCENT_WARN        THEME_AMBER_EDGE
#define ACCENT_ALERT       THEME_RED_EDGE
#define ACCENT_SUCCESS     THEME_GREEN_EDGE

/* ── Legacy colour names, kept so older call sites still compile ────────── */
#define COLOR_BG           THEME_BG
#define COLOR_WHITE        0xFFFF
#define COLOR_BLACK        0x0000
#define COLOR_TITLE        THEME_INK
#define COLOR_DLG_TEXT     THEME_INK
#define COLOR_DLG_BG       THEME_BG
#define COLOR_DLG_BORDER   THEME_FRAME
#define COLOR_BTN_BORDER   0xFFFF
#define COLOR_BTN_SHADOW   THEME_INK_SOFT
#define COLOR_GRAY         THEME_INK_SOFT
#define COLOR_BTN_REG      THEME_ROSE_EDGE
#define COLOR_BTN_DIS      THEME_GREEN_EDGE
#define COLOR_BTN_READY    THEME_ROSE_EDGE
#define COLOR_BTN_GREEN    THEME_GREEN_EDGE
#define COLOR_BTN_PROGRESS THEME_GREEN_EDGE
#define COLOR_ALERT        THEME_RED_EDGE
#define COLOR_WARN         THEME_AMBER_EDGE

/* ── Pill lentils ("gems") ──────────────────────────────────────────────── */
/* The dose is drawn as sugar-shell chocolate lentils rather than a clinical
 * capsule: rounder, friendlier, and instantly readable as "one of these is
 * one pill" at arm's length. Colour is IDENTITY here, not decoration - each
 * colour stands for one hopper, so the same colour means the same medication
 * everywhere it appears.
 *
 * The prototype has one hopper, so only GEM_RED is ever live; the rest are
 * drawn in GEM_OFF (grey) as visible, deliberately disabled slots. That is
 * the honest way to show a one-hopper device that the mechanical design
 * (the hardware specification) always intended to scale - and it leaves the screen
 * ready for a carer mode to enable them without a redesign. */
#define GEM_RED     0xE205   /* hopper A - the only one this build dispenses */
#define GEM_ORANGE  0xF486   /* hopper B - reserved                          */
#define GEM_YELLOW  0xF5E6   /* hopper C - reserved                          */
#define GEM_GREEN   0x4D2A   /* hopper D - reserved                          */
#define GEM_BLUE    0x2B7C   /* spare                                        */
#define GEM_BROWN   0x7AA9   /* spare                                        */
#define GEM_OFF     0xCE5A   /* disabled slot                                */

/* ── Card frame (800 x 480) ─────────────────────────────────────────────── */
#define FRAME_INSET     6u
#define FRAME_THICK    10u
#define FRAME_RADIUS   22u
#define MOTIF_MARGIN   26u    /* corner motifs, inset from the screen edge   */

/* Content area inside the frame. */
#define CONTENT_X      30u
#define CONTENT_Y      30u
#define CONTENT_W     740u
#define CONTENT_H     420u

/* Title line box (top edge); text is centred with measured widths. */
#define TITLE_Y        38u
#define TITLE_H        44u

/* ── Vertical rhythm below the title ────────────────────────
 * gui_draw_title_bar() draws the title at TITLE_Y in ui_font_lg and puts a
 * 5 px accent rule at TITLE_Y + TITLE_H + 4, so the header block ENDS at
 * y = 91. Every screen that puts a line of subtext under the title was
 * choosing its own y by eye, and two of them collided on hardware - the
 * dose-times screen drew its instruction at 96 (22 px tall) straight
 * through the clock strip at 108.
 *
 * These are the two slots. Use them instead of a literal. */
#define HEADER_BOTTOM  91u    /* last row the title block occupies          */
#define SUBTEXT_Y     100u    /* one line of ui_font_sm: 100..122           */
#define STATUS_Y      126u    /* a second line under it:   126..148         */

/* ── Home screen ────────────────────────────────────────────────────────── */
#define REG_BTN_X      44u
#define REG_BTN_Y     148u
#define REG_BTN_W     452u
#define REG_BTN_H     104u

#define DISP_BTN_X     44u
#define DISP_BTN_Y    266u
#define DISP_BTN_W    452u
#define DISP_BTN_H    104u

/* Mascot artwork sits to the right of the home/instruction buttons. */
#define MASCOT_BG_X   536u
#define MASCOT_BG_Y   136u
#define MASCOT_BG_W   214u
#define MASCOT_BG_H   236u

/* Bottom message panel (Lumio's greeting, SD warning, transient messages). */
#define DLG_BOX_X      90u
#define DLG_BOX_Y     382u
#define DLG_BOX_W     620u
#define DLG_BOX_H      68u

/* ── Instruction screens (READY) ────────────────────────────────────────── */
#define READY_BTN_X    44u
#define READY_BTN_Y   176u
#define READY_BTN_W   452u
#define READY_BTN_H   150u

/* ── Dispense flow ──────────────────────────────────────────────────────── */
/* The dose drawn as gems, above the bar. MAX_W is sized for the widest row
 * PILLCOUNT_MAX allows (10 gems at 46 px pitch), because the row is cleared
 * and redrawn on every progress step. */
#define DOSE_GEM_Y      230u
#define DOSE_GEM_H       50u
#define DOSE_GEM_CX     400u
#define DOSE_GEM_MAX_W  480u

#define DISPENSE_BAR_X  120u
#define DISPENSE_BAR_Y  296u
#define DISPENSE_BAR_W  560u
#define DISPENSE_BAR_H   54u
#define PCT_TEXT_Y      364u

/* The confirm screen became a SPLIT layout - a live camera view
 * with the AI overlay on the left, the confirming action on the right - so
 * that a viewer can watch the model track the pill while the patient uses the
 * button. The button shrank from 608x196 to 330x200: still a very large touch
 * target (the QWERTY keys this project calls its smallest are 70x56), and
 * still the single most prominent control on the screen, which is what the
 * elderly-friendly goal actually requires. */
#define TAKEN_BTN_X     424u
#define TAKEN_BTN_Y     170u
#define TAKEN_BTN_W     330u
#define TAKEN_BTN_H     200u

/* The camera preview pane on the same screen. ai_overlay.c draws here. */
/* ── The capture RESULT screen ─────────────────────────────
 *
 * Shown after a face capture: the frozen crop the NPU actually ran on, its
 * box and landmarks on the left, and a NEXT button on the right. It used to
 * be a 1.8 s automatic dwell, which is not long enough to study a picture and
 * cannot be extended without making every enrolment slower for everyone. A
 * button lets the viewer take the time they want and no more.
 *
 * The split mirrors the confirm screen's, so the two AI screens in the
 * product read as the same kind of thing. */
#define CAPRES_IMG_X     56u
#define CAPRES_IMG_Y    110u
#define CAPRES_IMG_SZ   300u
#define CAPRES_BTN_X    430u
#define CAPRES_BTN_Y    180u
#define CAPRES_BTN_W    310u
#define CAPRES_BTN_H    150u
/* The live intake pane is THE SAME RECTANGLE as the two face panels.
 *
 * It used to be 336x252 at (46,140) - a different size, a different aspect
 * and a different position from the capture screens, which made the three
 * camera views in this product look like three unrelated components rather
 * than one thing appearing at three moments of the same flow.
 *
 * Defined in terms of CAPRES_* rather than repeated, so they cannot drift.
 * The confirm screen's button lives at TAKEN_BTN_X 424 and this pane ends at
 * 56 + 300 = 356, so the split still clears with room to spare. */
#define CONFIRM_PV_X     CAPRES_IMG_X
#define CONFIRM_PV_Y     CAPRES_IMG_Y
#define CONFIRM_PV_W     CAPRES_IMG_SZ
#define CONFIRM_PV_H     CAPRES_IMG_SZ

#define SKIP_BTN_X      596u
#define SKIP_BTN_Y       92u
#define SKIP_BTN_W      156u
#define SKIP_BTN_H       58u

/* ── Two-choice / alert screens ─────────────────────────────────────────── */
/* The sad mascot's box on the two-choice screen. anime_ui animates the three
 * MASCOT_ERROR frames inside exactly this rectangle, so the layout and the
 * animation agree on one definition. */
#define MASCOT_SAD_X    56u
#define MASCOT_SAD_Y   112u
#define MASCOT_SAD_W   150u
#define MASCOT_SAD_H   158u
/* Messages on the two-choice screen centre to the right of the mascot. */
#define CHOICE_MSG_CX  500u

#define CHOICE_BTN_Y    296u
#define CHOICE_BTN_W    300u
#define CHOICE_BTN_H    124u
#define CHOICE_BTN_GAP   40u
#define CHOICE_LEFT_X   ((800u - (2u * CHOICE_BTN_W + CHOICE_BTN_GAP)) / 2u)
#define CHOICE_RIGHT_X  (CHOICE_LEFT_X + CHOICE_BTN_W + CHOICE_BTN_GAP)

#define ALERT_BTN_X     250u
#define ALERT_BTN_Y     318u
#define ALERT_BTN_W     300u
#define ALERT_BTN_H     112u

/* ═══════════════════════════════════════════════════════════════════════════
 * Core primitives
 * ═══════════════════════════════════════════════════════════════════════════ */
void gui_draw_init(uint32_t buffer_address, uint16_t width, uint16_t height);
void gui_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* Rounded rectangles - the shape language for every panel and button. */
void gui_fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t r, uint16_t color);
void gui_stroke_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t r, uint16_t t, uint16_t color);

/* 4bpp palettised sprite blit; palette index 0 is transparent. */
void gui_blit_sprite(uint16_t x, uint16_t y, const ui_sprite_t *s);

/* One pill lentil, centred on (cx, cy): anti-aliased disc in `fill`, a
 * darker rim derived from it, and a soft highlight up and to the left so it
 * reads as a glossy sugar shell rather than a flat dot. Drawn rather than
 * blitted because colour is a parameter - a sprite per hopper colour would
 * be six near-identical copies in ROM. */
void gui_draw_gem(uint16_t cx, uint16_t cy, uint16_t r, uint16_t fill);

/* ── Text (anti-aliased proportional fonts, ui_assets.h) ────────────────── */
uint16_t gui_font_width(const ui_font_t *f, const char *str);
/* y is the TOP of the line box. '\n' advances by the font's line height. */
void gui_font_text(uint16_t x, uint16_t y, const char *str,
                   uint16_t color, const ui_font_t *f);
void gui_font_text_centered_tracked(uint16_t cx, uint16_t y, const char *str,
                                    uint16_t color, const ui_font_t *f,
                                    uint8_t track);
void gui_font_text_centered(uint16_t cx, uint16_t y, const char *str,
                            uint16_t color, const ui_font_t *f);
/* Centred, but truncated with ".." if it will not fit in `avail` pixels.
 * Use this for anything whose length this UI does not control -
 * patient names above all, which are up to 31 characters and were running
 * out of their boxes on the carer screens. */
void gui_font_text_ellipsis(uint16_t cx, uint16_t y, const char *str,
                            uint16_t avail, uint16_t color, const ui_font_t *f);

/* The old 8x8 bitmap font, kept for anything that still wants a blocky
 * fixed-cell glyph. New UI code should use gui_font_* above. */
void gui_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint8_t scale);
void gui_draw_text_centered(uint16_t x_center, uint16_t y,
                            const char *str, uint16_t color, uint8_t scale);

/* ── Shared chrome ──────────────────────────────────────────────────────── */
/* The blush card border plus the four corner motifs. Every full-screen draw
 * starts with this, which is what makes the screens feel like one product. */
void gui_draw_frame(void);
/* Centred bold-italic screen title in `accent`, drawn under the frame. */
void gui_draw_title_bar(const char *title, uint16_t accent);
/* Rounded button: light fill, saturated edge, ink label. line2 may be "". */
void gui_draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t edge, const char *line1, const char *line2);

/* ═══════════════════════════════════════════════════════════════════════════
 * Screens
 * ═══════════════════════════════════════════════════════════════════════════ */
void gui_draw_home_screen(void);
void gui_draw_dialog_text(const char *text);
void gui_draw_ready_screen(const char *title, uint16_t accent, const char *dialog_msg);
void gui_draw_mascot_bg(void);   /* no-op; kept for API compatibility */

void gui_draw_dispensing_screen(const char *patient_name, uint8_t pill_count);
/* `percent` fills the bar; `pills_done` is how many of the dose have been
 * released, and lights exactly that many gems. Both come from the caller's
 * per-pill loop so the bar, the number and the gems can never disagree. */
void gui_draw_dispensing_progress(uint16_t percent, uint8_t pills_done);

void gui_draw_confirm_taken_screen(void);
void gui_draw_taken_thankyou_screen(void);

void gui_draw_two_choice_screen(const char *title, const char *message,
                                uint16_t accent,
                                const char *left_label, const char *right_label);
void gui_draw_alert_screen(const char *title, const char *message,
                           uint16_t accent, const char *button_label);
/* Note: there is no "checking" screen. With one framebuffer the NPU
 * corrupts anything drawn during inference, so state_machine.c blanks the
 * LTDC layer for that window instead - see its Part A note. */

/* Clean the D-Cache over the drawn framebuffer region so the LTDC (which
 * reads it directly, bypassing the CPU cache) actually sees what was drawn.
 * The screen functions above flush themselves; any other module drawing via
 * gui_draw_rect()/gui_font_text() must call one of these itself. */
void gui_draw_flush(void);
void gui_draw_flush_rows(uint16_t y_start, uint16_t y_end);

#ifdef __cplusplus
}
#endif

#endif /* GUI_DRAW_H */
