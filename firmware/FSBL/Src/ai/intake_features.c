/* intake_features.c - Stage 2: geometry.
 *
 * Turns "where are the pill and the mouth" into "what is the pill doing
 * relative to the mouth". No NPU, no model, no floating-point library beyond
 * sqrtf - this is the stage the collaborator's own the collaborator's summary calls
 * "🟢 Very safe" for STM32, and it is.
 *
 * Every feature name matches the collaborator's reference implementation and
 * the collaborator's summary, deliberately, so that when somebody tunes a threshold they
 * can hold the Python and the C side by side. Where a feature could not be
 * carried across, it is absent rather than approximated under the same name -
 * see intake.h's header for the three that did not survive.
 *
 * THE ONE IDEA THAT MATTERS HERE is normalisation by face width. Their
 * `norm_dist = raw_dist / face_width_px` makes every threshold in Stage 3
 * independent of how far the patient is standing from the camera: a face
 * twice as far away is half as wide in pixels, and so is the distance the
 * pill travels to reach it. Without that, every constant in intake_fsm.c
 * would be a function of standing distance, and the machine would work for
 * exactly one person in exactly one position.
 */

#include "ai/intake.h"
#include <math.h>
#include <string.h>

/* How much of the mouth counts as "in the mouth zone".
 *
 * The collaborator tested `cv2.pointPolygonTest(inner_lip_polygon) >= -5`,
 * i.e. inside the inner lip contour or within 5 px of it. We have two mouth
 * corners and therefore no contour, so this is a RADIAL proxy: a circle about
 * the mouth centre whose radius is half the corner-to-corner width. A mouth is
 * roughly an ellipse about that centre with the corners at its extremes, so
 * half the width is the right order for "at the lips".
 *
 * Read intake.h before changing this: it is a proxy for their POLYGON, not
 * for their mouth-open test. Widening it does not recover mouth-open
 * detection; it only makes the zone sloppier. */
#define MOUTH_ZONE_WIDTH_FRAC   0.50f

/* Their second, independent route into AT_MOUTH: `norm_dist < 0.15`. It exists
 * because the zone test above needs a mouth width, and a mouth seen at an
 * angle can be narrow enough for that to be unreliable.
 *
 * ── WIDENED TO 0.30, BECAUSE THE TRACKED OBJECT CHANGED ──────────────────
 *
 * 0.15 is their number and it was right for their signal: they tracked a
 * PILL, and a pill's centroid IS the thing entering the mouth. 0.15 face
 * widths is about 22 mm on a 150 mm face - roughly a mouth's half-width, so
 * "the pill is at the lips" and "the pill centroid is within 0.15" are the
 * same statement.
 *
 * Stage 1C tracks a HAND, and a hand's centroid is not at the lips when the
 * pill is. It is the middle of a ~90 mm blob whose FINGERTIPS reach the
 * mouth, so at the moment of delivery the centroid sits something like 40-60
 * mm away - two to three times the old threshold. Holding 0.15 does not make
 * the machine stricter about a real gesture; it makes a correctly completed
 * gesture unreachable, which is what three hardware doses showed: the hand
 * was seen in 232 and 69 frames and the verdict still came out `uncertain`
 * because the zone was never entered.
 *
 * ── 0.30 WAS TOO LOOSE: IT LOCKED ONTO AN EAR ───────────────────────────
 *
 * Set to 0.30 (~45 mm, a hand's half-width) on that reasoning, and on
 * hardware it started confirming on a hand raised to the ear. Working out
 * why is what produced the containment test below, so it is worth being
 * precise about the failure:
 *
 *     mouth centre -> ear          ~0.45-0.55 face widths
 *     hand centroid at delivery    ~0.27-0.40 face widths
 *
 * Those two ranges are close enough that head pose closes the gap. A RADIAL
 * TEST ON A HAND CENTROID CANNOT SEPARATE THESE TWO CASES - not at 0.30, and
 * not at any other single radius. 0.15 excludes the ear but also excludes a
 * correct delivery; 0.30 admits a correct delivery and the ear with it.
 * There is no value that does both, and picking a third number would only
 * move which of the two mistakes we make.
 *
 * So this goes back to a tight 0.18 - near the collaborator's original, and
 * now only a FALLBACK for the case the containment test below cannot cover
 * (a hand box that fails to overlap the mouth because the mouth landmarks
 * are momentarily off). The work of recognising a delivery is done by
 * containment, which is geometric rather than metric and does not care how
 * big the hand is or how far its middle happens to sit from the lips. */
/* BACK TO THE COLLABORATOR'S 0.15, because the tracked point changed again
 * and this time in their favour.
 *
 * Every move of this constant - 0.15 -> 0.30 -> 0.18 - was an attempt to make
 * a HAND CENTROID work as a stand-in for the thing that enters the mouth. It
 * never could: a palm is 40-60 mm from the lips at delivery and an ear is
 * 0.45-0.55 face widths away, and those ranges overlap.
 *
 * Stage 1C now reports the midpoint of the THUMB TIP and INDEX TIP
 * (intake_handnet.h). A fingertip is at the lips exactly when the pill is -
 * which is the property the pill's own centroid had, and the property that
 * made 0.15 right in the first place. So it goes back, unchanged, for the
 * original reason. */
#define MOUTH_ZONE_NORM_DIST    0.15f

/* How far outside the tracked object's box the mouth may still count as
 * contained, as a divisor of the box's own width and height. 8 means one
 * eighth - a hand whose fingertips stop just short of the lips still reads as
 * a delivery, while the ear case is nowhere near.
 *
 * Expressed as a fraction of the OBJECT rather than of the face so that it
 * scales with the hand and with standing distance on its own. */
#define MOUTH_CONTAIN_MARGIN_DIV  8

/* How far the tracked object's CENTRE may sit from the mouth and still be
 * eligible for the containment test above. Sits between the two ranges in
 * that comment: above a delivering hand's 0.27-0.40, below an ear's
 * 0.45-0.55. */
#define MOUTH_CONTAIN_MAX_NORM  0.42f

static float s_prev_norm_dist;
static float s_prev_velocity;
static bool  s_have_prev;

void intake_features_reset(void)
{
    s_prev_norm_dist = 0.0f;
    s_prev_velocity  = 0.0f;
    s_have_prev      = false;
}

void intake_features_update(const pill_obs_t *pill, const mouth_obs_t *mouth,
                            intake_features_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->pill_visible    = (pill  != NULL) && pill->detected;
    out->mouth_visible   = (mouth != NULL) && mouth->detected;
    out->pill_confidence = out->pill_visible ? pill->confidence : 0.0f;

    /* Without BOTH observations there is no geometry to compute. Leave the
     * previous distance untouched rather than zeroing it: a pill occluded for
     * a few frames must not read as "it teleported to the mouth" when it
     * reappears, which is exactly what a zeroed distance would produce on the
     * next velocity calculation. */
    if (!out->pill_visible || !out->mouth_visible) {
        out->norm_dist      = s_have_prev ? s_prev_norm_dist : 0.0f;
        out->norm_dist_prev = out->norm_dist;
        return;
    }

    /* face_w is the normalisation base and it must never be zero - a divide
     * here would produce an infinity that propagates silently through every
     * threshold in Stage 3. */
    float face_w = (float)mouth->face_w;
    if (face_w < 1.0f) {
        face_w = 1.0f;
    }

    const float dx = (float)(pill->cx - mouth->cx);
    const float dy = (float)(pill->cy - mouth->cy);
    const float raw_dist = sqrtf((dx * dx) + (dy * dy));

    out->norm_dist      = raw_dist / face_w;
    out->norm_dist_prev = s_have_prev ? s_prev_norm_dist : out->norm_dist;

    /* Sign convention is theirs: POSITIVE means the pill is closing on the
     * mouth, because they test `(prev - now) > threshold` for approach. */
    out->distance_velocity = s_have_prev
                           ? (s_prev_norm_dist - out->norm_dist)
                           : 0.0f;
    out->distance_acceleration = s_have_prev
                               ? (out->distance_velocity - s_prev_velocity)
                               : 0.0f;

    /* ── THE MOUTH MUST BE INSIDE THE OBJECT, NOT MERELY NEAR ITS MIDDLE ──
     *
     * This is the test that actually distinguishes a delivery from a hand
     * doing something else near the head, and it works because it asks about
     * the object's EXTENT rather than its centre.
     *
     * When a person puts a pill in their mouth their fingers are at their
     * lips, so the mouth point falls INSIDE the hand's bounding box - no
     * matter that the hand's centroid, being the middle of the palm, sits
     * 40-60 mm lower. When the same hand is at an ear, the box is beside the
     * head and the mouth is outside it, even though the two centroids are a
     * similar distance away. A radial test cannot see that difference; this
     * one is exactly that difference.
     *
     * The margin is a fraction of the object's own size, so it scales with
     * the hand and with standing distance without another constant to tune.
     * A box with no extent (the pill detector's tiny boxes, or an unset
     * observation) falls through to the radial tests below, which is the
     * right behaviour: containment is meaningless for an 8 px object. */
    /* ── CONTAINMENT ALONE IS NOT ENOUGH, BECAUSE BLOBS MERGE ────────────
     *
     * A hand and a face are both skin. Moving a hand near the head occludes
     * and re-exposes cheek, so the face lights up as foreground AROUND the
     * hand and the two join into a SINGLE connected component. Its bounding
     * box can then stretch from the ear across the cheek and swallow the
     * mouth, and a pure containment test says yes.
     *
     * Found by deliberately waving a hand at an ear to stress the stage - a
     * test worth repeating on anything else here, because it is exactly the
     * motion an itch produces and a patient will make it eventually.
     *
     * So containment must ALSO have the object's centre somewhere plausible.
     * The two measurements fail in different directions, which is what makes
     * the conjunction worth more than either:
     *
     *     hand centroid at delivery    ~0.27-0.40 face widths
     *     mouth centre -> ear          ~0.45-0.55 face widths
     *
     * A merged ear-and-cheek blob CONTAINS the mouth but is centred far from
     * it; a delivering hand is centred close but its centroid alone is too
     * far to pass a tight radial test. Requiring both admits the first case
     * and rejects the second. */
    bool contains_mouth = false;
    if ((pill->w > 0) && (pill->h > 0) &&
        (out->norm_dist < MOUTH_CONTAIN_MAX_NORM)) {
        const int mx = (int)(pill->w / MOUTH_CONTAIN_MARGIN_DIV);
        const int my = (int)(pill->h / MOUTH_CONTAIN_MARGIN_DIV);
        contains_mouth = ((int)mouth->cx >= ((int)pill->x - mx)) &&
                         ((int)mouth->cx <= ((int)pill->x + (int)pill->w + mx)) &&
                         ((int)mouth->cy >= ((int)pill->y - my)) &&
                         ((int)mouth->cy <= ((int)pill->y + (int)pill->h + my));
    }

    const float zone_r = MOUTH_ZONE_WIDTH_FRAC * (float)mouth->width;
    out->in_mouth_zone = contains_mouth ||
                         (raw_dist <= zone_r) ||
                         (out->norm_dist < MOUTH_ZONE_NORM_DIST);

    s_prev_norm_dist = out->norm_dist;
    s_prev_velocity  = out->distance_velocity;
    s_have_prev      = true;
}
