/* intake_hand.h - Stage 1C: find the HAND, not the pill.
 *
 * ── WHY THIS EXISTS, WHICH IS AN ADMISSION ───────────────────────────────
 *
 * Stage 1A detects a pill on the NPU, and on real hardware it does not work
 * well enough to decide anything. Five consecutive doses were logged with the
 * detector running: 2/54, 53/420, 6/243, 0/121 and 5/210 frames containing a
 * pill, with the box frequently on wall texture rather than on the object.
 * Retraining moved those numbers and did not change the conclusion.
 *
 * The reason is geometry, and it was knowable in advance:
 *
 *     ROI = 1.5 x face width  ~= 225 mm across a 160 px input = 0.71 px/mm
 *
 *     a 12 mm tablet   ->   8.5 px
 *     a hand           ->  ~64 px
 *
 * We were asking a detector to find an EIGHT PIXEL object in a frame whose
 * mean luminance measured 63..78, while the hand carrying it was seven times
 * larger in each dimension and about fifty times larger in area. At 8 px a
 * tablet and a patch of wall grain are not reliably different things. That is
 * not a training problem and no amount of retraining fixes it.
 *
 * ── WHAT THE FIELD ACTUALLY DOES ─────────────────────────────────────────
 *
 * Medication-adherence systems do not detect the pill. They decompose the act
 * into mini-activities - hand-to-mouth, pill-into-mouth, hand-off-mouth - and
 * detect the HAND and the MOUTH (AiCure, US10402982 and family). The
 * smartwatch literature detects the same gesture from wrist accelerometry and
 * never sees a pill at all (JMIR Hum Factors 2023;10:e42714). Nobody resolves
 * a 10 mm tablet at conversational distance, because it cannot be done.
 *
 * ── HOW THIS WORKS, AND WHY IT NEEDS NO MODEL ────────────────────────────
 *
 * A hand approaching a mouth is a large, moving, skin-toned region converging
 * on a point this device already knows for free - the mouth, from the
 * CenterFace landmarks Stage 1B decodes at no cost.
 *
 * So: subtract a BASELINE of the scene, mask to skin tone, take the centroid
 * of what is left. The baseline is captured once the ISP has settled, a few
 * frames into the watch.
 *
 *   THE FACE, THE WALL AND THE ROOM ARE ALL IN THE BASELINE, so they cancel
 *   exactly. What survives is what ARRIVED after the watch began - which is
 *   the question being asked. The false-positive class that dogged Stage 1A
 *   cannot occur here: a static surface produces no evidence at all.
 *
 *   A BASELINE, NOT THE PREVIOUS FRAME. Differencing consecutive frames
 *   lights up only the EDGES of a smooth moving object - its interior is
 *   unchanged between them - so a hand appears as a thin arc that no shape
 *   test can tell from scattered noise. That version found a hand in 0 of
 *   316 frames on hardware. Against a still reference the whole area of the
 *   hand differs, and the blob is solid enough to test.
 *
 * It runs on the M55 over a 40x40 decimation of a crop the service already
 * has in hand. Stage 1A is now sampled on one frame in four rather than
 * every frame, which is where the idle percentage went (42% during a watch
 * against 88% either side, measured on a run where the overlay drew ONE
 * time - so the overlay was never the cause and the inference always was).
 *
 * ── THE PILL DETECTOR IS NOT REMOVED ─────────────────────────────────────
 *
 * Stage 1A stays and still runs. It is demoted from "the decision" to "a
 * corroborating signal": when it fires it raises confidence, its box still
 * draws on the overlay, and the claim that a YOLOv8n runs on the Neural-ART
 * NPU remains true and demonstrable. What changes is that a dose is no longer
 * decided by an 8-pixel object.
 */
#ifndef AI_INTAKE_HAND_H
#define AI_INTAKE_HAND_H

#include "ai/intake.h"
#include <stdbool.h>
#include <stdint.h>

/** Forget the previous frame. Call once at the start of every watch, or the
 *  first difference is taken against another dose's ROI and the first frame
 *  reports a hand that is not there. */
void intake_hand_reset(void);

/** One frame. `chw` is the SAME planar RGB crop Stage 1A was given - CHW,
 *  `size` x `size`, 0..255 - so this costs one extra pass over memory that is
 *  already hot and no extra camera work.
 *
 *  On success `out` carries the moving-skin centroid and a bounding box in
 *  ROI pixel space (0..size), for the caller to map into frame space exactly
 *  as it maps the pill's. Returns false, with out->detected clear, when there
 *  is no hand this frame.
 *
 *  Never blocks and never allocates. */
bool intake_hand_update(const uint8_t *chw, int size, int mouth_half_cells,
                         pill_obs_t *out);

/** How much of the MOUTH PATCH was covered on the last frame, 0..100, and
 *  (optionally) the highest value seen since the last reset.
 *
 *  This is what the verdict rests on. Everything else in this header
 *  searches the ROI for an object; this asks one question about one known
 *  place, and that is why it is not fooled by an ear. See the long note in
 *  intake_hand.c.
 *
 *  `mouth_half_cells` passed to intake_hand_update() sizes the patch. Pass 0
 *  when the mouth position is not known and this returns 0. */
uint8_t intake_hand_mouth_cover(uint8_t *best);

/** Totals for the once-per-dose diagnostic line: how many frames carried a
 *  hand, and the mean skin / motion coverage across the watch in percent.
 *
 *  These exist because the skin and motion thresholds below are the kind of
 *  constant that cannot be set from a desk - the ISP, the lighting and the
 *  patient's skin all move them. One printed line per dose is what makes the
 *  next adjustment a measurement instead of another guess. */
void intake_hand_stats(uint32_t *frames_with_hand,
                       uint8_t *mean_skin_pct, uint8_t *mean_motion_pct);

/** WHICH GATE rejected, and the best blob the watch ever saw.
 *
 * The previous round's diagnostic reported healthy skin and motion masks and
 * could not say why their combination never fired once in 316 frames. A
 * summary that cannot distinguish "the blob was too small" from "the blob
 * was too sparse" costs a hardware round every time it is ambiguous, so
 * these name the gate directly:
 *
 *   best_cells small          the hand is not reaching the ROI at all
 *   best_cells big, density   low - the blob is scattered, so DIFF_T is
 *                             picking up noise rather than an object
 *   rej_small dominant        MIN_CELLS or DIFF_T too high
 *   rej_sparse dominant       MIN_DENSITY_PCT too high for this scene
 *   rej_flood dominant        the baseline is stale; the ISP is drifting
 *                             faster than REF_ADAPT_SHIFT tracks it
 *   rej_stale dominant        cells are being retired into the baseline -
 *                             something in the scene is stuck as foreground
 *   rej_far dominant          blobs appearing away from the mouth: a moving
 *                             ear or a turned head, not a delivery
 */
void intake_hand_debug(uint16_t *best_cells, uint16_t *best_density_pct,
                       uint32_t *rej_small, uint32_t *rej_sparse,
                       uint32_t *rej_flood, uint32_t *rej_stale,
                       uint32_t *rej_far);

#endif /* AI_INTAKE_HAND_H */
