/* intake_handnet.c - Stage 1C. See Inc/ai/intake_handnet.h for what this
 * replaces, why, and what the four outputs mean; this file is mechanism. */

#include "ai/intake_handnet.h"

#if MEDSIGHT_ACTION_RECOGNITION

#include "stai_hand.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>
#include <string.h>

/* Which generated output is which. Determined by RUNNING the generated graph
 * on known inputs rather than inferred from the names, because the names are
 * `Dequantize_<n>_out_0` and carry no meaning:
 *
 *     face image  -> OUT_3 = 0.0078,  OUT_1 = 0.5273
 *     noise       -> OUT_3 = 0.0078,  OUT_1 = 0.6445
 *     flat grey   -> OUT_3 = 0.0117,  OUT_1 = 0.7148
 *
 * OUT_3 is the one pinned near zero on everything that is not a hand, so OUT_3
 * is presence. OUT_4 carries values like 103.9, 175.2, 0.0 - pixel coordinates
 * in the 224 input space - so OUT_4 is the landmark set. */
#define OUT_PRESENCE   2      /* zero-based index of OUT_3 */
#define OUT_LANDMARKS  3      /* zero-based index of OUT_4 */

/* Above this, a hand is present. 0.50 originally; 0.30 once hardware had
 * measured both sides.
 *
 * The offline evidence only ever covered the NEGATIVE side - a face 0.0078,
 * noise 0.0078, flat grey 0.0117 - so 0.50 was set deliberately far from that
 * floor rather than close to it, because a threshold set tight against
 * UNMEASURED positives is exactly how the pill detector's bar got set wrong.
 *
 * Hardware then supplied the missing half, and it says two things:
 *
 *   A REAL DELIVERY PEAKS AT ABOUT 0.50. Three doses in a row reported a best
 *   presence of exactly 50%, i.e. barely clearing the bar, and were detected
 *   on only 2 frames each. The model is trained on TIGHT hand crops from a
 *   palm detector and is being given a 1.5-face-width one, so it half-sees a
 *   hand rather than missing it.
 *
 *   AN EAR IS NOT DETECTED AT ALL. The confusor that defeated four rounds of
 *   skin heuristics does not reach this threshold, and neither does a face.
 *
 * Which means the absolute score never mattered - the SEPARATION does. There
 * is a wide empty band between 0.012 and 0.50 with nothing in it, and moving
 * into that band buys detections without buying false ones. 0.30 sits in the
 * middle of the gap: 25x the highest non-hand score ever measured, and well
 * under the 0.50 a real delivery reaches.
 *
 * If a future round ever reports a non-hand scoring near 0.30, this goes back
 * up and the crop gets tighter instead - that is the other way to give this
 * model what it was trained on. */
/* 0.50 -> 0.30 -> 0.15, each step justified by a measurement rather than a
 * hope. The last step goes with widening the ROI to 250% of face width
 * (intake_camera.c): a hand occupies 24% of the input there instead of 39%,
 * and a smaller hand scores lower, so the bar comes down to meet it.
 *
 * There is room for this because the NEGATIVE side is measured and is nowhere
 * near: a face 0.0078, random noise never above 0.035 over 400 images, an ear
 * confirmed on hardware not to fire at all. 0.15 is still an order of
 * magnitude above the highest non-hand score ever recorded here.
 *
 * The band histogram is the check. If a widened crop starts putting frames in
 * <.2 that are NOT hands, this goes back up and the answer becomes a second
 * inference on a tracked sub-window instead - which is what MediaPipe's own
 * palm-detector-then-landmarks pipeline does, and what we are approximating
 * with one pass. */
/* Back to 0.30 with the ROI back at 150%: a hand at the mouth is ~39% of the
 * model input again, not 24%, so the bar does not need to be as low. 0.30 is
 * still 25x the highest non-hand score ever measured here (a face 0.0078, 400
 * random images never above 0.035, and an ear confirmed on hardware not to
 * fire). */
#define PRESENCE_T     0.30f

static stai_network s_net[STAI_HAND_CONTEXT_SIZE]
    __attribute__((aligned(8)));
static stai_ptr  s_in[STAI_HAND_IN_NUM];
static stai_ptr  s_out[STAI_HAND_OUT_NUM];
static const stai_size s_out_bytes[STAI_HAND_OUT_NUM] = STAI_HAND_OUT_SIZES_BYTES;

static bool     s_ready;
static uint32_t s_last_ms;
static float    s_best_conf;

/* ── WHY THE DISTRIBUTION IS MEASURED AND NOT JUST THE PEAK ──────────────
 *
 * Five watches have now reported a best presence of EXACTLY 50%, including
 * two after the acceptance threshold was lowered from 0.50 to 0.30. If 0.50
 * were a real confidence level, lowering the bar would have surfaced frames
 * at 0.31, 0.44, 0.67; instead the count moved 2 -> 3 and the maximum did not
 * move at all.
 *
 * A score that never exceeds one value is a CEILING, not a confidence. Either
 * the output saturates there or it is being read wrongly - and a single
 * summary number cannot tell those apart from a genuinely bimodal signal.
 *
 * So the whole distribution gets reported: how many frames landed in each
 * band, and the peak to four decimal places rather than as a rounded
 * percentage. Reading it:
 *
 *   everything in b00, peak ~0.0117   the model never sees a hand at all
 *   a spread across b10..b50          a real confidence signal; set the
 *                                     threshold from where the modes divide
 *   b00 and b50 only, peak 0.5000     bimodal with nothing between: the
 *                                     value is behaving like a flag rather
 *                                     than a confidence
 *
 * THE RANGE IS KNOWN, so 0.5 can be placed exactly. The presence tensor is
 * int8 with scale 0.00390625 and zero point -128, i.e. the float is
 * (q + 128) / 256. So:
 *
 *     q = -128  ->  0.000    q = -126  ->  0.0078 (a face, measured)
 *     q =    0  ->  0.500    q =  +127 ->  0.996  (the true ceiling)
 *
 * 0.5 is therefore NOT saturation - an earlier reading of this
 * guessed that it was, and was wrong. 0.5 is q = 0, the midpoint: 128
 * quantisation steps above the noise floor (400 random inputs never exceeded
 * 0.035) and 127 steps below the maximum. A genuine mid-range confidence.
 *
 * What remains odd, and is what these bands are for, is that the PEAK has
 * been exactly 0.5 on five separate watches. A real-valued score should
 * scatter - 0.52, 0.58, 0.61. Landing on q = 0 every time is either a
 * genuine mode of this model on wide crops, or a sign the value is being
 * read on a frame boundary rather than at the peak of the gesture. The
 * distribution distinguishes those; a single maximum cannot. */
static uint32_t s_band[6];   /* <0.1, <0.2, <0.3, <0.4, <0.5, >=0.5 */

/* ── IS THE LOCALISATION REAL? ───────────────────────────────────────────
 *
 * Observed on hardware: the overlay box NEVER appears anywhere except
 * directly on the mouth - not on the hand as it approaches, not anywhere
 * else in the ROI.
 *
 * That is exactly what a degenerate output looks like here, because the ROI
 * is CENTRED ON THE MOUTH: a model returning coordinates near the middle of
 * its own input maps back onto the mouth every time, whatever the hand is
 * doing. And it is the expected failure of this particular model on this
 * particular input - MediaPipe's landmark net is trained on TIGHT crops
 * handed to it by a palm detector, with the hand filling the frame. Given a
 * 1.5-face-width crop instead, presence can still respond while localisation
 * collapses toward the centre.
 *
 * If that is what is happening, the geometric match in Stage 2 is VACUOUS:
 * the fingertip sits at the mouth by construction, norm_dist < 0.15 passes
 * automatically whenever presence fires, and the audit line claims a
 * fingertip reached the mouth when all that was established is that a hand
 * was somewhere in the crop.
 *
 * These accumulate the tip position over a watch, in the model's own 224
 * space where the centre is 112,112. Reading it:
 *
 *   spread 0-3 px about (112,112)   degenerate. Localisation is unusable and
 *                                   the match must not be claimed. Fix by
 *                                   cropping tighter, not by tuning.
 *   tips ranging over tens of px    real. The match means what it says.
 */
static uint32_t s_tip_n;
static int32_t  s_tip_sx, s_tip_sy;
static int16_t  s_tip_minx, s_tip_maxx, s_tip_miny, s_tip_maxy;

bool intake_handnet_ready(void) { return s_ready; }

void intake_handnet_reset(void)
{
    s_last_ms   = 0u;
    s_best_conf = 0.0f;
    memset(s_band, 0, sizeof(s_band));
    s_tip_n = 0u;
    s_tip_sx = s_tip_sy = 0;
    s_tip_minx = s_tip_miny = 32767;
    s_tip_maxx = s_tip_maxy = -32768;
}

void intake_handnet_bands(uint32_t out[6])
{
    if (out != NULL) {
        memcpy(out, s_band, sizeof(s_band));
    }
}

void intake_handnet_tips(uint32_t *n, int16_t *mean_x, int16_t *mean_y,
                          int16_t *span_x, int16_t *span_y)
{
    if (n) *n = s_tip_n;
    if (mean_x) *mean_x = (s_tip_n > 0u) ? (int16_t)(s_tip_sx / (int32_t)s_tip_n) : -1;
    if (mean_y) *mean_y = (s_tip_n > 0u) ? (int16_t)(s_tip_sy / (int32_t)s_tip_n) : -1;
    if (span_x) *span_x = (s_tip_n > 0u) ? (int16_t)(s_tip_maxx - s_tip_minx) : -1;
    if (span_y) *span_y = (s_tip_n > 0u) ? (int16_t)(s_tip_maxy - s_tip_miny) : -1;
}

void intake_handnet_stats(uint32_t *last_ms, float *best_conf)
{
    if (last_ms)   *last_ms   = s_last_ms;
    if (best_conf) *best_conf = s_best_conf;
}

bool intake_handnet_init(void)
{
    if (s_ready) {
        return true;
    }

    stai_return_code err = stai_hand_init(s_net);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_hand_init failed: %d\r\n", (int)err);
        return false;
    }

    /* Both sides are PREALLOCATED (STAI_HAND_IN_1_FLAGS and every
     * STAI_HAND_OUT_n_FLAGS carry STAI_FLAG_PREALLOCATED), so they are
     * FETCHED rather than bound - the same shape as FaceID, not CenterFace. */
    stai_size n = STAI_HAND_IN_NUM;
    err = stai_hand_get_inputs(s_net, s_in, &n);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_hand_get_inputs failed: %d\r\n", (int)err);
        return false;
    }

    n = STAI_HAND_OUT_NUM;
    err = stai_hand_get_outputs(s_net, s_out, &n);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_hand_get_outputs failed: %d\r\n", (int)err);
        return false;
    }

    /* No LOAD_QPARAMS here, and its absence is deliberate rather than an
     * oversight: every output of this network is STAI_FORMAT_FLOAT32, so there
     * is nothing to dequantise. The pill detector needed six scale/zero-point
     * pairs pulled out of braced initialisers by macro; this one needs none. */

    s_ready = true;
    printf("intake: hand landmark model ready (MediaPipe 224x224 INT8, "
           "21 keypoints, ~978 KB of activations in PSRAM).\r\n");
    return true;
}

uint8_t *intake_handnet_input(void)
{
    return s_ready ? (uint8_t *)s_in[0] : NULL;
}

bool intake_handnet_run(hand_lm_t *out)
{
    if (!s_ready || (out == NULL)) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* The caller filled the input through the CPU; the NPU reads it as a bus
     * master. Same discipline as every other buffer in this project that two
     * masters touch. */
    SCB_CleanDCache_by_Addr((uint32_t *)s_in[0], STAI_HAND_IN_1_SIZE_BYTES);

    const uint32_t t0 = HAL_GetTick();
    if (stai_hand_run(s_net, STAI_MODE_SYNC) != STAI_SUCCESS) {
        return false;
    }
    s_last_ms = HAL_GetTick() - t0;

    for (int i = 0; i < STAI_HAND_OUT_NUM; i++) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)s_out[i], s_out_bytes[i]);
    }

    const float conf = ((const float *)s_out[OUT_PRESENCE])[0];
    out->confidence = conf;
    if (conf > s_best_conf) {
        s_best_conf = conf;
    }
    {
        int b = (int)(conf * 10.0f) / 1;
        b = (conf >= 0.5f) ? 5 : ((b > 4) ? 4 : ((b < 0) ? 0 : b));
        if (b > 5) b = 5;
        s_band[b]++;
    }
    if (conf < PRESENCE_T) {
        return true;              /* ran fine; there is simply no hand */
    }
    out->present = true;

    /* 21 keypoints, (x, y, z) each, already in pixels of the 224 input. */
    const float *lm = (const float *)s_out[OUT_LANDMARKS];

    int sx = 0, sy = 0;
    int minx = HANDNET_SIZE, miny = HANDNET_SIZE, maxx = 0, maxy = 0;
    for (int k = 0; k < HANDNET_KEYPOINTS; k++) {
        int x = (int)lm[(k * 3) + 0];
        int y = (int)lm[(k * 3) + 1];
        if (x < 0)                { x = 0; }
        if (x >= HANDNET_SIZE)    { x = HANDNET_SIZE - 1; }
        if (y < 0)                { y = 0; }
        if (y >= HANDNET_SIZE)    { y = HANDNET_SIZE - 1; }
        sx += x; sy += y;
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x > maxx) maxx = x;
        if (y > maxy) maxy = y;
    }
    out->cx = (int16_t)(sx / HANDNET_KEYPOINTS);
    out->cy = (int16_t)(sy / HANDNET_KEYPOINTS);
    out->x  = (int16_t)minx;
    out->y  = (int16_t)miny;
    out->w  = (int16_t)((maxx - minx) + 1);
    out->h  = (int16_t)((maxy - miny) + 1);

    /* THE POINT THAT ENTERS THE MOUTH. Thumb tip and index tip, averaged -
     * a pill is held between those two, so their midpoint is where it is.
     * Not the centroid: see the note in the header on why using the middle of
     * the palm is what sent MOUTH_ZONE_NORM_DIST chasing an ear. */
    {
        const float tx = (lm[(HANDNET_KP_THUMB_TIP * 3) + 0] +
                          lm[(HANDNET_KP_INDEX_TIP * 3) + 0]) * 0.5f;
        const float ty = (lm[(HANDNET_KP_THUMB_TIP * 3) + 1] +
                          lm[(HANDNET_KP_INDEX_TIP * 3) + 1]) * 0.5f;
        int ix = (int)tx, iy = (int)ty;
        if (ix < 0)             { ix = 0; }
        if (ix >= HANDNET_SIZE) { ix = HANDNET_SIZE - 1; }
        if (iy < 0)             { iy = 0; }
        if (iy >= HANDNET_SIZE) { iy = HANDNET_SIZE - 1; }
        out->tip_x = (int16_t)ix;
        out->tip_y = (int16_t)iy;

        s_tip_n++;
        s_tip_sx += ix;
        s_tip_sy += iy;
        if (ix < s_tip_minx) s_tip_minx = (int16_t)ix;
        if (ix > s_tip_maxx) s_tip_maxx = (int16_t)ix;
        if (iy < s_tip_miny) s_tip_miny = (int16_t)iy;
        if (iy > s_tip_maxy) s_tip_maxy = (int16_t)iy;
    }

    return true;
}

#else  /* !MEDSIGHT_ACTION_RECOGNITION */

bool intake_handnet_init(void)  { return false; }
bool intake_handnet_ready(void) { return false; }
uint8_t *intake_handnet_input(void) { return NULL; }
bool intake_handnet_run(hand_lm_t *out)
{ if (out) memset(out, 0, sizeof(*out)); return false; }
void intake_handnet_stats(uint32_t *a, float *b) { if (a) *a = 0u; if (b) *b = 0.0f; }
void intake_handnet_bands(uint32_t out[6]) { if (out) { for (int i=0;i<6;i++) out[i]=0u; } }
void intake_handnet_tips(uint32_t *n, int16_t *a, int16_t *b, int16_t *c, int16_t *d)
{ if (n) *n = 0u; if (a) *a = -1; if (b) *b = -1; if (c) *c = -1; if (d) *d = -1; }
void intake_handnet_reset(void) { }

#endif /* MEDSIGHT_ACTION_RECOGNITION */
