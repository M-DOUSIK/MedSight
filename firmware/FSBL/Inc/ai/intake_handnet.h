/* intake_handnet.h - Stage 1C, second implementation: a real hand model.
 *
 *
 * ── WHAT THIS REPLACES, AND WHY ──────────────────────────────────────────
 *
 * Src/ai/intake_hand.c finds a hand with a skin-tone mask over a
 * background-subtracted grid. It took four hardware rounds and never became
 * trustworthy, for a reason no amount of tuning could reach:
 *
 *     A MOVING EAR IS A MOVING SKIN REGION.
 *
 * At the level that stage works - skin-toned, solid, differs from a baseline -
 * an ear revealed by a turn of the head is indistinguishable from a hand. Size,
 * density and colour all say the same thing about both. Every fix was therefore
 * a geometric workaround (how far from the mouth, is the mouth inside the box,
 * has the blob moved), and each one traded one failure for another.
 *
 * This asks a model that actually knows what a hand is.
 *
 * ── THE MODEL ────────────────────────────────────────────────────────────
 *
 * MediaPipe hand landmarks, `handlandmarks_full_224_int8.tflite`, from the ST
 * model zoo (`pose_estimation/handlandmarks`), Apache-2.0 - which is also a
 * licensing improvement on Stage 1A, whose YOLOv8n retrain left an unresolved
 * question in THIRD_PARTY_SOFTWARE.md about AGPL derivative works.
 *
 * 224x224x3 uint8 HWC in. Four float32 outputs, all preallocated, none
 * quantised - so unlike the pill detector there are no scale/zero-point tables
 * to load and nothing to dequantise. Measured on the generated graph, feeding
 * it this project's own compiled-in face image:
 *
 *   OUT_3  Dequantize_221   1 float   HAND PRESENCE      face: 0.0078
 *   OUT_4  Dequantize_230  63 floats  21 x (x, y, z), pixels in 224 space
 *   OUT_1  Dequantize_227   1 float   handedness (meaningless with no hand)
 *   OUT_2  Dequantize_233  63 floats  world/normalised landmarks, unused
 *
 * Presence reads 0.0078 on a face, 0.0078 on random noise and 0.0117 on flat
 * grey. The confusor that defeated the skin heuristic scores a flat zero here.
 *
 * ── THE FINGERTIPS ARE THE POINT ─────────────────────────────────────────
 *
 * This is the part that matters most, and it retroactively settles an argument
 * this project had twice.
 *
 * MOUTH_ZONE_NORM_DIST was 0.15 face widths in the collaborator's design. It
 * was widened to 0.30 because a hand's CENTROID sits 40-60 mm below the lips at
 * the moment of delivery - and 0.30 then locked onto an ear, because
 * mouth-to-ear is 0.45-0.55 and the two ranges overlap once head pose is
 * allowed for. No single radius separates them.
 *
 * With real landmarks the problem does not arise. Keypoints 4 and 8 are the
 * THUMB TIP and INDEX TIP - the part that actually enters the mouth. A
 * fingertip is at the lips exactly when the pill is, so 0.15 is correct again,
 * for the same reason it was correct for a pill.
 *
 * ── WHAT IT COSTS ────────────────────────────────────────────────────────
 *
 * Activations are 1,197,952 bytes: about 220 KB in AI_ARENA and about 978 KB
 * in PSRAM across 13 large buffers, because the arena alone cannot hold it
 * (the compiler refuses outright - `total bytes left unallocated=3515456`).
 * The `.mpool` therefore opens xSPI1 at 0x90500000, clear of MobileFaceNet
 * (0x90000000-0x90310000) and of the intake camera frame (0x90400000).
 *
 * That has two consequences worth stating plainly:
 *
 *   THE PILL DETECTOR IS GONE. It needed the whole arena and the whole of
 *   0x73000000. This is a replacement, not an addition.
 *
 *   INFERENCE WILL BE SLOWER THAN ST'S PUBLISHED 20.75 ms, which is an
 *   all-internal figure. PSRAM is THROUGHPUT=MID LATENCY=HIGH. The real number
 *   is printed once per watch rather than guessed at here.
 */
#ifndef AI_INTAKE_HANDNET_H
#define AI_INTAKE_HANDNET_H

#include "ai/intake.h"
#include <stdbool.h>
#include <stdint.h>

/** The model's input side, so callers size their crop correctly. */
#define HANDNET_SIZE       224
#define HANDNET_KEYPOINTS  21

/** MediaPipe keypoint indices. 4 and 8 are the two that reach a mouth. */
#define HANDNET_KP_THUMB_TIP  4
#define HANDNET_KP_INDEX_TIP  8

typedef struct {
    bool    present;        /**< presence cleared the threshold             */
    float   confidence;     /**< the raw presence score, 0..1               */
    /* All coordinates are in the model's own 224x224 input space; the caller
     * maps them into frame space with the ROI scale factor, exactly as it did
     * for the pill detector's box. */
    int16_t tip_x, tip_y;   /**< midpoint of thumb tip and index tip        */
    int16_t cx, cy;         /**< centroid of all 21 keypoints               */
    int16_t x, y, w, h;     /**< bounding box over all 21 keypoints         */
} hand_lm_t;

/** Bring the network up. Safe to call more than once; returns false and says
 *  why on the console if the NPU refuses, in which case the whole subsystem
 *  degrades to "no corroboration" exactly as a missing pill detector did. */
bool intake_handnet_init(void);

/** Whether init() succeeded. */
bool intake_handnet_ready(void);

/** The network's PREALLOCATED input buffer: HANDNET_SIZE x HANDNET_SIZE x 3
 *  bytes, **HWC** (interleaved RGB), 0..255 unsigned.
 *
 *  Note the two differences from the pill detector, both of which are stated
 *  by the generated header rather than chosen here: that one wanted CHW planar
 *  and SIGNED bytes, this one wants HWC and UNSIGNED. Writing the wrong layout
 *  produces a network that runs, returns plausible numbers and means nothing -
 *  the failure mode that cost two rounds on the pill detector. */
uint8_t *intake_handnet_input(void);

/** Run one inference over whatever is in the input buffer.
 *
 *  Returns false if the network is not ready or the run failed. A run that
 *  succeeds but finds no hand returns true with `out->present` clear - those
 *  are different outcomes and the caller should not conflate them. */
bool intake_handnet_run(hand_lm_t *out);

/** Wall-clock milliseconds the last successful run took, and the highest
 *  presence score seen since the last reset.
 *
 *  The timing is reported because it cannot be predicted from ST's published
 *  figure: theirs is measured with all activations in internal RAM, and this
 *  build runs about 978 KB of them out of PSRAM. */
void intake_handnet_stats(uint32_t *last_ms, float *best_conf);

/** How many frames of the last watch landed in each presence band:
 *  [0]<0.1 [1]<0.2 [2]<0.3 [3]<0.4 [4]<0.5 [5]>=0.5.
 *
 *  Exists because a peak alone cannot distinguish a saturated output from a
 *  bimodal one, and five watches running have reported the same peak. See the
 *  note at the definition. */
void intake_handnet_bands(uint32_t out[6]);

/** Where the fingertip landed, over the whole watch, in the model's own 224
 *  space - centre is (112,112).
 *
 *  This exists to answer one question: IS THE LOCALISATION REAL? The ROI is
 *  centred on the mouth, so a model returning coordinates near the middle of
 *  its input maps onto the mouth every time regardless of the hand, and the
 *  geometric match in Stage 2 becomes vacuous. A span of a few pixels about
 *  (112,112) means exactly that. See the note at the definition. */
void intake_handnet_tips(uint32_t *n, int16_t *mean_x, int16_t *mean_y,
                          int16_t *span_x, int16_t *span_y);

/** Forget the per-watch statistics. */
void intake_handnet_reset(void);

#endif /* AI_INTAKE_HANDNET_H */
