/* ui/registration_ui.c - patient registration UI flow
 * See registration_ui.h for the module-boundary rationale.
 *
 * Restyled onto the shared design system in gui_draw.h - the
 * card frame, corner motifs, anti-aliased type and rounded buttons every
 * other screen uses. These three screens previously had no title bar at all
 * (just raw 8x8 text at y=16) and drew their own plain-bordered buttons, so
 * they were the most obviously mismatched screens in the product.
 */
#include "ui/registration_ui.h"
#include "ui/gui_draw.h"
#include "ui/ui_assets.h"
#include "ai_vision.h"

#include <string.h>
#include <stdio.h>

/* Routine bookkeeping output is gated; fault reporting
 * is not. Same pattern as state_machine.c / sd_diskio.c. */
#ifndef MEDSIGHT_DEBUG
#define MEDSIGHT_DEBUG 0
#endif
#if MEDSIGHT_DEBUG
#define MS_DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define MS_DBG_PRINTF(...) do { } while (0)
#endif

/* ── Screen state (one registration attempt at a time) ──────────────────── */
static char    s_name[PATIENT_NAME_MAX];
static uint8_t s_name_len;
static uint8_t s_pill_count;
static int8_t  s_embedding[EMBEDDING_SIZE];

#define PILLCOUNT_MIN 1
#define PILLCOUNT_MAX 10

void registration_ui_reset(void)
{
    memset(s_name, 0, sizeof(s_name));
    s_name_len   = 0;
    s_pill_count = PILLCOUNT_MIN;
}

void registration_ui_set_embedding(const int8_t *embedding)
{
    memcpy(s_embedding, embedding, sizeof(s_embedding));
}

/* ── Shared hit-test helper (same pattern as state_machine.c's check_hit) ── */
static bool hit(uint32_t tx, uint32_t ty, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    return (tx >= x && tx <= (uint32_t)(x + w) && ty >= y && ty <= (uint32_t)(y + h));
}

/* ══════════════════════════════════════════════════════════════════════════
 * KEYBOARD screen
 * ══════════════════════════════════════════════════════════════════════════ */

/* Raised from 70x56. the technical reference.7 flagged those
 * as the smallest touch targets in the product, under the ~60px floor the
 * rest of the UI follows. All ten Row-1 keys still fit inside the card
 * frame: 10*70 + 9*5 = 745px of the 800px panel. */
#define KEY_W   70u
#define KEY_H   60u
#define KEY_GAP  5u

#define KB_ROW1_Y 172u
#define KB_ROW2_Y (KB_ROW1_Y + KEY_H + KEY_GAP)
#define KB_ROW3_Y (KB_ROW2_Y + KEY_H + KEY_GAP)
#define KB_ROW4_Y (KB_ROW3_Y + KEY_H + KEY_GAP)

#define NAME_BOX_X 110u
#define NAME_BOX_Y 100u
#define NAME_BOX_W 580u
#define NAME_BOX_H  56u

/* Row 4: SPACE / DEL / DONE, sharing row 3's centred span. */
#define R4_SPACE_W 250u
#define R4_DEL_W   125u
#define R4_DONE_W  140u
#define R4_TOTAL   (R4_SPACE_W + R4_DEL_W + R4_DONE_W + 2u * KEY_GAP)
#define R4_X       ((800u - R4_TOTAL) / 2u)
#define R4_DEL_X   (R4_X + R4_SPACE_W + KEY_GAP)
#define R4_DONE_X  (R4_DEL_X + R4_DEL_W + KEY_GAP)

static const char *KB_ROW1 = "QWERTYUIOP"; /* 10 keys */
static const char *KB_ROW2 = "ASDFGHJKL";  /* 9 keys  */
static const char *KB_ROW3 = "ZXCVBNM";    /* 7 keys  */

static uint16_t kb_row_start_x(int n_keys)
{
    uint16_t total = (uint16_t)(n_keys * KEY_W + (n_keys - 1) * KEY_GAP);
    return (uint16_t)((800u - total) / 2u);
}

static void draw_name_box(void)
{
    gui_fill_round_rect(NAME_BOX_X, NAME_BOX_Y, NAME_BOX_W, NAME_BOX_H, 14u, THEME_BG);
    gui_stroke_round_rect(NAME_BOX_X, NAME_BOX_Y, NAME_BOX_W, NAME_BOX_H, 14u, 4u,
                          THEME_ROSE_EDGE);

    char display[PATIENT_NAME_MAX + 2];
    snprintf(display, sizeof(display), "%s_", s_name);
    gui_font_text_centered((uint16_t)(NAME_BOX_X + NAME_BOX_W / 2u),
                           (uint16_t)(NAME_BOX_Y + (NAME_BOX_H - ui_font_lg.line_height) / 2u),
                           display, THEME_INK, &ui_font_lg);

    /* Called standalone on every keypress, so it flushes its own rows. */
    gui_draw_flush_rows(NAME_BOX_Y, (uint16_t)(NAME_BOX_Y + NAME_BOX_H));
}

const char *registration_ui_name(void)
{
    return s_name;
}

void registration_ui_draw_keyboard(void)
{
    gui_draw_frame();
    gui_draw_title_bar("ENTER PATIENT NAME", ACCENT_NEUTRAL);

    draw_name_box();

    char label[2] = {0, 0};
    struct { const char *row; int n; uint16_t y; } rows[3] = {
        { KB_ROW1, 10, KB_ROW1_Y }, { KB_ROW2, 9, KB_ROW2_Y }, { KB_ROW3, 7, KB_ROW3_Y }
    };
    for (int r = 0; r < 3; r++)
    {
        uint16_t x = kb_row_start_x(rows[r].n);
        for (int i = 0; i < rows[r].n; i++)
        {
            label[0] = rows[r].row[i];
            gui_draw_button((uint16_t)(x + i * (KEY_W + KEY_GAP)), rows[r].y,
                            KEY_W, KEY_H, THEME_ROSE_EDGE, label, "");
        }
    }

    gui_draw_button(R4_X,      KB_ROW4_Y, R4_SPACE_W, KEY_H, THEME_ROSE_EDGE,  "SPACE", "");
    gui_draw_button(R4_DEL_X,  KB_ROW4_Y, R4_DEL_W,   KEY_H, THEME_AMBER_EDGE, "DEL",   "");
    gui_draw_button(R4_DONE_X, KB_ROW4_Y, R4_DONE_W,  KEY_H, THEME_GREEN_EDGE, "DONE",  "");

    gui_draw_flush();
}

bool registration_ui_handle_keyboard_touch(uint32_t tx, uint32_t ty)
{
    struct { const char *row; int n; uint16_t y; } rows[3] = {
        { KB_ROW1, 10, KB_ROW1_Y }, { KB_ROW2, 9, KB_ROW2_Y }, { KB_ROW3, 7, KB_ROW3_Y }
    };
    for (int r = 0; r < 3; r++)
    {
        uint16_t x = kb_row_start_x(rows[r].n);
        for (int i = 0; i < rows[r].n; i++)
        {
            if (hit(tx, ty, (uint16_t)(x + i * (KEY_W + KEY_GAP)), rows[r].y, KEY_W, KEY_H))
            {
                if (s_name_len < PATIENT_NAME_MAX - 1)
                {
                    s_name[s_name_len++] = rows[r].row[i];
                    s_name[s_name_len]   = '\0';
                    draw_name_box();
                }
                return false;
            }
        }
    }

    if (hit(tx, ty, R4_X, KB_ROW4_Y, R4_SPACE_W, KEY_H))
    {
        if (s_name_len < PATIENT_NAME_MAX - 1)
        {
            s_name[s_name_len++] = ' ';
            s_name[s_name_len]   = '\0';
            draw_name_box();
        }
        return false;
    }
    if (hit(tx, ty, R4_DEL_X, KB_ROW4_Y, R4_DEL_W, KEY_H))
    {
        if (s_name_len > 0)
        {
            s_name[--s_name_len] = '\0';
            draw_name_box();
        }
        return false;
    }
    if (hit(tx, ty, R4_DONE_X, KB_ROW4_Y, R4_DONE_W, KEY_H))
    {
        /* Trim trailing spaces so an accidental space-only name can't pass
         * as "non-empty". */
        while (s_name_len > 0 && s_name[s_name_len - 1] == ' ')
            s_name[--s_name_len] = '\0';
        return (s_name_len > 0);
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PILL COUNT screen
 *
 * Design note (per user feedback after the first hardware test): no +/-
 * buttons - a tappable card that adds one pill per tap, plus a RESET button
 * to go back to the minimum in one tap rather than repeatedly tapping "-".
 *
 * Second pass: the single capsule card became a row of four
 * hopper slots drawn as coloured pill lentils. Slot A is live and is the
 * tap-to-add control; B, C and D are drawn greyed out and do not respond.
 *
 * That is a deliberate piece of honesty about what this prototype is. The
 * mechanical design (the hardware specification) has always been a multi-hopper
 * carousel, and the dispense record already stores a per-patient dose; what
 * is missing is three more hoppers and a carer-facing way to assign a
 * medication to each. Showing the empty slots states the shape of the
 * finished product without pretending the hardware exists - and when the
 * hoppers do arrive, the screen needs colours turned on, not a redesign.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Hopper slots. Only HOPPER_LIVE of them accept touch. */
#define HOPPER_COUNT    4u
#define HOPPER_LIVE     1u
#define HOPPER_W      170u
#define HOPPER_H       94u
#define HOPPER_GAP     16u
#define HOPPER_Y      124u
#define HOPPER_X0      36u    /* (800 - (4*170 + 3*16)) / 2 */

#define HOPPER_X(i)   ((uint16_t)(HOPPER_X0 + (i) * (HOPPER_W + HOPPER_GAP)))

/* Chip colours for a slot that is present but not yet fitted. */
#define SLOT_OFF_FILL  0xFFFF
#define SLOT_OFF_EDGE  0xBDD7

#define CAPTION_Y     226u

#define PC_NUM_Y      252u
#define PC_NUM_H       92u

#define RESET_BTN_X   150u
#define RESET_BTN_Y   352u
#define RESET_BTN_W   220u
#define RESET_BTN_H    86u

#define PC_NEXT_X     430u
#define PC_NEXT_Y     352u
#define PC_NEXT_W     220u
#define PC_NEXT_H      86u

static const uint16_t s_hopper_color[HOPPER_COUNT] = {
    GEM_RED, GEM_ORANGE, GEM_YELLOW, GEM_GREEN
};
static const char *const s_hopper_label[HOPPER_COUNT] = { "A", "B", "C", "D" };

static void draw_hopper_slot(uint8_t i)
{
    const uint16_t x    = HOPPER_X(i);
    const bool     live = (i < HOPPER_LIVE);

    gui_fill_round_rect(x, HOPPER_Y, HOPPER_W, HOPPER_H, 18u,
                        live ? THEME_ROSE_FILL : SLOT_OFF_FILL);
    gui_stroke_round_rect(x, HOPPER_Y, HOPPER_W, HOPPER_H, 18u, live ? 5u : 3u,
                          live ? THEME_ROSE_EDGE : SLOT_OFF_EDGE);

    /* The lentil itself: hopper colour when the slot is fitted, grey when it
     * is a placeholder. */
    gui_draw_gem((uint16_t)(x + 40u), (uint16_t)(HOPPER_Y + HOPPER_H / 2u), 21u,
                 live ? s_hopper_color[i] : GEM_OFF);

    gui_font_text((uint16_t)(x + 74u),
                  (uint16_t)(HOPPER_Y + 12u),
                  s_hopper_label[i], live ? THEME_INK : THEME_INK_SOFT,
                  &ui_font_md);
    gui_font_text((uint16_t)(x + 74u),
                  (uint16_t)(HOPPER_Y + 50u),
                  live ? "TAP +1" : "SOON",
                  THEME_INK_SOFT, &ui_font_sm);
}

static void draw_pillcount_number(void)
{
    gui_draw_rect(250, PC_NUM_Y, 300, PC_NUM_H, THEME_BG);
    char buf[4];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s_pill_count);
    /* Dedicated 76px numeral font - measured centring handles 1 and 10 alike
     * (this used to be a hardcoded "350: 375" two-branch x-offset). */
    gui_font_text_centered(400u, PC_NUM_Y, buf, THEME_INK, &ui_font_num);

    /* Standalone (also called on every pill/reset tap) - flush itself. */
    gui_draw_flush_rows(PC_NUM_Y, (uint16_t)(PC_NUM_Y + PC_NUM_H));
}

void registration_ui_draw_pillcount(void)
{
    gui_draw_frame();
    gui_draw_title_bar("PILLS PER DOSE", ACCENT_NEUTRAL);

    gui_font_text_centered(400u, 96u, s_name, THEME_INK_SOFT, &ui_font_sm);

    for (uint8_t i = 0; i < HOPPER_COUNT; i++)
        draw_hopper_slot(i);

    gui_font_text_centered(400u, CAPTION_Y,
                           "This unit has one hopper. B, C and D unlock with the "
                           "carer app.",
                           THEME_INK_SOFT, &ui_font_sm);

    draw_pillcount_number();

    gui_draw_button(RESET_BTN_X, RESET_BTN_Y, RESET_BTN_W, RESET_BTN_H,
                    THEME_AMBER_EDGE, "RESET", "");
    gui_draw_button(PC_NEXT_X, PC_NEXT_Y, PC_NEXT_W, PC_NEXT_H,
                    THEME_GREEN_EDGE, "NEXT", "");

    gui_draw_flush();
}

bool registration_ui_handle_pillcount_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, RESET_BTN_X, RESET_BTN_Y, RESET_BTN_W, RESET_BTN_H))
    {
        s_pill_count = PILLCOUNT_MIN;
        draw_pillcount_number();
        return false;
    }
    /* Only the fitted hoppers accept a tap. The greyed slots are deliberately
     * inert rather than showing an error - they are labelled "SOON", which
     * already says why nothing happened. */
    for (uint8_t i = 0; i < HOPPER_LIVE; i++)
    {
        if (hit(tx, ty, HOPPER_X(i), HOPPER_Y, HOPPER_W, HOPPER_H))
        {
            if (s_pill_count < PILLCOUNT_MAX)
            {
                s_pill_count++;
                draw_pillcount_number();
            }
            return false;
        }
    }
    if (hit(tx, ty, PC_NEXT_X, PC_NEXT_Y, PC_NEXT_W, PC_NEXT_H))
        return true;

    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * CONFIRM screen
 *
 * This screen used to keep its own CONFIRM_BTN_* geometry,
 * visually different from the FACE_RETRY/alert screens' CHOICE_BTN_* layout
 * (260x180 vs 260x170 - both "two big buttons side by side",
 * the technical reference.5). It reuses CHOICE_* from gui_draw.h now.
 * ══════════════════════════════════════════════════════════════════════════ */

void registration_ui_draw_confirm(void)
{
    gui_draw_frame();
    gui_draw_title_bar("CONFIRM REGISTRATION", ACCENT_NEUTRAL);

    char line[80];
    gui_font_text_centered(400u, 140u, "NAME", THEME_INK_SOFT, &ui_font_sm);
    snprintf(line, sizeof(line), "%s", s_name);
    gui_font_text_centered(400u, 166u, line, THEME_INK, &ui_font_lg);

    gui_font_text_centered(400u, 216u, "PILLS PER DOSE", THEME_INK_SOFT, &ui_font_sm);
    snprintf(line, sizeof(line), "%u", (unsigned)s_pill_count);
    gui_font_text_centered(400u, 242u, line, THEME_INK, &ui_font_lg);

    gui_draw_button(CHOICE_LEFT_X,  CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H,
                    THEME_GREEN_EDGE, "CONFIRM", "");
    gui_draw_button(CHOICE_RIGHT_X, CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H,
                    THEME_AMBER_EDGE, "RETRY", "");

    gui_draw_flush();
}

reg_confirm_result_t registration_ui_handle_confirm_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, CHOICE_LEFT_X, CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H))
    {
        int slot = gallery_add_patient(s_name, s_embedding, (int)s_pill_count);
        if (slot < 0)
        {
            MS_DBG_PRINTF("registration_ui: gallery full, could not save '%s'.\r\n", s_name);
            return REG_CONFIRM_FULL;
        }
        if (!gallery_last_save_ok())
        {
            MS_DBG_PRINTF("registration_ui: patient '%s' in slot %d is RAM-ONLY "
                          "(patients.dat not written).\r\n", s_name, slot);
            return REG_CONFIRM_SAVED_NO_SD;
        }
        MS_DBG_PRINTF("registration_ui: patient '%s' saved to slot %d.\r\n", s_name, slot);
        return REG_CONFIRM_SAVED;
    }
    if (hit(tx, ty, CHOICE_RIGHT_X, CHOICE_BTN_Y, CHOICE_BTN_W, CHOICE_BTN_H))
        return REG_CONFIRM_RETRY;

    return REG_CONFIRM_NONE;
}
