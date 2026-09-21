/* intake_hand.c - Stage 1C. See Inc/ai/intake_hand.h for why this exists and
 * why it does not use the NPU; this file is the mechanism only. */

#include "ai/intake_hand.h"
#include <string.h>

#if MEDSIGHT_ACTION_RECOGNITION

/* ── THE WORKING GRID ─────────────────────────────────────────────────────
 *
 * 40x40, decimated 4x from the 160x160 crop. A hand spans ~64 px there, so it
 * is still 16 cells across at this resolution - there is nothing to gain from
 * working at full size and a 16x cost to pay for it. Two 1600-byte frames in
 *.bss, which matters on a part where AXISRAM is the scarce resource.
 *
 * The decimation is a plain stride, not an average. Averaging would soften
 * exactly the edges this is looking for. */
#define G          40
#define G_CELLS    (G * G)

/* ══════════════════════════════════════════════════════════════════════════
 * WHY THIS SUBTRACTS A BASELINE AND NOT THE PREVIOUS FRAME
 * ══════════════════════════════════════════════════════════════════════════
 *
 * The first version differenced consecutive frames. On hardware it found a
 * hand in 0 of 316 frames across two doses, while reporting mean skin 21% and
 * mean motion 7% - both masks working, their combination never qualifying.
 *
 * The reason is a property of frame differencing that is easy to forget:
 *
 *     A SMOOTH OBJECT IN MOTION ONLY LIGHTS UP ITS OWN EDGES.
 *
 * The interior of a moving hand is hand-in-frame-N and hand-in-frame-N+1, so
 * it differences to zero. What survives is a thin arc where the boundary
 * swept - a large bounding box containing very few lit cells. The density
 * gate added to reject scattered noise rejected that too, because on the two
 * measurements it looks at, a moving edge and sprinkled noise are the same
 * shape. The gate was not wrong; the signal was.
 *
 * Subtracting a BASELINE fixes it at the source. Compared against a still
 * reference, the whole AREA of the hand differs, not just its rim - so the
 * blob is solid, the density gate passes on a hand and still fails on noise,
 * and the two cases separate cleanly instead of overlapping.
 *
 * It also disposes of the face for free, which the previous-frame version was
 * relying on motion to do. The face is IN the baseline. So is the wall, the
 * lamp and the doorframe. Anything that was there when the watch started
 * cancels, and what remains is what ARRIVED - which is exactly the question
 * being asked.
 *
 * ── WHAT A BASELINE COSTS, AND HOW THAT IS PAID ──────────────────────────
 *
 * A static reference drifts: the ISP keeps converging during a watch (on
 * purpose - see intake_camera.c), and a slow exposure change would eventually
 * light up the entire frame. So the baseline is adapted towards the current
 * frame, ONE EIGHTH per frame, but only on frames where nothing was found. On
 * a frame with a hand in it, adapting would quietly absorb the hand into the
 * background and the detection would fade out while the hand was still there.
 *
 * Adapt when you see nothing; hold when you see something. */
#define REF_ADAPT_SHIFT  3       /* nothing found: ref += (cur - ref) >> 3  */

/* ── THE BASELINE MUST BLEED EVEN WHILE SOMETHING IS FOUND ───────────────
 *
 * The first version adapted ONLY on frames where nothing was found, on the
 * reasoning that adapting during a detection would absorb the hand. That
 * reasoning is right and the rule built from it was a trap:
 *
 *   ONCE ANYTHING FALSE IS DETECTED, IT IS DETECTED FOREVER.
 *
 * The baseline stops adapting *because* of the false detection, so the thing
 * that caused it never gets absorbed, so it is found again next frame. On
 * hardware this presented exactly as its mechanism predicts: with a hand in
 * frame the stage works, and the moment the hand is WITHDRAWN the box jumps
 * to the EAR and stays there for the rest of the watch.
 *
 * The ear is what it lands on because the ear/jaw boundary is the
 * highest-contrast skin edge on a face, so it is the first thing to differ
 * from the baseline once the hand that was shading and re-exposing the scene
 * leaves it. That blob is skin, solid and compact: it passes every gate this
 * stage has. And having passed, it freezes the baseline that would otherwise
 * have absorbed it.
 *
 * So the baseline now always bleeds, 64x more slowly while something is
 * found than while nothing is. A genuine hand is present for about a second
 * and survives that easily; a false blob decays out over a few seconds. */
#define REF_ADAPT_SHIFT_FOUND  6   /* found: ref += (cur - ref) >> 6       */

/* ── STUCK FOREGROUND, MEASURED PER CELL ─────────────────────────────────
 *
 * The instinct is right - a delivery travels and a re-lit face edge does not
 * - but the first attempt tested it at the wrong level. It asked whether the
 * largest COMPONENT's centroid had moved between frames, and on hardware
 * that fired exactly zero times in 312 frames while the ear was being
 * tracked throughout.
 *
 * The reason is that "the largest component" is not a stable object. Cells
 * flicker in and out at the threshold boundary, so consecutive frames pick
 * slightly different regions, and the centroid of a region that is not the
 * same region twice jumps around freely. A test for stillness applied to
 * something with no persistent identity measures nothing.
 *
 * PER CELL there is no identity problem. A cell is either foreground this
 * frame or it is not, and a cell that has been foreground for a long time is
 * part of the scene by definition - whatever component it currently belongs
 * to. This is the standard background-subtraction answer to stuck foreground,
 * and it degrades gracefully: the ear erodes cell by cell as each one reaches
 * the limit, rather than vanishing all at once.
 *
 * STUCK_FRAMES is set from how long a cell is legitimately covered. A hand
 * crossing the ROI covers any given cell for perhaps 5-20 frames. A hand held
 * at the lips covers cells for as long as it is held - and that is absorbed
 * too, correctly, because SIMPLE_HOLD needs three frames and the verdict was
 * recorded a second earlier. */
#define STUCK_FRAMES       40      /* ~1.3 s at the 33 ms loop period      */

/* ── HOW FAR FROM THE MOUTH A CANDIDATE MAY SIT ────────────────────────
 *
 * The ROI is CENTRED ON THE MOUTH (intake_camera.c), so the centre of this
 * grid is the mouth and distance from the centre is distance from the mouth,
 * for free and without this stage needing to be told where the mouth is.
 *
 * The reason this gate exists is a limit worth stating rather than hiding:
 *
 *   A MOVING EAR IS A MOVING SKIN REGION. At the level this stage works -
 *   skin-toned, solid, differs from the baseline - an ear revealed by a turn
 *   of the head is INDISTINGUISHABLE from a hand. No threshold on size,
 *   density or colour separates them, because on every one of those
 *   measurements they are the same kind of thing.
 *
 * What separates them is position, and only position. The grid spans 1.5
 * face widths across 40 cells, so one cell is 0.0375 face widths:
 *
 *     mouth centre -> ear         ~0.45-0.55 face widths  = 12-15 cells
 *     hand centroid at delivery   ~0.27-0.40 face widths  =  7-11 cells
 *     grid corner                        0.75 face widths = 20 cells
 *
 * 14 cells (0.53 face widths) sits above a delivery and at the near edge of
 * an ear.
 *
 * WHAT THIS COSTS: a hand entering from the edge of the ROI is not tracked
 * until it is already fairly close, so the APPROACH is partly invisible.
 * That is acceptable for MEDSIGHT_INTAKE_SIMPLE, which needs only the
 * arrival. If the full state machine is ever switched back on, its
 * APPROACHING state needs the early part of the trajectory and this gate
 * should be widened, or moved into the features stage where the real mouth
 * position is known rather than assumed to be the ROI centre. */
#define MAX_CENTRE_CELLS   14

/* ── THE THRESHOLDS ──────────────────────────────────────────────────────
 *
 * DIFF_T is a per-cell luminance delta against the baseline. The measured ROI
 * mean is 91..119 now that the ISP settles properly, so a hand - which is
 * usually brighter or darker than whatever it covers - clears 18 easily,
 * while sensor noise on this sensor sits well under it.
 *
 * The skin rule is RATIO-based rather than absolute. The usual published rule
 * opens with R > 95, which frames at these levels fail for genuinely
 * skin-toned pixels. What survives poor exposure is the ORDERING of the
 * channels: skin is red-dominant and stays red-dominant in the dark. Measured
 * on hardware this rule marks 21% of the ROI as skin, which is about what the
 * face alone should occupy - so it is neither too tight nor too loose. */
#define DIFF_T          18
#define SKIN_MIN_R      45
#define SKIN_R_OVER_G    6
#define SKIN_R_OVER_B   10

/* ── THE NOISE GATES ─────────────────────────────────────────────────────
 *
 * A hand spans ~64 px of a 160 px ROI: 16x16 = 256 cells. Even a partial one,
 * entering from the edge, clears MIN_CELLS comfortably.
 *
 * MIN_DENSITY is the gate that separates a hand from noise, and it is only
 * meaningful now that the baseline makes a hand SOLID. It measures lit cells
 * as a fraction of their own bounding box: a hand fills most of its box,
 * noise scattered across the same box fills a few percent.
 *
 * MAX_CELLS catches the case the baseline cannot - a genuine exposure step,
 * where everything changes at once. */
#define MIN_CELLS       50
/* Lowered from 35. The best blob a real hand produced on hardware measured
 * 28% against the whole-frame bounding box; a connected component is a
 * tighter box than that, so a hand should now clear this comfortably while
 * scattered noise - which no longer forms a single large component at all -
 * cannot reach it. */
#define MIN_DENSITY_PCT 25
#define MAX_CELLS       ((G_CELLS * 2) / 3)
#define WARMUP_FRAMES   6

static uint8_t  s_ref[G_CELLS];
static bool     s_have_ref;
static uint32_t s_frames, s_hand_frames, s_warmup;
static uint32_t s_skin_acc, s_fg_acc;

/* Why each rejection happened, and the best blob ever seen. The previous
 * round's diagnostics said skin and motion were both healthy and could not
 * say why their combination never fired, which cost a hardware round. These
 * name the gate. */
static uint32_t s_rej_small, s_rej_sparse, s_rej_flood, s_rej_far;
static uint8_t  s_streak[G_CELLS];   /* consecutive frames each cell was fg */
static uint32_t s_absorbed;          /* cells taken into the baseline       */
static uint8_t  s_cover_pct;         /* last mouth-patch occlusion, 0..100  */
static uint8_t  s_cover_best;
static uint16_t s_best_fg, s_best_density;

void intake_hand_reset(void)
{
    s_have_ref    = false;
    s_frames      = 0u;
    s_hand_frames = 0u;
    s_warmup      = 0u;
    s_skin_acc    = 0u;
    s_fg_acc      = 0u;
    s_rej_small = s_rej_sparse = s_rej_flood = s_rej_far = 0u;
    s_absorbed = 0u;
    s_cover_pct = s_cover_best = 0u;
    memset(s_streak, 0, sizeof(s_streak));
    s_best_fg = s_best_density = 0u;
    memset(s_ref, 0, sizeof(s_ref));
}

void intake_hand_stats(uint32_t *frames_with_hand,
                       uint8_t *mean_skin_pct, uint8_t *mean_motion_pct)
{
    if (frames_with_hand) *frames_with_hand = s_hand_frames;
    if (mean_skin_pct) {
        *mean_skin_pct = (s_frames > 0u)
                       ? (uint8_t)(s_skin_acc / s_frames) : 0u;
    }
    if (mean_motion_pct) {
        *mean_motion_pct = (s_frames > 0u)
                         ? (uint8_t)(s_fg_acc / s_frames) : 0u;
    }
}

void intake_hand_debug(uint16_t *best_cells, uint16_t *best_density_pct,
                       uint32_t *rej_small, uint32_t *rej_sparse,
                       uint32_t *rej_flood, uint32_t *rej_stale,
                       uint32_t *rej_far)
{
    if (best_cells)       *best_cells       = s_best_fg;
    if (best_density_pct) *best_density_pct = s_best_density;
    if (rej_small)        *rej_small        = s_rej_small;
    if (rej_sparse)       *rej_sparse       = s_rej_sparse;
    if (rej_flood)        *rej_flood        = s_rej_flood;
    if (rej_stale)        *rej_stale        = s_absorbed;
    if (rej_far)          *rej_far          = s_rej_far;
}

uint8_t intake_hand_mouth_cover(uint8_t *best)
{
    if (best) *best = s_cover_best;
    return s_cover_pct;
}

static inline bool is_skin(int r, int g, int b)
{
    return (r > SKIN_MIN_R) &&
           (r > (g + SKIN_R_OVER_G)) &&
           (r > (b + SKIN_R_OVER_B)) &&
           (g >= b);
}

bool intake_hand_update(const uint8_t *chw, int size,
                         int mouth_half_cells, pill_obs_t *out)
{
    if ((chw == NULL) || (size <= 0) || (out == NULL)) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const int plane = size * size;
    const uint8_t *R  = &chw[0 * plane];
    const uint8_t *Gp = &chw[1 * plane];
    const uint8_t *B  = &chw[2 * plane];

    const int step = size / G;          /* 4 at the shipped 160 */
    if (step <= 0) {
        return false;
    }

    /*.bss, not the stack. The AI task is created with 2048 WORDS - 8 KB
     * (main.c) - and these three grids are 4,800 bytes between them. Putting
     * them on the stack left under half the task stack for the rest of the
     * call chain, which is the kind of margin that fails as a hard fault in
     * some unrelated function months later. Nothing here is reentrant: one
     * task, one call at a time. */
    static uint8_t cur[G_CELLS];
    static bool    skin[G_CELLS];
    int     n_skin = 0;

    for (int gy = 0; gy < G; gy++) {
        const int sy = gy * step;
        for (int gx = 0; gx < G; gx++) {
            const int si = (sy * size) + (gx * step);
            const int gi = (gy * G) + gx;
            const int r = R[si], g = Gp[si], b = B[si];
            cur[gi]  = (uint8_t)g;      /* green as luminance: it carries the
                                         * most bits in RGB565 and needs no
                                         * multiply */
            skin[gi] = is_skin(r, g, b);
            if (skin[gi]) n_skin++;
        }
    }

    s_frames++;
    s_skin_acc += (uint32_t)((n_skin * 100) / G_CELLS);

    /* The baseline is taken AFTER the ISP has had a few frames to converge on
     * the restarted pipe. Capturing it on frame one would bake an exposure
     * ramp into the reference and every later frame would differ from it. */
    if (!s_have_ref) {
        if (s_warmup < WARMUP_FRAMES) {
            s_warmup++;
            return false;
        }
        memcpy(s_ref, cur, sizeof(cur));
        s_have_ref = true;
        return false;
    }

    /* ── ILLUMINATION-INVARIANT FOREGROUND ───────────────────────────────
     *
     * A plain |cur - ref| lit up the ENTIRE face whenever the ISP moved, and
     * on hardware it moved constantly: ROI mean swung 85..127 within a single
     * watch, because a hand entering the scene changes what the auto-exposure
     * is metering. 418 of 429 frames were thrown out as floods.
     *
     * A uniform brightness change adds the SAME offset to every cell, so
     * subtracting the difference of the two means removes it exactly and
     * leaves whatever changed locally. Two sums over 1600 bytes; nothing that
     * shows up against an NPU pass. */
    int mean_cur = 0, mean_ref = 0;
    for (int i = 0; i < G_CELLS; i++) {
        mean_cur += cur[i];
        mean_ref += s_ref[i];
    }
    const int bias = (mean_cur - mean_ref) / G_CELLS;

    static bool fg[G_CELLS];   /*.bss - see cur/skin above */
    int  n_fg = 0;
    for (int i = 0; i < G_CELLS; i++) {
        fg[i] = false;
        if (!skin[i]) continue;
        int d = ((int)cur[i] - (int)s_ref[i]) - bias;
        if (d < 0) d = -d;
        if (d <= DIFF_T) continue;
        fg[i] = true;
        n_fg++;
    }

    /* ── ABSORB STUCK CELLS BEFORE ANYTHING IS LABELLED ──────────────────
     * Done here, not after, so an absorbed cell cannot contribute to a
     * component on the very frame it is retired. */
    for (int i = 0; i < G_CELLS; i++) {
        if (!fg[i]) {
            s_streak[i] = 0u;
            continue;
        }
        if (s_streak[i] < 255u) {
            s_streak[i]++;
        }
        if (s_streak[i] >= STUCK_FRAMES) {
            s_ref[i]    = cur[i];    /* it IS the background now */
            s_streak[i] = 0u;
            fg[i]       = false;
            n_fg--;
            s_absorbed++;
        }
    }

    /* ══════════════════════════════════════════════════════════════════════
     * MOUTH OCCLUSION - THE MEASUREMENT THE VERDICT NOW RESTS ON
     * ══════════════════════════════════════════════════════════════════════
     *
     * Everything above searches the whole ROI for an object and then asks
     * where it is. That is why an ear kept winning: a search has more places
     * to go wrong than the question actually being asked has.
     *
     * This inverts it. The mouth position is already known for free from the
     * CenterFace landmarks, and intake_camera.c centres the ROI on it - so
     * the mouth is at the CENTRE OF THIS GRID, and the only question left is
     * whether that one small patch is covered.
     *
     * An ear is not in the patch. A hand at the temple is not in the patch.
     * A turned head does not put anything in the patch. The whole class of
     * failure that four hardware rounds were spent on stops existing by
     * construction rather than by threshold - which is the difference
     * between a fix and another constant.
     *
     * What counts as covered: a patch cell that differs from the baseline
     * AND reads as skin. Both, because the lips themselves are skin (so
     * colour alone is always true there) and a shadow crossing the mouth
     * differs from the baseline without anything arriving (so difference
     * alone is not enough either).
     *
     * The stuck-cell absorption above still applies, so a patch that stays
     * covered for STUCK_FRAMES is retired into the baseline and stops
     * reading as covered. That is correct: SIMPLE_HOLD needs three frames,
     * so a real delivery has been recorded a second before, and a hand
     * resting on a chin indefinitely should not hold the verdict open. */
    if (mouth_half_cells > 0) {
        int hc = mouth_half_cells;
        if (hc > (G / 4)) hc = G / 4;          /* never more than a quarter */
        const int c0 = (G / 2) - hc, c1 = (G / 2) + hc;
        int in_patch = 0, covered = 0;
        for (int gy = c0; gy <= c1; gy++) {
            if ((gy < 0) || (gy >= G)) continue;
            for (int gx = c0; gx <= c1; gx++) {
                if ((gx < 0) || (gx >= G)) continue;
                in_patch++;
                if (fg[(gy * G) + gx]) covered++;
            }
        }
        s_cover_pct = (in_patch > 0)
                    ? (uint8_t)((covered * 100) / in_patch) : 0u;
        if (s_cover_pct > s_cover_best) {
            s_cover_best = s_cover_pct;
        }
    } else {
        s_cover_pct = 0u;
    }

    s_fg_acc += (uint32_t)((n_fg * 100) / G_CELLS);

    /* ── THE LARGEST CONNECTED REGION, NOT THE BOUNDING BOX OF EVERYTHING ─
     *
     * The previous version measured the bounding box of every lit cell at
     * once. That is the wrong shape to measure: a hand plus a handful of
     * stray cells in the far corner produces a box spanning the whole grid,
     * and the hand is then judged - and rejected - on the stray cells'
     * geometry rather than its own.
     *
     * A flood fill answers the question actually being asked: is there ONE
     * region big and solid enough to be a hand? Scattered noise produces many
     * tiny components and no large one; a hand produces exactly one. The
     * stack is 4-connected, iterative and bounded by the grid, so there is no
     * recursion and no allocation. */
    static int16_t stack[G_CELLS];
    static uint8_t seen[G_CELLS];
    memset(seen, 0, sizeof(seen));

    int best_n = 0, best_minx = 0, best_miny = 0, best_maxx = 0, best_maxy = 0;
    int best_sx = 0, best_sy = 0;

    for (int s0 = 0; s0 < G_CELLS; s0++) {
        if (!fg[s0] || seen[s0]) continue;

        int sp = 0;
        stack[sp++] = (int16_t)s0;
        seen[s0] = 1u;

        int n = 0, sxs = 0, sys = 0;
        int mnx = G, mny = G, mxx = -1, mxy = -1;

        while (sp > 0) {
            const int ci = stack[--sp];
            const int cy = ci / G, cx = ci % G;
            n++;
            sxs += cx; sys += cy;
            if (cx < mnx) mnx = cx;
            if (cy < mny) mny = cy;
            if (cx > mxx) mxx = cx;
            if (cy > mxy) mxy = cy;

            if ((cx > 0)       && fg[ci - 1] && !seen[ci - 1]) { seen[ci - 1] = 1u; stack[sp++] = (int16_t)(ci - 1); }
            if ((cx < (G - 1)) && fg[ci + 1] && !seen[ci + 1]) { seen[ci + 1] = 1u; stack[sp++] = (int16_t)(ci + 1); }
            if ((cy > 0)       && fg[ci - G] && !seen[ci - G]) { seen[ci - G] = 1u; stack[sp++] = (int16_t)(ci - G); }
            if ((cy < (G - 1)) && fg[ci + G] && !seen[ci + G]) { seen[ci + G] = 1u; stack[sp++] = (int16_t)(ci + G); }
        }

        if (n > best_n) {
            best_n = n;
            best_sx = sxs; best_sy = sys;
            best_minx = mnx; best_miny = mny;
            best_maxx = mxx; best_maxy = mxy;
        }
    }

    bool found = false;
    int  density = 0;

    if (n_fg > MAX_CELLS) {
        s_rej_flood++;              /* a genuine exposure blowout */
    } else if (best_n < MIN_CELLS) {
        s_rej_small++;
    } else {
        const int bw = (best_maxx - best_minx) + 1;
        const int bh = (best_maxy - best_miny) + 1;
        density = (best_n * 100) / (bw * bh);
        if (best_n > (int)s_best_fg) {
            s_best_fg      = (uint16_t)best_n;
            s_best_density = (uint16_t)density;
        }
        if (density < MIN_DENSITY_PCT) {
            s_rej_sparse++;
        } else {
            const int cx_g = best_sx / best_n;
            const int cy_g = best_sy / best_n;

            /* Too far from the mouth to be a delivery. See MAX_CENTRE_CELLS.
             * Rejected HERE rather than in the features stage for two
             * reasons: a distant blob cannot then win the largest-component
             * pick and hide a real hand behind it, and the overlay does not
             * draw a box on an ear the machine has already discounted. */
            {
                const int dcx = cx_g - (G / 2);
                const int dcy = cy_g - (G / 2);
                if (((dcx * dcx) + (dcy * dcy)) >
                    (MAX_CENTRE_CELLS * MAX_CENTRE_CELLS)) {
                    s_rej_far++;
                    return false;
                }
            }

            found = true;
            const int half = step / 2;

            out->detected = true;
            out->cx = (int16_t)((cx_g * step) + half);
            out->cy = (int16_t)((cy_g * step) + half);
            out->x  = (int16_t)(best_minx * step);
            out->y  = (int16_t)(best_miny * step);
            out->w  = (int16_t)(bw * step);
            out->h  = (int16_t)(bh * step);

            /* Confidence is coverage, saturating. A hand well inside the ROI
             * covers roughly a sixth of the grid; at or past that this stage
             * is as certain as it gets, and the FSM only ever compares it
             * against a threshold. */
            float c = ((float)best_n / (float)G_CELLS) / 0.16f;
            if (c > 1.0f) c = 1.0f;
            out->confidence = c;

            s_hand_frames++;
        }
    }
    /* Always bleed; only the rate depends on whether something was found. */
    {
        const int sh = found ? REF_ADAPT_SHIFT_FOUND : REF_ADAPT_SHIFT;
        for (int i = 0; i < G_CELLS; i++) {
            s_ref[i] = (uint8_t)((int)s_ref[i] +
                                 (((int)cur[i] - (int)s_ref[i]) >> sh));
        }
    }
    return found;
}

#else  /* !MEDSIGHT_ACTION_RECOGNITION */

void intake_hand_reset(void) { }
bool intake_hand_update(const uint8_t *chw, int size, int mouth_half_cells,
                         pill_obs_t *out)
{ (void)chw; (void)size; (void)mouth_half_cells;
  if (out) memset(out, 0, sizeof(*out)); return false; }
uint8_t intake_hand_mouth_cover(uint8_t *best) { if (best) *best = 0u; return 0u; }
void intake_hand_stats(uint32_t *f, uint8_t *s, uint8_t *m)
{ if (f) *f = 0u; if (s) *s = 0u; if (m) *m = 0u; }
void intake_hand_debug(uint16_t *a, uint16_t *b, uint32_t *c, uint32_t *d,
                       uint32_t *e, uint32_t *f, uint32_t *g)
{ if (a) *a = 0u; if (b) *b = 0u; if (c) *c = 0u; if (d) *d = 0u; if (e) *e = 0u;
  if (f) *f = 0u; if (g) *g = 0u; }

#endif /* MEDSIGHT_ACTION_RECOGNITION */
