/* ui/carer_ui.c - The passcode gate and carer mode.
 * See ui/carer_ui.h for what this module is and why the gate is shared.
 *
 * Drawing follows the project's design system throughout - gui_draw_frame(),
 * gui_draw_title_bar(), gui_draw_button() - so these screens look like the
 * rest of the product rather than like a settings menu bolted on. Touch is
 * handled exactly as registration_ui.c handles it: state_machine.c polls the
 * panel once per tick and passes rising-edge coordinates in here. This module
 * never calls touch_driver.h itself, so there is only ever one edge detector
 * for one physical touch.
 */

#include "ui/carer_ui.h"
#include "ui/gui_draw.h"
#include "ai_vision.h"
#include "sd_logger.h"
#include "schedule_time_source.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>
#include <string.h>

#ifndef MEDSIGHT_DEBUG
#define MEDSIGHT_DEBUG 0
#endif
#if MEDSIGHT_DEBUG
#define MS_DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define MS_DBG_PRINTF(...) do { } while (0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 1 - the passcode store
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CARER_FILE      "carer.cfg"
#define CARER_MAGIC     0x4350534Du   /* 'M','S','P','C' little-endian */
#define CARER_VERSION   1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t hash;        /* FNV-1a of salt||digits - see below */
} CarerConfigFile;

static uint32_t s_pass_hash    = 0u;
static bool     s_pass_default = true;

/* FNV-1a, 32-bit, with a fixed salt.
 *
 * Be clear-eyed about what this is. It is NOT a password hash in the sense
 * that word usually means - there is no work factor, no per-device salt, and
 * a four-digit code has ten thousand possibilities, which is seconds of work
 * for anyone who has the card and knows the algorithm. Both of those facts
 * are stated plainly in the technical reference rather than papered
 * over.
 *
 * What it DOES buy, and the reason it is here rather than storing the digits
 * in the clear: a carer's chosen code is very likely a code they use
 * elsewhere, and the card can be read on any laptop. Writing "4291" to a
 * file on a removable card would leak that reuse to anyone who ever picks
 * the card up, for no reason at all. A hash costs eight lines and removes
 * that. */
#define PASS_SALT  "MedSight-S15-carer"

static uint32_t pass_hash(const char *digits)
{
    uint32_t h = 2166136261u;
    for (const char *p = PASS_SALT; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    for (const char *p = digits; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

/* Constant-time compare of the two hashes.
 *
 * A plain `a == b` on two 32-bit words is already very close to constant
 * time on this core, so this is close to free - which is exactly the design
 * brief's test ("compare in constant time if it is free to do so"). It is
 * written out anyway because the shape is what matters: an early-out
 * comparison is the habit that eventually gets applied to a byte array,
 * where it genuinely leaks. */
static bool hash_equal_ct(uint32_t a, uint32_t b)
{
    uint32_t diff = a ^ b;
    /* Fold every differing bit down into bit 0 without branching. */
    diff |= diff >> 16;
    diff |= diff >> 8;
    diff |= diff >> 4;
    diff |= diff >> 2;
    diff |= diff >> 1;
    return ((diff & 1u) == 0u);
}

static bool pass_save(uint32_t hash)
{
    CarerConfigFile cfg = {
        .magic    = CARER_MAGIC,
        .version  = CARER_VERSION,
        .reserved = 0u,
        .hash     = hash,
    };
    bool ok = SD_Write_File(CARER_FILE, (const uint8_t *)&cfg, sizeof(cfg));
    if (!ok) {
        printf("carer_pass: FAILED to write %s - the new passcode is live "
               "now but will be lost on the next power cycle.\r\n", CARER_FILE);
    }
    return ok;
}

void carer_pass_init(void)
{
    s_pass_hash    = pass_hash(MEDSIGHT_DEFAULT_CARER_CODE);
    s_pass_default = true;

    CarerConfigFile cfg;
    uint32_t br = 0u;
    if (!SD_Read_File(CARER_FILE, (uint8_t *)&cfg, sizeof(cfg), &br)) {
        printf("carer_pass: no %s on the card - using the build-time default "
               "passcode.\r\n", CARER_FILE);
        return;
    }
    if ((br != sizeof(cfg)) || (cfg.magic != CARER_MAGIC) ||
        (cfg.version != CARER_VERSION)) {
        /* Same reasoning as patients.dat's versioned header: a
         * file that is the wrong shape says so, rather than being silently
         * indistinguishable from no file at all. */
        printf("carer_pass: %s is not a v%u MedSight passcode file "
               "(%lu bytes, magic %08lX) - using the default.\r\n",
               CARER_FILE, (unsigned)CARER_VERSION,
               (unsigned long)br, (unsigned long)cfg.magic);
        return;
    }

    s_pass_hash    = cfg.hash;
    s_pass_default = hash_equal_ct(s_pass_hash,
                                   pass_hash(MEDSIGHT_DEFAULT_CARER_CODE));
    printf("carer_pass: passcode loaded from %s (%s).\r\n", CARER_FILE,
           s_pass_default ? "still the default" : "changed from the default");
}

bool carer_pass_is_default(void) { return s_pass_default; }

/* ── Rate limiting ────────────────────────────────────────────────────────
 *
 * After CARER_MAX_TRIES wrong entries the prompt refuses everything for
 * CARER_LOCKOUT_MS. Ten thousand four-digit codes at five tries per thirty
 * seconds is about eight hours of continuous tapping, which is not a
 * cryptographic guarantee but is far past what a curious patient will do.
 *
 * The counter lives in RAM, so a power cycle clears it. That is a real
 * weakness and it is deliberate rather than overlooked: persisting it would
 * mean writing to the SD card on every failed attempt - turning a wrong tap
 * into a card write, and giving anyone who wanted it a way to wear the card
 * out or to lock a device out permanently by pulling power at the wrong
 * moment. Against the stated threat model the RAM counter is the better
 * trade. It is recorded in the technical reference, not hidden. */
#define CARER_MAX_TRIES    5u
#define CARER_LOCKOUT_MS   30000u

static uint8_t  s_fail_count   = 0u;
static uint32_t s_lock_until   = 0u;

static bool pass_locked_out(void)
{
    if (s_lock_until == 0u) {
        return false;
    }
    if ((int32_t)(HAL_GetTick() - s_lock_until) >= 0) {
        s_lock_until = 0u;
        s_fail_count = 0u;
        return false;
    }
    return true;
}

/* THE one validation routine. Both entry points reach the passcode through
 * this function and no other. */
static bool carer_pass_check(const char *digits)
{
    if (pass_locked_out()) {
        return false;
    }
    if (hash_equal_ct(pass_hash(digits), s_pass_hash)) {
        s_fail_count = 0u;
        s_lock_until = 0u;
        return true;
    }
    s_fail_count++;
    if (s_fail_count >= CARER_MAX_TRIES) {
        s_lock_until = HAL_GetTick() + CARER_LOCKOUT_MS;
        /* Unconditional: a lockout is a security-relevant event and it goes
         * in the audit trail whether or not MEDSIGHT_DEBUG is on. No entered
         * digits are logged, here or anywhere. */
        printf("carer_pass: %u failed attempts - refusing for %us.\r\n",
               (unsigned)s_fail_count, (unsigned)(CARER_LOCKOUT_MS / 1000u));
        SD_Log_Event_Async("SECURITY: carer passcode locked out (5 failures)");
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 2 - shared layout helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* A four-button footer row is the shape of nearly every carer screen. */
#define FOOT_Y        374u
#define FOOT_H         76u
#define FOOT_W        168u
#define FOOT_GAP       16u
#define FOOT_X(i)     ((uint16_t)(56u + (i) * (FOOT_W + FOOT_GAP)))

static bool hit(uint32_t tx, uint32_t ty,
                uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return (tx >= x) && (tx <= (x + w)) && (ty >= y) && (ty <= (y + h));
}

/* A big minus and a big plus, flanking whatever is being adjusted.
 *
 * These were the clock screen's until that was replaced with keypad
 * entry (Part 5), and the dose-times screen's until the bench showed that
 * stepping to an arbitrary time is just as bad. ONE screen still uses them,
 * and it is the one case where stepping is genuinely right: a dose size runs
 * 1..10, so every value is at most five taps away and the control shows the
 * whole range implicitly. A four-digit year or an arbitrary time is what
 * stepping is worst at. */
#define STEP_MINUS_X   90u
#define STEP_PLUS_X   582u
#define STEP_W        128u
#define STEP_H         74u

/* The status line every carer screen carries: what time the device thinks it
 * is, and which time source said so. A carer setting up a schedule needs to
 * see the clock at all times - a schedule set against a wrong clock is the
 * single most likely way to make this feature do harm.
 *
 * A fix from hardware: this used to be TWO lines at fixed y=108
 * and y=132, and the caller had no say. On the dose-times screen that put it
 * straight through the instruction line the screen drew at y=96. It is now
 * one line at a y the caller picks, so no screen can collide with it, and
 * the "never been set" case folds INTO that one line instead of adding a
 * second. See gui_draw.h's vertical-rhythm block. */
/* Where the strip was last drawn, and what it last said. 0 means no screen
 * is currently showing one, so the tick below is a no-op. */
static uint16_t s_strip_y = 0u;
static char     s_strip_last[96] = "";

#define STRIP_H   24u

static void strip_compose(char *out, size_t n, uint16_t *out_colour)
{
    if (!time_source_is_valid()) {
        snprintf(out, n, "CLOCK NOT SET - schedules will not run");
        *out_colour = THEME_RED_EDGE;
        return;
    }
    char now[48];
    time_source_format_now(now, sizeof(now));
    snprintf(out, n, "%s   [%s]", now, time_source_mode());
    *out_colour = THEME_INK_SOFT;
}

void carer_ui_draw_clock_strip(uint16_t y)
{
    uint16_t colour = THEME_INK_SOFT;
    strip_compose(s_strip_last, sizeof(s_strip_last), &colour);

    gui_draw_rect(50u, y, 700u, STRIP_H, THEME_BG);
    gui_font_text_ellipsis(400u, y, s_strip_last, 700u, colour, &ui_font_sm);
    s_strip_y = y;
}

/* Internal shorthand - every carer screen draws its strip through this. */
static void draw_clock_strip(uint16_t y)
{
    carer_ui_draw_clock_strip(y);
}

void carer_ui_clock_strip_hide(void)
{
    s_strip_y = 0u;
    s_strip_last[0] = '\0';
}

bool carer_ui_clock_tick(void)
{
    if (s_strip_y == 0u) {
        return false;
    }

    char     fresh[96];
    uint16_t colour = THEME_INK_SOFT;
    strip_compose(fresh, sizeof(fresh), &colour);

    /* Only touch the framebuffer when the text actually changes. At one
     * simulated minute per real second that is about once a second, not
     * once every 10 ms UI tick. */
    if (strcmp(fresh, s_strip_last) == 0) {
        return false;
    }

    memcpy(s_strip_last, fresh, sizeof(s_strip_last));
    gui_draw_rect(50u, s_strip_y, 700u, STRIP_H, THEME_BG);
    gui_font_text_ellipsis(400u, s_strip_y, s_strip_last, 700u, colour, &ui_font_sm);
    gui_draw_flush_rows(s_strip_y, (uint16_t)(s_strip_y + STRIP_H));
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 3 - the passcode prompt (shared by carer mode and registration)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Numeric keypad: 3 columns x 4 rows, the phone layout everyone already
 * knows. 128 px keys - nearly twice the area of registration's QWERTY keys,
 * which is the whole argument for digits over a typed word here. */
#define KEY_W        128u
#define KEY_H         66u
#define KEY_GAP_X     14u
#define KEY_GAP_Y     10u
#define KEY_X0       182u
#define KEY_Y0       150u
#define KEY_X(c)     ((uint16_t)(KEY_X0 + (c) * (KEY_W + KEY_GAP_X)))
#define KEY_Y(r)     ((uint16_t)(KEY_Y0 + (r) * (KEY_H + KEY_GAP_Y)))

#define PW_CANCEL_X   40u
#define PW_CANCEL_Y  372u
#define PW_CANCEL_W  120u
#define PW_CANCEL_H   62u

static char                  s_entry[CARER_CODE_MAX_DIGITS + 1u];
static uint8_t               s_entry_len = 0u;
static carer_prompt_reason_t s_reason    = CARER_PROMPT_CARER_MODE;
static const char           *s_prompt_note = "";

void carer_ui_prompt_begin(carer_prompt_reason_t reason)
{
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len   = 0u;
    s_reason      = reason;
    s_prompt_note = "";
}

/* The keypad's label for (row, col). Row 3 is DEL / 0 / OK. */
static const char *key_label(uint8_t r, uint8_t c)
{
    static const char *const L[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "DEL", "0", "OK" }
    };
    return L[r][c];
}

/* Where the entry dots sit: below SUBTEXT_Y's line and clear of the keypad's
 * first row at KEY_Y0. Both ends of that gap are asserted by the geometry
 * rather than eyeballed. */
#define DOTS_BAND_Y   120u
#define DOTS_BAND_H    28u
#define DOTS_CY       134u

static void draw_entry_dots(void)
{
    /* Dots, not digits. A passcode typed at a device in a living room is
     * read over a shoulder as easily as it is typed, and echoing it back at
     * 40 px would make that trivial. The count is still visible, which is
     * the feedback the user actually needs. */
    gui_draw_rect(250u, DOTS_BAND_Y, 300u, DOTS_BAND_H, THEME_BG);
    const uint16_t cx0 = (uint16_t)(400u - (s_entry_len * 26u) / 2u);
    for (uint8_t i = 0; i < s_entry_len; i++) {
        gui_draw_gem((uint16_t)(cx0 + i * 26u + 13u), DOTS_CY, 9u, THEME_ROSE_EDGE);
    }
}

void carer_ui_draw_prompt(void)
{
    gui_draw_frame();

    if (s_reason == CARER_PROMPT_REGISTRATION) {
        gui_draw_title_bar("A CARER SETS THIS UP", ACCENT_NEUTRAL);
    } else {
        gui_draw_title_bar("CARER ACCESS", ACCENT_NEUTRAL);
    }

    /* Wording matters here and it is not decoration. The person most likely
     * to see the registration version of this screen is a patient who
     * pressed the wrong button, and they should be told what to do, not that
     * they were refused. */
    const char *sub = (s_reason == CARER_PROMPT_REGISTRATION)
        ? "Ask your carer to enter their code."
        : "Enter the carer code.";
    if (s_prompt_note[0] != '\0') {
        sub = s_prompt_note;
    }
    gui_font_text_ellipsis(400u, SUBTEXT_Y, sub, 700u,
                           (s_prompt_note[0] != '\0') ? THEME_RED_EDGE : THEME_INK_SOFT,
                           &ui_font_sm);

    draw_entry_dots();

    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            const char *lbl = key_label(r, c);
            uint16_t edge = THEME_INK_SOFT;
            if (r == 3u && c == 2u) edge = THEME_GREEN_EDGE;   /* OK  */
            if (r == 3u && c == 0u) edge = THEME_AMBER_EDGE;   /* DEL */
            gui_draw_button(KEY_X(c), KEY_Y(r), KEY_W, KEY_H, edge, lbl, "");
        }
    }

    gui_draw_button(PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H,
                    THEME_INK_SOFT, "CANCEL", "");
    gui_draw_flush();
}

/* Shared by the prompt and the change-code screen: apply one keypad tap to
 * s_entry. Returns the label that was hit, or NULL. */
static const char *keypad_apply(uint32_t tx, uint32_t ty)
{
    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            if (!hit(tx, ty, KEY_X(c), KEY_Y(r), KEY_W, KEY_H)) {
                continue;
            }
            const char *lbl = key_label(r, c);
            if (lbl[0] == 'D') {                       /* DEL */
                if (s_entry_len > 0u) {
                    s_entry[--s_entry_len] = '\0';
                }
            } else if (lbl[0] == 'O') {                /* OK  */
                /* handled by the caller */
            } else if (s_entry_len < CARER_CODE_MAX_DIGITS) {
                s_entry[s_entry_len++] = lbl[0];
                s_entry[s_entry_len]   = '\0';
            }
            return lbl;
        }
    }
    return NULL;
}

carer_action_t carer_ui_handle_prompt_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H)) {
        return CARER_ACT_BACK;
    }

    const char *lbl = keypad_apply(tx, ty);
    if (lbl == NULL) {
        return CARER_ACT_NONE;
    }

    if (lbl[0] != 'O') {          /* a digit or DEL: just update the dots */
        draw_entry_dots();
        gui_draw_flush_rows(DOTS_BAND_Y, (uint16_t)(DOTS_BAND_Y + DOTS_BAND_H));
        return CARER_ACT_NONE;
    }

    /* OK */
    if (pass_locked_out()) {
        memset(s_entry, 0, sizeof(s_entry));
        s_entry_len   = 0u;
        s_prompt_note = "Too many wrong codes. Please wait 30 seconds.";
        carer_ui_draw_prompt();
        return CARER_ACT_LOCKED_OUT;
    }
    if (s_entry_len < CARER_CODE_MIN_DIGITS) {
        s_prompt_note = "The code is at least 4 digits.";
        carer_ui_draw_prompt();
        return CARER_ACT_NONE;
    }

    bool ok = carer_pass_check(s_entry);
    /* Wipe the entry buffer either way, immediately. It is the only place in
     * this firmware that holds a secret in plain bytes and it should hold it
     * for as short a time as possible. */
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len = 0u;

    if (ok) {
        s_prompt_note = "";
        return CARER_ACT_ACCEPTED;
    }

    /* A fix from the hardware log: the FIFTH wrong entry used to
     * return LOCKED_OUT and nothing else, so the audit trail read as four
     * wrong codes followed by a lockout. Five wrong codes is what happened
     * and five is what the log should say. The caller logs REJECTED; the
     * lockout is logged separately inside carer_pass_check(). */
    s_prompt_note = pass_locked_out()
                    ? "Too many wrong codes. Please wait 30 seconds."
                    : "That code is not right. Try again.";
    carer_ui_draw_prompt();
    return CARER_ACT_REJECTED;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 4 - carer menu
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MENU_W        340u
#define MENU_H         92u
#define MENU_LX        40u
#define MENU_RX       420u
#define MENU_TY       168u
#define MENU_BY       268u

void carer_ui_draw_menu(void)
{
    gui_draw_frame();
    gui_draw_title_bar("CARER MODE", ACCENT_NEUTRAL);
    draw_clock_strip(SUBTEXT_Y);

    gui_draw_button(MENU_LX, MENU_TY, MENU_W, MENU_H, THEME_ROSE_EDGE,
                    "SET CLOCK", "date and time");
    gui_draw_button(MENU_RX, MENU_TY, MENU_W, MENU_H, THEME_GREEN_EDGE,
                    "PATIENTS", "schedule, dose, delete");
    gui_draw_button(MENU_LX, MENU_BY, MENU_W, MENU_H, THEME_ROSE_EDGE,
                    "REVIEW LOG", "doses taken and missed");
    gui_draw_button(MENU_RX, MENU_BY, MENU_W, MENU_H, THEME_AMBER_EDGE,
                    "CHANGE CODE", carer_pass_is_default()
                                   ? "still the default!" : "");

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT,
                    "EXIT", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_menu_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, MENU_LX, MENU_TY, MENU_W, MENU_H)) return CARER_ACT_GOTO_CLOCK;
    if (hit(tx, ty, MENU_RX, MENU_TY, MENU_W, MENU_H)) return CARER_ACT_GOTO_PATIENTS;
    if (hit(tx, ty, MENU_LX, MENU_BY, MENU_W, MENU_H)) return CARER_ACT_GOTO_LOG;
    if (hit(tx, ty, MENU_RX, MENU_BY, MENU_W, MENU_H)) return CARER_ACT_GOTO_CHANGE_CODE;
    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;
    return CARER_ACT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 5 - set the clock
 *
 * REDESIGNED after the first hardware round. The original was
 * five boxes stepped with a big minus and a big plus, chosen because it made
 * an invalid date impossible to enter. It did - and it was miserable to use:
 * setting a year, a day and a time from the device's power-on default is on
 * the order of a hundred taps, and the project owner said so after one go.
 *
 * "Impossible to enter an invalid value" was the wrong thing to optimise for.
 * A carer sets the clock while looking at their phone; they know the date.
 * The job is to let them TYPE it. Twelve digits and an OK is thirteen taps,
 * against a hundred, and validation is one function call at the end.
 *
 * It reuses the passcode keypad - same geometry, same key labels, same DEL
 * and OK - so there is one numeric input in this UI rather than two, and a
 * carer who has entered their code already knows how this screen works. The
 * only thing that differs is what is displayed above it.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* DDMMYYYYHHMM, filled left to right. */
#define CLK_DIGITS   12u

static char    s_clk[CLK_DIGITS + 1u];
static uint8_t s_clk_len  = 0u;
static const char *s_clk_note = "";

/* The entry line sits above the keypad; the current clock sits below it as a
 * reference, so a carer can see what they are replacing.
 *
 * CLK_ENTRY_H is one ui_font_md line (28 px) plus a couple of pixels of
 * slack, and it is the exact band clock_refresh() repaints on every keypress
 * instead of redrawing the screen. */
#define CLK_ENTRY_Y   96u
#define CLK_ENTRY_H   30u

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1u || month > 12u) return 31u;
    if (month == 2u) {
        bool leap = ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
        return leap ? 29u : 28u;
    }
    return d[month - 1u];
}

void carer_ui_begin_clock(void)
{
    memset(s_clk, 0, sizeof(s_clk));
    s_clk_len  = 0u;
    s_clk_note = "";
}

/* Build "DD/MM/YYYY  HH:MM" with '_' wherever a digit has not been typed
 * yet. One string, one draw - the separators are what make twelve otherwise
 * undifferentiated digits legible as a date and a time. */
static void clk_entry_text(char *buf, size_t n)
{
    char d[CLK_DIGITS];
    for (uint8_t i = 0; i < CLK_DIGITS; i++) {
        d[i] = (i < s_clk_len) ? s_clk[i] : '_';
    }
    snprintf(buf, n, "%c%c/%c%c/%c%c%c%c   %c%c:%c%c",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
             d[8], d[9], d[10], d[11]);
}

/* Repaint ONLY the entry line and the note/clock line beneath it. Called on
 * every keypress; the twelve keys, the frame and the title never change
 * while a date is being typed, so they are never redrawn. */
static void clock_refresh_fields(void)
{
    char entry[40];
    clk_entry_text(entry, sizeof(entry));

    gui_draw_rect(150u, CLK_ENTRY_Y, 500u, CLK_ENTRY_H, THEME_BG);
    gui_font_text_centered(400u, CLK_ENTRY_Y, entry,
                           (s_clk_note[0] != '\0') ? THEME_RED_EDGE : THEME_INK,
                           &ui_font_md);

    if (s_clk_note[0] != '\0') {
        /* A validation message replaces the reference clock while it is up,
         * so the strip must stop ticking over the top of it. */
        carer_ui_clock_strip_hide();
        gui_draw_rect(50u, STATUS_Y, 700u, STRIP_H, THEME_BG);
        gui_font_text_ellipsis(400u, STATUS_Y, s_clk_note, 700u,
                               THEME_RED_EDGE, &ui_font_sm);
    } else {
        draw_clock_strip(STATUS_Y);
    }
}

/* The same two bands, flushed. Use after a keypress. */
static void clock_refresh(void)
{
    clock_refresh_fields();
    gui_draw_flush_rows(CLK_ENTRY_Y, (uint16_t)(STATUS_Y + STRIP_H));
}

void carer_ui_draw_clock(void)
{
    gui_draw_frame();
    gui_draw_title_bar("SET THE CLOCK", ACCENT_NEUTRAL);

    clock_refresh_fields();

    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            uint16_t edge = THEME_INK_SOFT;
            if (r == 3u && c == 2u) edge = THEME_GREEN_EDGE;   /* OK  */
            if (r == 3u && c == 0u) edge = THEME_AMBER_EDGE;   /* DEL */
            gui_draw_button(KEY_X(c), KEY_Y(r), KEY_W, KEY_H, edge,
                            key_label(r, c), "");
        }
    }

    gui_draw_button(PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H,
                    THEME_INK_SOFT, "CANCEL", "");
    gui_draw_flush();
}

/* Two digits at s_clk[i]. */
static uint16_t clk_pair(uint8_t i)
{
    return (uint16_t)(((s_clk[i] - '0') * 10) + (s_clk[i + 1u] - '0'));
}

carer_action_t carer_ui_handle_clock_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H)) {
        return CARER_ACT_BACK;
    }

    /* Same keypad, driving this screen's own buffer rather than the passcode
     * one - deliberately NOT shared state, because a half-typed date and a
     * half-typed passcode must never be able to reach each other. */
    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            if (!hit(tx, ty, KEY_X(c), KEY_Y(r), KEY_W, KEY_H)) continue;

            const char *lbl = key_label(r, c);

            if (lbl[0] == 'D') {                       /* DEL */
                if (s_clk_len > 0u) s_clk[--s_clk_len] = '\0';
                s_clk_note = "";
                clock_refresh();
                return CARER_ACT_NONE;
            }
            if (lbl[0] != 'O') {                       /* a digit */
                if (s_clk_len < CLK_DIGITS) {
                    s_clk[s_clk_len++] = lbl[0];
                    s_clk[s_clk_len]   = '\0';
                }
                s_clk_note = "";
                clock_refresh();
                return CARER_ACT_NONE;
            }

            /* OK - validate, then commit. */
            if (s_clk_len < CLK_DIGITS) {
                s_clk_note = "Type all of DD MM YYYY HH MM first.";
                clock_refresh();
                return CARER_ACT_NONE;
            }

            ms_datetime_t dt;
            dt.day    = (uint8_t)clk_pair(0u);
            dt.month  = (uint8_t)clk_pair(2u);
            dt.year   = (uint16_t)((clk_pair(4u) * 100u) + clk_pair(6u));
            dt.hour   = (uint8_t)clk_pair(8u);
            dt.minute = (uint8_t)clk_pair(10u);
            /* Setting the clock zeroes the seconds: a carer is reading a
             * phone, not a stopwatch, and starting the minute cleanly is
             * what makes a dose window open when they expect it to. */
            dt.second = 0u;

            /* Validation is one pass, and it names the field that is wrong
             * rather than saying "invalid". Getting told WHICH number to fix
             * is the whole difference between a helpful error and a wall. */
            if (dt.month < 1u || dt.month > 12u) {
                s_clk_note = "Month must be 01 to 12.";
            } else if (dt.year < 2026u || dt.year > 2099u) {
                s_clk_note = "Year must be 2026 to 2099.";
            } else if (dt.day < 1u || dt.day > days_in_month(dt.year, dt.month)) {
                s_clk_note = "That day does not exist in that month.";
            } else if (dt.hour > 23u) {
                s_clk_note = "Hour must be 00 to 23.";
            } else if (dt.minute > 59u) {
                s_clk_note = "Minutes must be 00 to 59.";
            } else if (!time_source_set(&dt)) {
                return CARER_ACT_REJECTED;
            } else {
                char now[48];
                time_source_format_now(now, sizeof(now));
                char line[SD_LOG_MSG_MAX_LEN];
                snprintf(line, sizeof(line), "CARER: clock set to %s", now);
                SD_Log_Event_Async(line);
                return CARER_ACT_ACCEPTED;
            }

            clock_refresh();
            return CARER_ACT_NONE;
        }
    }
    return CARER_ACT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 6 - change the passcode
 *
 * Reuses the same keypad as the prompt. Two stages: enter a new code, then
 * enter it again. There is deliberately NO "enter the old code first" - the
 * carer already entered it to get into carer mode, thirty seconds ago, and
 * asking twice teaches people that this device asks for its code more often
 * than it needs to.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool     s_cc_second_stage = false;
static uint32_t s_cc_first_hash   = 0u;
static const char *s_cc_note      = "";

void carer_ui_begin_change_code(void)
{
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len       = 0u;
    s_cc_second_stage = false;
    s_cc_first_hash   = 0u;
    s_cc_note         = "";
}

void carer_ui_draw_change_code(void)
{
    gui_draw_frame();
    gui_draw_title_bar(s_cc_second_stage ? "CONFIRM NEW CODE" : "NEW CARER CODE",
                       ACCENT_WARN);

    const char *sub = s_cc_second_stage
        ? "Enter the same code again."
        : "4 to 8 digits. Write it down somewhere safe.";
    if (s_cc_note[0] != '\0') sub = s_cc_note;
    gui_font_text_ellipsis(400u, SUBTEXT_Y, sub, 700u,
                           (s_cc_note[0] != '\0') ? THEME_RED_EDGE : THEME_INK_SOFT,
                           &ui_font_sm);

    draw_entry_dots();

    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            uint16_t edge = THEME_INK_SOFT;
            if (r == 3u && c == 2u) edge = THEME_GREEN_EDGE;
            if (r == 3u && c == 0u) edge = THEME_AMBER_EDGE;
            gui_draw_button(KEY_X(c), KEY_Y(r), KEY_W, KEY_H, edge,
                            key_label(r, c), "");
        }
    }
    gui_draw_button(PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H,
                    THEME_INK_SOFT, "CANCEL", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_change_code_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H)) {
        memset(s_entry, 0, sizeof(s_entry));
        s_entry_len = 0u;
        return CARER_ACT_BACK;
    }

    const char *lbl = keypad_apply(tx, ty);
    if (lbl == NULL) {
        return CARER_ACT_NONE;
    }
    if (lbl[0] != 'O') {
        draw_entry_dots();
        gui_draw_flush_rows(DOTS_BAND_Y, (uint16_t)(DOTS_BAND_Y + DOTS_BAND_H));
        return CARER_ACT_NONE;
    }

    if (s_entry_len < CARER_CODE_MIN_DIGITS) {
        s_cc_note = "The code must be at least 4 digits.";
        carer_ui_draw_change_code();
        return CARER_ACT_NONE;
    }

    uint32_t h = pass_hash(s_entry);
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len = 0u;

    if (!s_cc_second_stage) {
        s_cc_first_hash   = h;
        s_cc_second_stage = true;
        s_cc_note         = "";
        carer_ui_draw_change_code();
        return CARER_ACT_NONE;
    }

    if (!hash_equal_ct(h, s_cc_first_hash)) {
        s_cc_second_stage = false;
        s_cc_first_hash   = 0u;
        s_cc_note         = "The two codes did not match. Start again.";
        carer_ui_draw_change_code();
        return CARER_ACT_REJECTED;
    }

    s_pass_hash    = h;
    s_pass_default = hash_equal_ct(h, pass_hash(MEDSIGHT_DEFAULT_CARER_CODE));
    bool saved = pass_save(h);
    SD_Log_Event_Async(saved ? "CARER: passcode changed"
                             : "CARER: passcode changed - RAM only (no SD)");
    return saved ? CARER_ACT_ACCEPTED : CARER_ACT_REJECTED;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 7 - patient list
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ROW_X          56u
#define ROW_W         688u
#define ROW_H          62u
#define ROW_GAP        10u
#define ROW_Y0        142u
#define ROW_Y(i)      ((uint16_t)(ROW_Y0 + (i) * (ROW_H + ROW_GAP)))
#define ROWS_PER_PAGE  3u

static int  s_slot_map[MAX_PATIENTS];   /* page-independent list of live slots */
static int  s_live_count   = 0;
static int  s_page         = 0;
static int  s_sel_slot     = -1;

void carer_ui_begin_patients(void)
{
    s_live_count = 0;
    for (int i = 0; i < MAX_PATIENTS; i++) {
        if (patient_gallery[i].valid) {
            s_slot_map[s_live_count++] = i;
        }
    }
    s_page     = 0;
    s_sel_slot = -1;
}

int carer_ui_selected_slot(void) { return s_sel_slot; }

const char *carer_ui_selected_name(void)
{
    if (s_sel_slot < 0 || s_sel_slot >= MAX_PATIENTS) return "";
    return patient_gallery[s_sel_slot].name;
}

/* "3 pills   08:00, 20:00" - the whole of a patient's plan on one line. */
static void patient_summary(int slot, char *buf, size_t n)
{
    const PatientRecord *p = &patient_gallery[slot];
    int used = snprintf(buf, n, "%u pill%s   ",
                        (unsigned)p->pill_count, (p->pill_count == 1u) ? "" : "s");
    if (used < 0 || (size_t)used >= n) return;

    if (p->dose_time_count == 0u) {
        snprintf(buf + used, n - (size_t)used, "no schedule");
        return;
    }
    for (uint8_t i = 0; i < p->dose_time_count && (size_t)used < n; i++) {
        char hhmm[8];
        time_source_format_hhmm(hhmm, sizeof(hhmm), p->dose_time[i]);
        int w = snprintf(buf + used, n - (size_t)used, "%s%s",
                         (i == 0u) ? "" : ", ", hhmm);
        if (w < 0) return;
        used += w;
    }
}

void carer_ui_draw_patients(void)
{
    gui_draw_frame();
    gui_draw_title_bar("PATIENTS", ACCENT_DISPENSE);

    if (s_live_count == 0) {
        gui_font_text_centered(400u, 190u,
                               "Nobody is registered on this device yet.\n"
                               "Use REGISTER PATIENT on the home screen.",
                               THEME_INK_SOFT, &ui_font_md);
    }

    int first = s_page * (int)ROWS_PER_PAGE;
    for (uint8_t r = 0; r < ROWS_PER_PAGE; r++) {
        int idx = first + (int)r;
        if (idx >= s_live_count) break;
        int slot = s_slot_map[idx];

        gui_fill_round_rect(ROW_X, ROW_Y(r), ROW_W, ROW_H, 16u, THEME_GREEN_FILL);
        gui_stroke_round_rect(ROW_X, ROW_Y(r), ROW_W, ROW_H, 16u, 3u,
                              THEME_GREEN_EDGE);

        /* PATIENT_NAME_MAX is 32, and a name that long in ui_font_md is far
         * wider than the 240 px this column has. Centred-with-ellipsis in a
         * measured column, rather than left-aligned and hoping. */
        gui_font_text_ellipsis((uint16_t)(ROW_X + 140u), (uint16_t)(ROW_Y(r) + 16u),
                               patient_gallery[slot].name, 236u,
                               THEME_INK, &ui_font_md);

        char sum[64];
        patient_summary(slot, sum, sizeof(sum));
        gui_font_text_ellipsis((uint16_t)(ROW_X + 470u), (uint16_t)(ROW_Y(r) + 20u),
                               sum, 400u, THEME_INK_SOFT, &ui_font_sm);
    }

    int pages = (s_live_count + (int)ROWS_PER_PAGE - 1) / (int)ROWS_PER_PAGE;
    if (pages < 1) pages = 1;
    /* Sized for the full int range the format could in principle produce,
     * not for the two digits it will actually hold - the compiler checks the
     * former and it is cheaper to satisfy it than to explain the difference. */
    char pg[40];
    snprintf(pg, sizeof(pg), "page %d of %d", s_page + 1, pages);
    gui_font_text_centered(400u, 348u, pg, THEME_INK_SOFT, &ui_font_sm);

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT, "BACK", "");
    if (pages > 1) {
        gui_draw_button(FOOT_X(2), FOOT_Y, FOOT_W, FOOT_H, THEME_ROSE_EDGE, "PREV", "");
        gui_draw_button(FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H, THEME_ROSE_EDGE, "NEXT", "");
    }
    gui_draw_flush();
}

carer_action_t carer_ui_handle_patients_touch(uint32_t tx, uint32_t ty)
{
    int first = s_page * (int)ROWS_PER_PAGE;
    for (uint8_t r = 0; r < ROWS_PER_PAGE; r++) {
        int idx = first + (int)r;
        if (idx >= s_live_count) break;
        if (hit(tx, ty, ROW_X, ROW_Y(r), ROW_W, ROW_H)) {
            s_sel_slot = s_slot_map[idx];
            return CARER_ACT_PATIENT_PICKED;
        }
    }

    int pages = (s_live_count + (int)ROWS_PER_PAGE - 1) / (int)ROWS_PER_PAGE;
    if (pages > 1) {
        if (hit(tx, ty, FOOT_X(2), FOOT_Y, FOOT_W, FOOT_H)) {
            s_page = (s_page + pages - 1) % pages;
            carer_ui_draw_patients();
            return CARER_ACT_NONE;
        }
        if (hit(tx, ty, FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H)) {
            s_page = (s_page + 1) % pages;
            carer_ui_draw_patients();
            return CARER_ACT_NONE;
        }
    }
    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;
    return CARER_ACT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 8 - one patient: schedule / dose / delete
 * ═══════════════════════════════════════════════════════════════════════════ */

void carer_ui_draw_patient_menu(void)
{
    gui_draw_frame();

    /* gui_draw_title_bar() draws in the 34 px display face without measuring,
     * and this title is a PATIENT NAME - up to 31 characters, which runs off
     * both sides of an 800 px panel. Draw the bar with an empty title and put
     * the name in ourselves, measured. */
    gui_draw_title_bar("", ACCENT_DISPENSE);
    gui_font_text_ellipsis(400u, TITLE_Y, carer_ui_selected_name(), 700u,
                           ACCENT_DISPENSE, &ui_font_lg);

    char sum[64] = "";
    if (s_sel_slot >= 0) {
        patient_summary(s_sel_slot, sum, sizeof(sum));
    }
    gui_font_text_ellipsis(400u, SUBTEXT_Y, sum, 700u, THEME_INK_SOFT, &ui_font_sm);

    gui_draw_button(MENU_LX, MENU_TY, MENU_W, MENU_H, THEME_GREEN_EDGE,
                    "DOSE TIMES", "when they take it");
    gui_draw_button(MENU_RX, MENU_TY, MENU_W, MENU_H, THEME_GREEN_EDGE,
                    "DOSE SIZE", "how many pills");
    gui_draw_button(MENU_LX, MENU_BY, MENU_W, MENU_H, THEME_RED_EDGE,
                    "DELETE", "remove this patient");

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT, "BACK", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_patient_menu_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, MENU_LX, MENU_TY, MENU_W, MENU_H)) return CARER_ACT_GOTO_SCHEDULE;
    if (hit(tx, ty, MENU_RX, MENU_TY, MENU_W, MENU_H)) return CARER_ACT_GOTO_DOSE;
    if (hit(tx, ty, MENU_LX, MENU_BY, MENU_W, MENU_H)) return CARER_ACT_GOTO_DELETE;
    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;
    return CARER_ACT_NONE;
}

/* ── Dose times ──────────────────────────────────────────────────────────
 *
 * REWORKED after the first hardware round, for the same reason the clock
 * screen was. This used a 15-minute stepper, and the bench log shows what
 * that costs: the schedule screen was entered and saved SIX times in one
 * session, because moving from 04:00 to 06:15 is nine taps and moving to an
 * arbitrary time is worse. A carer knows the time they want. Let them type it.
 *
 * Two modes on one screen, so no new AppState is needed and state_machine.c
 * is untouched:
 *
 *   GRID  - the four slots, CLEAR, BACK, SAVE. Tapping a slot opens...
 *   ENTRY - the same numeric keypad as the passcode and the clock, four
 *           digits into HH:MM, OK to commit back to the grid.
 *
 * That makes THREE screens in this UI driven by one keypad, which is the
 * point: a carer learns it once.
 */
#define SCH_SLOT_W    150u
#define SCH_SLOT_H     86u
#define SCH_SLOT_GAP   18u
#define SCH_SLOT_X0    82u
#define SCH_SLOT_Y    170u
#define SCH_SLOT_X(i) ((uint16_t)(SCH_SLOT_X0 + (i) * (SCH_SLOT_W + SCH_SLOT_GAP)))

/* The grid's own action row, clear of the slots above and the footer below. */
#define SCH_CLEAR_X   310u
#define SCH_CLEAR_Y   282u
#define SCH_CLEAR_W   180u
#define SCH_CLEAR_H    74u

static uint16_t s_sch_time[MAX_DOSE_TIMES];
static bool     s_sch_used[MAX_DOSE_TIMES];
static uint8_t  s_sch_sel = 0u;

/* ENTRY mode: which slot is being typed, and the digits so far (HHMM). */
static bool    s_sch_entry = false;
static char    s_sch_dig[5];
static uint8_t s_sch_dig_len = 0u;
static const char *s_sch_note = "";

/* The entry line, and the band repainted on every keypress instead of
 * redrawing the screen. Sits between the subtext line (ending at 122) and
 * the keypad's first row (starting at 150), so it abuts both exactly. */
#define SCH_ENTRY_Y   122u
#define SCH_ENTRY_H    28u

void carer_ui_begin_schedule(void)
{
    memset(s_sch_time, 0, sizeof(s_sch_time));
    memset(s_sch_used, 0, sizeof(s_sch_used));
    s_sch_sel     = 0u;
    s_sch_entry   = false;
    s_sch_dig_len = 0u;
    s_sch_dig[0]  = '\0';
    s_sch_note    = "";
    if (s_sel_slot < 0) return;

    const PatientRecord *p = &patient_gallery[s_sel_slot];
    for (uint8_t i = 0; i < p->dose_time_count && i < MAX_DOSE_TIMES; i++) {
        s_sch_time[i] = p->dose_time[i];
        s_sch_used[i] = true;
    }
}

/* ── ENTRY mode ─────────────────────────────────────────────────────────── */

/* Repaint ONLY the subtext line and the HH:MM entry line. The twelve keys,
 * the frame and the title do not change while four digits are being typed,
 * so they are not redrawn - see the note on clock_refresh_fields(). */
static void sch_entry_fields(void)
{
    const char *sub = (s_sch_note[0] != '\0')
                      ? s_sch_note
                      : "Type the time as four digits, then OK.";

    gui_draw_rect(50u, SUBTEXT_Y, 700u, 22u, THEME_BG);
    gui_font_text_ellipsis(400u, SUBTEXT_Y, sub, 700u,
                           (s_sch_note[0] != '\0') ? THEME_RED_EDGE : THEME_INK_SOFT,
                           &ui_font_sm);

    char d[4];
    for (uint8_t i = 0; i < 4u; i++) {
        d[i] = (i < s_sch_dig_len) ? s_sch_dig[i] : '_';
    }
    char entry[16];
    snprintf(entry, sizeof(entry), "%c%c : %c%c", d[0], d[1], d[2], d[3]);

    gui_draw_rect(250u, SCH_ENTRY_Y, 300u, SCH_ENTRY_H, THEME_BG);
    gui_font_text_centered(400u, SCH_ENTRY_Y, entry,
                           (s_sch_note[0] != '\0') ? THEME_RED_EDGE : THEME_INK,
                           &ui_font_md);
}

static void sch_entry_refresh(void)
{
    sch_entry_fields();
    gui_draw_flush_rows(SUBTEXT_Y, (uint16_t)(SCH_ENTRY_Y + SCH_ENTRY_H));
}

static void draw_schedule_entry(void)
{
    gui_draw_frame();

    char title[32];
    snprintf(title, sizeof(title), "DOSE TIME %u", (unsigned)(s_sch_sel + 1u));
    gui_draw_title_bar(title, ACCENT_DISPENSE);

    sch_entry_fields();

    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            uint16_t edge = THEME_INK_SOFT;
            if (r == 3u && c == 2u) edge = THEME_GREEN_EDGE;   /* OK  */
            if (r == 3u && c == 0u) edge = THEME_AMBER_EDGE;   /* DEL */
            gui_draw_button(KEY_X(c), KEY_Y(r), KEY_W, KEY_H, edge,
                            key_label(r, c), "");
        }
    }
    gui_draw_button(PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H,
                    THEME_INK_SOFT, "CANCEL", "");
    gui_draw_flush();
}

static carer_action_t handle_schedule_entry(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, PW_CANCEL_X, PW_CANCEL_Y, PW_CANCEL_W, PW_CANCEL_H)) {
        s_sch_entry = false;
        s_sch_note  = "";
        carer_ui_draw_schedule();
        return CARER_ACT_NONE;
    }

    for (uint8_t r = 0; r < 4u; r++) {
        for (uint8_t c = 0; c < 3u; c++) {
            if (!hit(tx, ty, KEY_X(c), KEY_Y(r), KEY_W, KEY_H)) continue;
            const char *lbl = key_label(r, c);

            if (lbl[0] == 'D') {                    /* DEL */
                if (s_sch_dig_len > 0u) s_sch_dig[--s_sch_dig_len] = '\0';
                s_sch_note = "";
                sch_entry_refresh();
                return CARER_ACT_NONE;
            }
            if (lbl[0] != 'O') {                    /* a digit */
                if (s_sch_dig_len < 4u) {
                    s_sch_dig[s_sch_dig_len++] = lbl[0];
                    s_sch_dig[s_sch_dig_len]   = '\0';
                }
                s_sch_note = "";
                sch_entry_refresh();
                return CARER_ACT_NONE;
            }

            /* OK */
            if (s_sch_dig_len < 4u) {
                s_sch_note = "Type all four digits, e.g. 0800.";
                sch_entry_refresh();
                return CARER_ACT_NONE;
            }
            uint16_t hh = (uint16_t)(((s_sch_dig[0] - '0') * 10) + (s_sch_dig[1] - '0'));
            uint16_t mm = (uint16_t)(((s_sch_dig[2] - '0') * 10) + (s_sch_dig[3] - '0'));
            if (hh > 23u) {
                s_sch_note = "Hour must be 00 to 23.";
            } else if (mm > 59u) {
                s_sch_note = "Minutes must be 00 to 59.";
            } else {
                s_sch_time[s_sch_sel] = (uint16_t)(hh * 60u + mm);
                s_sch_used[s_sch_sel] = true;
                s_sch_entry = false;
                s_sch_note  = "";
                carer_ui_draw_schedule();
                return CARER_ACT_NONE;
            }
            sch_entry_refresh();
            return CARER_ACT_NONE;
        }
    }
    return CARER_ACT_NONE;
}

/* ── GRID mode ──────────────────────────────────────────────────────────── */

void carer_ui_draw_schedule(void)
{
    if (s_sch_entry) {
        carer_ui_clock_strip_hide();
        draw_schedule_entry();
        return;
    }

    gui_draw_frame();
    gui_draw_title_bar("DOSE TIMES", ACCENT_DISPENSE);
    gui_font_text_ellipsis(400u, SUBTEXT_Y,
                           "Tap a slot to type its time.",
                           700u, THEME_INK_SOFT, &ui_font_sm);
    /* The clock goes UNDER the instruction, in the shared status slot, and
     * ticks while the screen is up - a carer choosing a dose time needs to
     * see what time it is now. These two used to overlap by ten pixels. */
    draw_clock_strip(STATUS_Y);

    for (uint8_t i = 0; i < MAX_DOSE_TIMES; i++) {
        bool sel = (i == s_sch_sel);
        uint16_t edge = s_sch_used[i]
                        ? (sel ? THEME_GREEN_EDGE : THEME_INK_SOFT)
                        : (sel ? THEME_ROSE_EDGE  : THEME_INK_SOFT);
        gui_draw_button(SCH_SLOT_X(i), SCH_SLOT_Y, SCH_SLOT_W, SCH_SLOT_H,
                        edge, "", "");
        char v[8];
        if (s_sch_used[i]) {
            time_source_format_hhmm(v, sizeof(v), s_sch_time[i]);
        } else {
            snprintf(v, sizeof(v), "--:--");
        }
        gui_font_text_centered((uint16_t)(SCH_SLOT_X(i) + SCH_SLOT_W / 2u),
                               (uint16_t)(SCH_SLOT_Y + 22u), v,
                               s_sch_used[i] ? THEME_INK : THEME_INK_SOFT,
                               &ui_font_md);
    }

    gui_draw_button(SCH_CLEAR_X, SCH_CLEAR_Y, SCH_CLEAR_W, SCH_CLEAR_H,
                    THEME_INK_SOFT, "CLEAR", "this slot");

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT, "BACK", "");
    gui_draw_button(FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H, THEME_GREEN_EDGE, "SAVE", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_schedule_touch(uint32_t tx, uint32_t ty)
{
    if (s_sch_entry) {
        return handle_schedule_entry(tx, ty);
    }

    /* Tapping a slot selects it AND opens the keypad - one tap to start
     * typing, rather than select-then-press-edit. */
    for (uint8_t i = 0; i < MAX_DOSE_TIMES; i++) {
        if (hit(tx, ty, SCH_SLOT_X(i), SCH_SLOT_Y, SCH_SLOT_W, SCH_SLOT_H)) {
            s_sch_sel     = i;
            s_sch_entry   = true;
            s_sch_dig_len = 0u;
            s_sch_dig[0]  = '\0';
            s_sch_note    = "";
            carer_ui_draw_schedule();
            return CARER_ACT_NONE;
        }
    }

    if (hit(tx, ty, SCH_CLEAR_X, SCH_CLEAR_Y, SCH_CLEAR_W, SCH_CLEAR_H)) {
        s_sch_used[s_sch_sel] = false;
        s_sch_time[s_sch_sel] = 0u;
        carer_ui_draw_schedule();
        return CARER_ACT_NONE;
    }

    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;

    if (hit(tx, ty, FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H)) {
        uint16_t times[MAX_DOSE_TIMES];
        uint8_t  n = 0u;
        for (uint8_t i = 0; i < MAX_DOSE_TIMES; i++) {
            if (s_sch_used[i]) times[n++] = s_sch_time[i];
        }
        bool ok = gallery_set_schedule(s_sel_slot, times, n);

        char line[SD_LOG_MSG_MAX_LEN];
        snprintf(line, sizeof(line), "CARER: %s schedule set to %u time(s)%s",
                 carer_ui_selected_name(), (unsigned)n,
                 ok ? "" : " (RAM only, no SD)");
        SD_Log_Event_Async(line);
        return ok ? CARER_ACT_ACCEPTED : CARER_ACT_REJECTED;
    }
    return CARER_ACT_NONE;
}

/* ── Dose size ───────────────────────────────────────────────────────────
 * Deliberately the same 1..10 range registration used, so a dose set here
 * and a dose set there mean the same thing. */
#define DOSE_MIN   1u
#define DOSE_MAX  10u
/* 292, not 300: at 300 the 74 px steppers ended exactly on FOOT_Y and the
 * two rows of buttons touched. */
#define DOSE_STEP_Y  292u

static uint8_t s_dose_edit = 1u;

void carer_ui_begin_dose(void)
{
    s_dose_edit = (s_sel_slot >= 0) ? patient_gallery[s_sel_slot].pill_count : 1u;
    if (s_dose_edit < DOSE_MIN) s_dose_edit = DOSE_MIN;
    if (s_dose_edit > DOSE_MAX) s_dose_edit = DOSE_MAX;
}

/* The number and its row of lentils - the only two things a +/- tap changes.
 * Everything else on the screen is static, so a tap repaints 90 + 46 rows
 * rather than all 480. */
#define DOSE_NUM_Y    132u
#define DOSE_NUM_H     90u
#define DOSE_GEMS_Y   245u
#define DOSE_GEMS_H    46u

static void dose_refresh_fields(void)
{
    char v[8];
    snprintf(v, sizeof(v), "%u", (unsigned)s_dose_edit);
    gui_draw_rect(250u, DOSE_NUM_Y, 300u, DOSE_NUM_H, THEME_BG);
    /* ui_font_num is 89 px tall, so this occupies 132..221 and the gem row
     * below it starts at 268. */
    gui_font_text_centered(400u, DOSE_NUM_Y, v, THEME_INK, &ui_font_num);

    /* The same lentils the dispense screen uses, so "3" here and three gems
     * during a dispense are visibly the same fact. Cleared over the widest
     * row DOSE_MAX allows, because the row shrinks as well as grows. */
    gui_draw_rect(140u, DOSE_GEMS_Y, 520u, DOSE_GEMS_H, THEME_BG);
    const uint16_t pitch = 46u;
    const uint16_t x0 = (uint16_t)(400u - (s_dose_edit * pitch) / 2u + pitch / 2u);
    for (uint8_t i = 0; i < s_dose_edit; i++) {
        gui_draw_gem((uint16_t)(x0 + i * pitch), 268u, 17u, GEM_RED);
    }
}

static void dose_refresh(void)
{
    dose_refresh_fields();
    gui_draw_flush_rows(DOSE_NUM_Y, (uint16_t)(DOSE_GEMS_Y + DOSE_GEMS_H));
}

void carer_ui_draw_dose(void)
{
    gui_draw_frame();
    gui_draw_title_bar("DOSE SIZE", ACCENT_DISPENSE);
    gui_font_text_ellipsis(400u, SUBTEXT_Y,
                           "How many pills does this patient take at once?",
                           700u, THEME_INK_SOFT, &ui_font_sm);

    dose_refresh_fields();

    gui_draw_button(STEP_MINUS_X, DOSE_STEP_Y, STEP_W, STEP_H, THEME_AMBER_EDGE, "-", "");
    gui_draw_button(STEP_PLUS_X,  DOSE_STEP_Y, STEP_W, STEP_H, THEME_GREEN_EDGE, "+", "");

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT, "BACK", "");
    gui_draw_button(FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H, THEME_GREEN_EDGE, "SAVE", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_dose_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, STEP_MINUS_X, DOSE_STEP_Y, STEP_W, STEP_H)) {
        s_dose_edit = (s_dose_edit <= DOSE_MIN) ? DOSE_MAX : (uint8_t)(s_dose_edit - 1u);
        dose_refresh();
        return CARER_ACT_NONE;
    }
    if (hit(tx, ty, STEP_PLUS_X, DOSE_STEP_Y, STEP_W, STEP_H)) {
        s_dose_edit = (s_dose_edit >= DOSE_MAX) ? DOSE_MIN : (uint8_t)(s_dose_edit + 1u);
        dose_refresh();
        return CARER_ACT_NONE;
    }
    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;
    if (hit(tx, ty, FOOT_X(3), FOOT_Y, FOOT_W, FOOT_H)) {
        bool ok = gallery_set_dose(s_sel_slot, s_dose_edit);
        char line[SD_LOG_MSG_MAX_LEN];
        snprintf(line, sizeof(line), "CARER: %s dose set to %u pill(s)%s",
                 carer_ui_selected_name(), (unsigned)s_dose_edit,
                 ok ? "" : " (RAM only, no SD)");
        SD_Log_Event_Async(line);
        return ok ? CARER_ACT_ACCEPTED : CARER_ACT_REJECTED;
    }
    return CARER_ACT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 9 - log review
 *
 * This is the strongest thing in carer mode, and it is the reason the audit
 * trail has existed from the start. Until now, reading events.log meant
 * taking the card out and putting it in a laptop. A carer standing at the
 * device can now see whether the doses were taken.
 *
 * The screen shows only the lines that answer that question - CONFIRMED,
 * MISSED, DISPENSE, SKIPPED - filtered to the selected patient when one is
 * selected. Every STATE: line and every diagnostic is dropped: they are
 * genuinely useful over UART and they are noise to a carer.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 4 KB of the end of the log is roughly a hundred lines, which after
 * filtering is comfortably more than the eight this screen shows. Static
 * rather than a local: the UI task's stack is 8 KB and this would take half
 * of it. */
#define LOG_TAIL_BYTES   4096u
#define LOG_LINES_SHOWN     8u
#define LOG_LINE_MAX       64u

static uint8_t s_log_buf[LOG_TAIL_BYTES];
static char    s_log_line[LOG_LINES_SHOWN][LOG_LINE_MAX];
static uint8_t s_log_count = 0u;
static bool    s_log_read_ok = false;

static bool line_is_interesting(const char *line)
{
    return (strstr(line, "CONFIRMED") != NULL)
        || (strstr(line, "MISSED")    != NULL)
        || (strstr(line, "DISPENSE:") != NULL)
        || (strstr(line, "SKIPPED")   != NULL);
}

void carer_ui_begin_log(void)
{
    s_log_count   = 0u;
    s_log_read_ok = false;

    uint32_t br = 0u;
    if (!SD_Read_File_Tail("events.log", s_log_buf, LOG_TAIL_BYTES - 1u, &br)) {
        return;
    }
    s_log_read_ok = true;
    s_log_buf[br] = '\0';

    /* The first line in a tail read is almost always a partial one - skip to
     * the first newline before parsing anything. */
    char *p = (char *)s_log_buf;
    if (br == LOG_TAIL_BYTES - 1u) {
        char *nl = strchr(p, '\n');
        if (nl != NULL) p = nl + 1;
    }

    const char *want = (s_sel_slot >= 0) ? carer_ui_selected_name() : NULL;

    /* Walk forward keeping the LAST LOG_LINES_SHOWN matches, in a ring, so a
     * single pass gets the most recent ones without ever holding the whole
     * file. */
    uint8_t head = 0u;
    uint8_t seen = 0u;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');
        if (nl != NULL) *nl = '\0';

        if (line_is_interesting(p) &&
            ((want == NULL) || (want[0] == '\0') || (strstr(p, want) != NULL))) {
            strncpy(s_log_line[head], p, LOG_LINE_MAX - 1u);
            s_log_line[head][LOG_LINE_MAX - 1u] = '\0';
            head = (uint8_t)((head + 1u) % LOG_LINES_SHOWN);
            if (seen < LOG_LINES_SHOWN) seen++;
        }

        if (nl == NULL) break;
        p = nl + 1;
    }

    /* Unroll the ring into display order, oldest first. */
    if (seen == LOG_LINES_SHOWN) {
        char tmp[LOG_LINES_SHOWN][LOG_LINE_MAX];
        for (uint8_t i = 0; i < LOG_LINES_SHOWN; i++) {
            memcpy(tmp[i], s_log_line[(head + i) % LOG_LINES_SHOWN], LOG_LINE_MAX);
        }
        memcpy(s_log_line, tmp, sizeof(s_log_line));
    }
    s_log_count = seen;
}

void carer_ui_draw_log(void)
{
    gui_draw_frame();
    gui_draw_title_bar("DOSE HISTORY", ACCENT_NEUTRAL);

    if (s_sel_slot >= 0) {
        gui_font_text_ellipsis(400u, SUBTEXT_Y, carer_ui_selected_name(), 700u,
                               THEME_INK_SOFT, &ui_font_sm);
    }

    if (!s_log_read_ok) {
        gui_font_text_centered(400u, 200u,
                               "No SD card, or no log on it yet.\n"
                               "Nothing has been recorded.",
                               THEME_RED_EDGE, &ui_font_md);
    } else if (s_log_count == 0u) {
        gui_font_text_centered(400u, 200u,
                               "No doses recorded yet.",
                               THEME_INK_SOFT, &ui_font_md);
    } else {
        for (uint8_t i = 0; i < s_log_count; i++) {
            /* A missed dose is the line a carer opened this screen to find,
             * so it is the one line that is not grey. */
            uint16_t col = (strstr(s_log_line[i], "MISSED") != NULL)
                           ? THEME_RED_EDGE : THEME_INK;
            /* Starts below the header block and the patient name; 30 px
             * pitch for a 22 px face gives eight lines from 130 to 340,
             * clear of the footer at FOOT_Y. */
            gui_font_text_ellipsis(400u, (uint16_t)(130u + i * 30u),
                                   s_log_line[i], 690u, col, &ui_font_sm);
        }
    }

    gui_draw_button(FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H, THEME_INK_SOFT, "BACK", "");
    gui_draw_flush();
}

carer_action_t carer_ui_handle_log_touch(uint32_t tx, uint32_t ty)
{
    if (hit(tx, ty, FOOT_X(0), FOOT_Y, FOOT_W, FOOT_H)) return CARER_ACT_BACK;
    return CARER_ACT_NONE;
}
