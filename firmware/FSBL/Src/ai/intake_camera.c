/* intake_camera.c - the camera path. See Inc/ai/intake_camera.h for the
 * memory map, the address derivation and the LPEN story; this file is the
 * mechanism only. */

#include "ai/intake_camera.h"
#include "main.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>
#include <string.h>

extern DCMIPP_HandleTypeDef hdcmipp;

/* g_isp_suspend lives in state_machine.c and gates the ISP background task.
 * Action recognition does NOT suspend it: unlike a face capture, the whole
 * point here is that the camera keeps running and keeps its auto-exposure
 * converging while the patient moves a pill toward their mouth. */

static bool    s_running = false;
static uint8_t s_last_roi_mean = 0;

uint8_t intake_camera_last_roi_mean(void)
{
    return s_last_roi_mean;
}

bool intake_camera_start(void)
{
    if (s_running) {
        return true;
    }

    /* Stop whatever the pipe was doing before repointing it. Restarting a
     * running pipe at a new destination is not something the HAL promises to
     * do atomically, and a half-repointed DMA would write frames across two
     * addresses - one of which is the screen the user is looking at. */
    (void)HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0);

    if (HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0,
                                   INTAKE_FRAME_ADDR,
                                   DCMIPP_MODE_CONTINUOUS) != HAL_OK) {
        /* Not fatal, and must never be treated as one. The dose is confirmed
         * by the button; this subsystem only corroborates. */
        printf("camera->PSRAM start FAILED; the live view is unavailable; "
               "a capture may still work.\r\n");
        return false;
    }

    s_running = true;
    printf("camera streaming to PSRAM 0x%08lX (UI keeps the "
           "framebuffer).\r\n", (unsigned long)INTAKE_FRAME_ADDR);
    return true;
}

void intake_camera_stop(void)
{
    if (!s_running) {
        return;
    }
    (void)HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0);
    s_running = false;

    /* Leave the pipe pointing back at the display framebuffer. The dispense flow's
     * STATE_CAMERA_DISPENSE calls camera_start() expecting BUFFER_ADDRESS, and
     * a pipe left aimed at PSRAM would give it a preview that never appears -
     * a regression that would look like a camera fault rather than like this
     * module. Restoring here keeps the "with the switch off it behaves exactly
     * as it did before" promise true even with the switch ON. */
    (void)HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0,
                                     BUFFER_ADDRESS, DCMIPP_MODE_CONTINUOUS);
    (void)HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0);
}

bool intake_camera_is_running(void)
{
    return s_running;
}

/* How much of the face width the ROI spans, as a percentage.
 *
 * ── THIS CONSTANT SETS THE APPARENT SIZE OF A PILL, AND THAT IS THE WHOLE
 *    BALLGAME FOR THE DETECTOR ────────────────────────────────────────────
 *
 * The counter-intuitive part first: **standing closer to the device does not
 * help.** The ROI is defined as a multiple of the FACE WIDTH, and the face and
 * the pill shrink together with distance, so the pill's size in the detector's
 * 160x160 input is independent of how far away the patient stands. It is fixed
 * by one ratio: pill_diameter / (mult x face_width).
 *
 * Working it through for a 150 mm face and a 160 px input:
 *
 *   object                     at mult 2.0    at mult 1.0
 *   GEMS / small tablet  12 mm      6.4 px        12.8 px
 *   typical tablet       10 mm      5.3 px        10.7 px
 *   bottle cap           28 mm     14.9 px        29.9 px
 *
 * The detector was trained on pills spanning **17-49 px** at this input
 * (median 28), so at mult 2.0 a real pill is far outside the distribution it
 * learned. That is not a bug in the model; it is the deployment scenario -
 * "pill near a face, whole face in frame" - being genuinely different from the
 * close-up photographs the public dataset is made of.
 *
 * Measured on hardware at mult 2.0: a ~28 mm bottle cap (14.9 px) was detected
 * 8-18 times per watch. So ~15 px is the practical floor for this model, and
 * 6.4 px is hopeless.
 *
 * ── WHY 100 AND NOT TIGHTER ──────────────────────────────────────────────
 *
 * Tightening the ROI makes the pill bigger but shrinks the field the geometry
 * can see, and Stage 3's thresholds are expressed in FACE WIDTHS:
 *
 *   AT_MOUTH triggers at   norm_dist < 0.15 face widths
 *   RETREAT  triggers at   norm_dist > 0.35 face widths
 *
 * So the ROI must span at least +/-0.35 face widths about the mouth, i.e. a
 * multiplier of 0.7, or the retreat transition can never fire because the pill
 * has left the crop before it gets far enough away. That is the floor.
 *
 * -- WHY 150, WITH BOTH ENDS MEASURED --------------------------------------
 *
 * 200 was the original guess. 100 was then set from an offline curve that
 * measured pill SIZE only, and it made the device dramatically WORSE on
 * hardware: 1-3 detections per watch against 8-18 at 200. Halving this
 * constant also halves the FIELD OF VIEW, and a hand carrying a pill to a
 * mouth spends most of its travel more than half a face width away.
 * Measuring one of the two variables a constant controls is not measuring
 * the constant.
 *
 * 150 is set from both, against the small-object detector retrained at this
 * input size (Addendum 11):
 *
 *   field of view   +/-0.75 face widths - double the 0.35 the RETREAT
 *                   transition needs, so the geometry keeps real margin
 *   12 mm object    8.5 px -> 72% offline, ~36% expected on hardware, which
 *                   over a 100-frame watch gives a ~96% chance of the
 *                   three-consecutive lock
 *   28 mm object    19.9 px -> ~98%
 *
 * A true 10-12 mm pill sits at the edge of what a 160 px input resolves at
 * conversational distance. That is geometry, not a model deficiency, and
 * further retraining does not move it much; a ~20 mm object is the reliable
 * case. */
/* ── 250 SINCE THE HAND MODEL: THE CROP MUST CONTAIN THE GESTURE ────────
 *
 * Everything above this line is the reasoning for a PILL detector, where the
 * only question was how many pixels an 8 mm object occupies. Stage 1C now
 * runs a hand model, and a different question dominates: is the hand INSIDE
 * THE CROP AT ALL?
 *
 * At 150 it usually was not. Measured on hardware, ROI 228 px centred on a
 * mouth at (439,289) covers x 325-553, y 175-403 of an 800x480 frame - a
 * window around the face and nothing below it. A hand travelling up to the
 * mouth is outside that box for most of its journey, so the model had nothing
 * to detect until the gesture was already finished. That is exactly what the
 * logs showed: 3 detections in 33 frames, and a box that only ever appeared
 * on the mouth.
 *
 * 250 gives ~380 px at the same framing: x 249-629, y 99-479 - down to the
 * bottom of the frame, so a hand is in view for the whole approach.
 *
 * WHAT IT COSTS. The hand gets smaller in the model's 224 px input:
 *
 *     at 150   228 px ROI  ->  a ~90 mm hand is ~88 px, 39% of the input
 *     at 250   380 px ROI  ->  the same hand is ~53 px, 24% of the input
 *
 * MediaPipe's landmark net expects a tight crop from a palm detector, so
 * smaller is worse for it. This is a real trade and it is made deliberately:
 * a hand the model sees at lower confidence is worth more than a hand it
 * never sees. PRESENCE_T moves down with it.
 *
 * NONE OF THE GEOMETRY CHANGES. Stage 2 measures in FACE WIDTHS, not in ROI
 * fractions, so MOUTH_ZONE_NORM_DIST and the containment gate mean exactly
 * what they meant before. That is the property that makes this safe to change
 * - and it is why widening it here is not a repeat of Addendum 8, where
 * shrinking the ROI silently changed what the thresholds were measuring. */
/* REVERTED TO 150. The widening to 250 above was aimed at a problem this is
 * not: it assumed the hand was missed because it was outside the crop, and
 * the truth is that MediaPipe's LANDMARK net is not a detector at all. It is
 * fed a tight, hand-filling crop by a palm detector in the real pipeline, and
 * we have no palm detector - so it reports presence reliably only when the
 * hand is centred and large, which (the ROI being centred on the mouth) means
 * "hand at the mouth". Widening the crop made the hand SMALLER and pushed it
 * further out of distribution, i.e. the opposite of the intent.
 *
 * 150 is the value that was measured to work for the case this device
 * actually needs. The reasoning above the reverted block is still the right
 * reasoning for the FIELD OF VIEW the geometry needs. */
#define ROI_FACE_W_PCT    150
#define ROI_FACE_W_MULT_NUM  ROI_FACE_W_PCT
#define ROI_FACE_W_MULT_DEN  100

/* The same ROI, written HWC and unsigned for the hand landmark model.
 *
 * A separate function rather than a flag on the one below, because the two
 * layouts are not a preference - each generated header STATES what its
 * network wants, and they disagree:
 *
 *     pill detector   CHW planar, SIGNED   (rgb - 128), 160x160
 *     hand landmarks  HWC interleaved, UNSIGNED 0..255, 224x224
 *
 * Writing the wrong one produces a network that runs, returns plausible
 * numbers, and means nothing. That failure cost this project two hardware
 * rounds on the pill detector, so the two paths are kept visibly apart
 * rather than merged behind a parameter someone can pass wrongly. */
bool intake_camera_grab_roi_hwc(const mouth_obs_t *mouth,
                                 uint8_t *out_hwc, int out_size,
                                 int16_t *out_roi_x, int16_t *out_roi_y,
                                 int16_t *out_roi_size)
{
    if ((out_hwc == NULL) || (out_size <= 0)) {
        return false;
    }

    int roi = INTAKE_FRAME_H;
    int cx  = INTAKE_FRAME_W / 2;
    int cy  = INTAKE_FRAME_H / 2;
    if ((mouth != NULL) && mouth->detected && (mouth->face_w > 0)) {
        roi = ((int)mouth->face_w * ROI_FACE_W_MULT_NUM) / ROI_FACE_W_MULT_DEN;
        cx  = mouth->cx;
        cy  = mouth->cy;
    }
    if (roi < 64)             roi = 64;
    if (roi > INTAKE_FRAME_H) roi = INTAKE_FRAME_H;
    int x0 = cx - (roi / 2);
    int y0 = cy - (roi / 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if ((x0 + roi) > INTAKE_FRAME_W) x0 = INTAKE_FRAME_W - roi;
    if ((y0 + roi) > INTAKE_FRAME_H) y0 = INTAKE_FRAME_H - roi;

    SCB_InvalidateDCache_by_Addr((uint32_t *)INTAKE_FRAME_ADDR, INTAKE_FRAME_BYTES);
    const uint16_t *frame = (const uint16_t *)INTAKE_FRAME_ADDR;

    uint32_t sum = 0;
    for (int y = 0; y < out_size; y++) {
        const int sy = y0 + ((y * roi) / out_size);
        const uint16_t *row = &frame[sy * INTAKE_FRAME_W];
        uint8_t *dst = &out_hwc[(y * out_size) * 3];
        for (int x = 0; x < out_size; x++) {
            const int sx = x0 + ((x * roi) / out_size);
            const uint16_t px = row[sx];
            const uint8_t g = (uint8_t)(((px >> 5) & 0x3F) << 2);
            dst[(x * 3) + 0] = (uint8_t)(((px >> 11) & 0x1F) << 3);
            dst[(x * 3) + 1] = g;
            dst[(x * 3) + 2] = (uint8_t)(( px        & 0x1F) << 3);
            sum += g;
        }
    }
    s_last_roi_mean = (uint8_t)(sum / (uint32_t)(out_size * out_size));

    if (out_roi_x)    *out_roi_x    = (int16_t)x0;
    if (out_roi_y)    *out_roi_y    = (int16_t)y0;
    if (out_roi_size) *out_roi_size = (int16_t)roi;
    return true;
}

bool intake_camera_grab_roi(const mouth_obs_t *mouth,
                             uint8_t *out_chw, int out_size,
                             int16_t *out_roi_x, int16_t *out_roi_y,
                             int16_t *out_roi_size)
{
    if ((out_chw == NULL) || (out_size <= 0)) {
        return false;
    }

    int roi = INTAKE_FRAME_H;          /* fallback: the largest centred square */
    int cx  = INTAKE_FRAME_W / 2;
    int cy  = INTAKE_FRAME_H / 2;

    if ((mouth != NULL) && mouth->detected && (mouth->face_w > 0)) {
        roi = ((int)mouth->face_w * ROI_FACE_W_MULT_NUM) / ROI_FACE_W_MULT_DEN;
        cx  = mouth->cx;
        cy  = mouth->cy;
    }

    /* Clamp the square fully inside the frame. */
    if (roi < 64)                roi = 64;
    if (roi > INTAKE_FRAME_H)    roi = INTAKE_FRAME_H;
    int x0 = cx - (roi / 2);
    int y0 = cy - (roi / 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if ((x0 + roi) > INTAKE_FRAME_W) x0 = INTAKE_FRAME_W - roi;
    if ((y0 + roi) > INTAKE_FRAME_H) y0 = INTAKE_FRAME_H - roi;

    /* The DCMIPP wrote this frame with no CPU involvement, so anything the
     * D-cache holds for it is stale by construction. Same discipline as every
     * other buffer in this project that a DMA master touches. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)INTAKE_FRAME_ADDR, INTAKE_FRAME_BYTES);

    const uint16_t *frame = (const uint16_t *)INTAKE_FRAME_ADDR;
    const int plane = out_size * out_size;
    uint8_t *r_plane = &out_chw[0 * plane];
    uint8_t *g_plane = &out_chw[1 * plane];
    uint8_t *b_plane = &out_chw[2 * plane];

    /* Nearest-neighbour, CHW planar, raw 0..255 bytes - the same layout and
     * the same reasoning as convert_crop_to_fd_input() in ai_vision.c, which
     * documents why CHW is right here despite the generated header's
     * CHANNEL_LAST flag. */
    for (int y = 0; y < out_size; y++) {
        const int sy = y0 + ((y * roi) / out_size);
        const uint16_t *row = &frame[sy * INTAKE_FRAME_W];
        const int ro = y * out_size;
        for (int x = 0; x < out_size; x++) {
            const int sx = x0 + ((x * roi) / out_size);
            const uint16_t px = row[sx];
            r_plane[ro + x] = (uint8_t)(((px >> 11) & 0x1F) << 3);
            g_plane[ro + x] = (uint8_t)(((px >> 5)  & 0x3F) << 2);
            b_plane[ro + x] = (uint8_t)(( px        & 0x1F) << 3);
        }
    }

    /* Mean of the green plane - cheap, and enough to tell a live frame from a
     * dead one. See intake_camera_last_roi_mean(). */
    {
        uint32_t sum = 0;
        for (int i = 0; i < plane; i++) {
            sum += g_plane[i];
        }
        s_last_roi_mean = (uint8_t)(sum / (uint32_t)plane);
    }

    if (out_roi_x)    *out_roi_x    = (int16_t)x0;
    if (out_roi_y)    *out_roi_y    = (int16_t)y0;
    if (out_roi_size) *out_roi_size = (int16_t)roi;
    return true;
}
