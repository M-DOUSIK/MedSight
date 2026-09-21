/* intake_service.c - orchestration for action recognition.
 *
 * Runs Stages 1A, 1B, 2 and 3 for one dose, on the EXISTING ai task at
 * priority 3. See Inc/ai/intake.h for why there is no second AI task.
 *
 * ── THE CONTRACT WITH THE UI, WHICH IS THE WHOLE SAFETY ARGUMENT ─────────
 *
 * The UI never waits for this. It calls intake_begin() when the confirm
 * screen goes up, keeps polling touch at its usual 10 ms, and reads
 * intake_peek() only at the instant the patient taps "✓ I Took It" - to
 * decide which SUFFIX the audit line gets, never whether to write one.
 *
 * Every failure path in this file therefore ends in "the verdict stays
 * uncertain": no camera, no detector, no face, a stalled pipe, a timeout.
 * None of them can stop a dose being confirmed. That is deliberate and it is
 * the reason this feature was allowed to exist at all.
 *
 */

#include "ai/intake.h"
#include "ai/intake_handnet.h"
#include "ai/intake_camera.h"
#include "ai_vision.h"
#include "ui/ai_overlay.h"
#include "ms_osal.h"
#include "stm32n6xx_hal.h"   /* HAL_GetTick() for the watch timeout */
#include <stdio.h>
#include <string.h>

#if MEDSIGHT_ACTION_RECOGNITION

/* How long to watch before giving up. Matched to the confirm screen's own
 * CONFIRM_TAKEN_TIMEOUT_MS (30 s) so the watcher can never outlive the screen
 * it belongs to - a camera left streaming into PSRAM after the UI has moved
 * on would be a power cost with no consumer. */
#define INTAKE_WATCH_TIMEOUT_MS   30000u

/* The camera is 30 fps; the collaborator's frame counts (LOCK_FRAMES 3,
 * RETREAT_CONFIRM_FRAMES 15, REARM_FRAMES 75) are calibrated to that, so the
 * loop paces itself to roughly the same rate rather than spinning as fast as
 * the NPU allows. Sleeping also lets the UI task - which is ABOVE this one -
 * run without contention, which is what keeps the confirm screen responsive
 * while inference is going on. */
#define INTAKE_FRAME_PERIOD_MS    33u

/* Stage 1A runs on one frame in this many. See the note at the call. */
/* How often the PILL detector runs, as a fraction of frames.
 *
 * It is corroboration, not the decision, so it does not need to keep up with
 * the gesture - and its activations are in PSRAM, so a run is not cheap. One
 * frame in four samples it about once a second at the measured loop rate,
 * which is far more often than a verdict changes.
 *
 * WHY IT IS STILL HERE AT ALL, now that the hand decides: the Program Plan
 * commits to CNN classification of pills, and the honest position is that the
 * HOPPER does the classification - mechanically, one medicine per hopper,
 * which is foolproof in a way vision is not. The detector's job is to
 * corroborate that a pill was present in the frame where a hand reached the
 * mouth, which is a real contribution to the adherence claim without pretending
 * to be the thing that identifies the medicine.
 * See PROGRAM_PLAN_RECONCILIATION.md. */
#define PILL_EVERY_N              4u

/* How often the hand model runs: EVERY frame.
 *
 * It was 2, on the assumption that inference would be cheap enough to want
 * pacing. Hardware measured 285-302 ms per run, which means the loop is
 * NPU-bound whatever this is set to - the 33 ms sleep is noise beside it, and
 * skipping every other frame saved nothing while costing observations.
 *
 * Setting it to 1 also removes something that had quietly become
 * load-bearing. On a skipped frame neither branch of the grab/run block ran,
 * so `hand` KEPT ITS PREVIOUS VALUE and one real detection covered two state
 * machine frames. SIMPLE_HOLD asks for three consecutive frames in the mouth
 * zone, and it was being satisfied by two model hits stretched across four.
 * The verdict was true and the evidence for it was half invented.
 *
 * At 1, one loop iteration is one inference is one observation, and three
 * consecutive holds means three times the model actually saw a hand at the
 * mouth - about 0.9 s at the measured rate, which is the right order for a
 * deliberate gesture. */
#define HANDNET_EVERY_N           1u

/* ══════════════════════════════════════════════════════════════════════
 * THE VERDICT RESTS ON MOUTH OCCLUSION
 * ══════════════════════════════════════════════════════════════════════
 *
 * Stage 1C searches the ROI for a hand and Stage 1A searches it for a pill.
 * Both work, in the sense that they find things; neither could be stopped
 * from finding the WRONG thing, because a search over a whole region has more
 * ways to succeed than the question has correct answers. Four hardware rounds
 * went into rejecting ears, re-lit jawlines and wall texture one at a time.
 *
 * The occlusion test asks a different question: is the MOUTH covered? The
 * mouth position is already known from the landmarks and the ROI is centred
 * on it, so this is a measurement on one small known patch rather than a
 * search. An ear is not in the patch. Nothing outside the mouth can produce
 * a false positive, whatever it is and however it moves.
 *
 * OCCLUDE_PCT is how much of that patch must be covered. 55% rather than
 * something higher because a hand delivering a tablet approaches from below
 * and covers the lower lip and chin first - demanding near-total coverage
 * would only fire at the deepest part of the gesture, if at all.
 *
 * The hand and pill stages are KEPT, for the box the overlay draws and for
 * the corroboration flag that decides whether the audit line may say "pill"
 * or must say "hand". They no longer decide anything. */
#define OCCLUDE_PCT               55u

/* Written by the UI task, read by the AI task. Single writer each way, and
 * each is a single aligned word, so no lock is needed - the same argument
 * ai_vision.c makes for its capture result buffers. */
static volatile bool    s_start_req;
static volatile bool    s_stop_req;
static volatile bool    s_active;
static volatile uint8_t s_pills_required = 1u;

static intake_result_t  s_public;      /* the UI's snapshot */
static intake_view_t    s_view;        /* the overlay's snapshot */

const intake_view_t *intake_view(void) { return &s_view; }
/* No image buffer here on purpose. The ROI is written straight into the
 * network's own input via intake_detect_input(), so there is exactly one
 * 76,800-byte image buffer in the system rather than two, and no copy between
 * them. On this build the RAM region is the constrained one,
 * so that is not a micro-optimisation. */

void intake_begin(uint8_t pills_required)
{
    s_pills_required = (pills_required > 0u) ? pills_required : 1u;

    /* Publish a clean "watching, nothing concluded" result immediately, so
     * that a patient who taps the button within the first frame still gets a
     * well-formed log suffix rather than a stale verdict from last time. */
    memset(&s_public, 0, sizeof(s_public));
    s_public.state          = INTAKE_SEARCHING;
    s_public.pills_required = s_pills_required;

    memset(&s_view, 0, sizeof(s_view));

    s_stop_req  = false;
    s_start_req = true;
    ai_vision_wake_intake();
}

void intake_end(void)
{
    s_stop_req  = true;
    s_start_req = false;
    /* The camera is stopped by the AI task on its way out of the loop. If it
     * never started - detector missing, say - stop it here too so the DCMIPP
     * cannot be left aimed at PSRAM. Idempotent by design. */
    if (!s_active) {
        intake_camera_stop();
    }
}

const intake_result_t *intake_peek(void)
{
    return &s_public;
}

bool intake_is_active(void)
{
    return s_active;
}

void intake_service_run(void)
{
    if (!s_start_req) {
        return;
    }
    s_start_req = false;

    if (!intake_handnet_ready()) {
        /* No model - no corroboration, and that is a complete, correct
         * outcome rather than an error. Say so once and leave. */
        printf("intake: hand model not available; dose will be confirmed "
               "by the button alone.\r\n");
        return;
    }
    if (!intake_camera_start()) {
        return;
    }

    s_active = true;
    intake_fsm_reset(s_pills_required);
    intake_features_reset();

    const uint32_t started = HAL_GetTick();
    mouth_obs_t       mouth;
    intake_features_t feat;
    int16_t roi_x = 0, roi_y = 0, roi_size = 0;
    uint32_t frames = 0u;
    hand_lm_t  lm;
    pill_obs_t hand;
    pill_obs_t pill;
    memset(&lm, 0, sizeof(lm));
    memset(&hand, 0, sizeof(hand));
    memset(&pill, 0, sizeof(pill));
    intake_handnet_reset();
    uint32_t hand_frames = 0u, pill_frames = 0u;
    float    pill_best = 0.0f;
    float    best_conf = 0.0f;      /* highest detector confidence all watch */
    uint8_t  mean_min = 255u, mean_max = 0u;   /* ROI liveness */

    /* Once per dose, from task context: the five CenterFace landmarks in both
     * coordinate spaces. This is the Part 0 verification the Definition of
     * Done asks for - that the tensor decodes to sane pixels AND that the
     * mouth really is at indices 3 and 4 - and it needs a real face, so it
     * prints here rather than being asserted in a comment. */
    ai_vision_dump_landmarks();

    while (!s_stop_req && ((HAL_GetTick() - started) < INTAKE_WATCH_TIMEOUT_MS)) {

        /* Stage 1B - free, from the last successful face detection. The face
         * was captured moments ago in STATE_CAMERA_DISPENSE, so the mouth
         * position is current enough to aim an ROI with; it is not re-run per
         * frame, because that would cost a whole CenterFace pass (209 ms) for
         * a head that has barely moved. */
        if (!ai_vision_get_mouth(&mouth)) {
            memset(&mouth, 0, sizeof(mouth));
        }

        /* ── STAGE 1C ──────────────────────────────────────────────────────────
         *
         * One model, and it is the only NPU work in this loop. Its
         * activations are about 978 KB in PSRAM, so a run is expected to cost
         * a good deal more than the 20.75 ms ST publishes for an all-internal
         * placement - HANDNET_EVERY_N paces it rather than assuming a rate,
         * and the measured time is printed once per watch.
         *
         * The pill detector is gone. It needed the whole arena and the whole
         * of 0x73000000, both of which this model now occupies. */
        uint8_t *det_in = intake_handnet_input();
        const bool run_npu = ((frames % HANDNET_EVERY_N) == 0u);
        if (run_npu && (det_in != NULL) &&
            intake_camera_grab_roi_hwc(&mouth, det_in, HANDNET_SIZE,
                                        &roi_x, &roi_y, &roi_size) &&
            intake_handnet_run(&lm)) {

            const uint8_t m = intake_camera_last_roi_mean();
            if (m < mean_min) mean_min = m;
            if (m > mean_max) mean_max = m;
            if (lm.confidence > best_conf) best_conf = lm.confidence;

            memset(&hand, 0, sizeof(hand));
            if (lm.present) {
                hand_frames++;

                /* Model space (224) -> ROI space -> frame space, in one
                 * factor. The mouth coordinates Stage 2 compares against are
                 * in frame space; getting this wrong would put every distance
                 * in the wrong units and every threshold out by the ROI scale
                 * - silently, because the numbers would still look sane. */
                const float sc = (float)roi_size / (float)HANDNET_SIZE;

                hand.detected   = true;
                hand.confidence = lm.confidence;

                /* THE FINGERTIPS, not the centroid. See intake_handnet.h. */
                hand.cx = (int16_t)(roi_x + (int)((float)lm.tip_x * sc));
                hand.cy = (int16_t)(roi_y + (int)((float)lm.tip_y * sc));

                hand.x  = (int16_t)(roi_x + (int)((float)lm.x * sc));
                hand.y  = (int16_t)(roi_y + (int)((float)lm.y * sc));
                hand.w  = (int16_t)((float)lm.w * sc);
                hand.h  = (int16_t)((float)lm.h * sc);
            }
        } else {
            /* Nothing observed this frame - say so. Leaving the previous
             * observation in place is what let two detections satisfy a
             * three-frame hold; see HANDNET_EVERY_N. */
            memset(&hand, 0, sizeof(hand));
        }

        /* Stage 1A, corroboration only. Its own crop (CHW planar, SIGNED,
         * 160 px) and its own input buffer, so it cannot disturb the hand
         * model's (HWC, unsigned, 224). See intake_camera.c. */
        if ((frames % PILL_EVERY_N) == 0u) {
            uint8_t *pill_in = intake_detect_input();
            int16_t px = 0, py = 0, ps = 0;
            memset(&pill, 0, sizeof(pill));
            if (intake_detect_ready() && (pill_in != NULL) &&
                intake_camera_grab_roi(&mouth, pill_in, PILL_DET_SIZE,
                                        &px, &py, &ps) &&
                intake_detect_run(&pill)) {
                if (pill.confidence > pill_best) {
                    pill_best = pill.confidence;
                }
                if (pill.detected) {
                    pill_frames++;
                }
            }
        }

        /* ── WHAT THE STATE MACHINE FOLLOWS ────────────────────────────────────
         *
         * The fingertip midpoint, when the model says a hand is there.
         *
         * Stages 2 and 3 were written against ONE tracked object and do not
         * care what it is: they measure its distance to the mouth in face
         * widths and time the approach. Handing them a fingertip instead of a
         * hand centroid is what makes MOUTH_ZONE_NORM_DIST correct at the
         * collaborator's original 0.15 again - a fingertip is at the lips
         * exactly when the pill is, which a palm never was.
         *
         * Mouth occlusion is kept as corroboration, in the role the pill
         * detector used to have: it can strengthen an observation and never
         * create one, and it decides whether the audit line may say a pill
         * was seen or must say only that a hand was. */
        pill_obs_t track = hand;

        /* Corroboration: a pill seen in the same window as the hand. It can
         * strengthen an observation and never create one - the hand decides
         * whether anything happened, and this only decides whether the audit
         * line may say a PILL was seen or must say only that a HAND was. */
        track.corroborated = (track.detected && pill.detected);
        if (track.corroborated) {
            track.confidence += 0.25f * pill.confidence;
            if (track.confidence > 1.0f) {
                track.confidence = 1.0f;
            }
        }

        /* Stages 2 and 3 - plain C, no NPU, microseconds. */
        intake_features_update(&track, &mouth, &feat);
        const intake_result_t *r = intake_fsm_update(&feat, &track);
        s_public = *r;
        frames++;

        /* Publish for the overlay. Plain struct assignment
         * of a few words the UI only ever reads - the same single-writer
         * argument ai_vision.c makes for its capture result buffers. */
        s_view.pill            = track;   /* what the FSM actually followed */
        s_view.hand            = hand;
        s_view.tracked_is_hand = true;    /* it is always the hand now */
        s_view.mouth           = mouth;
        s_view.roi_x       = roi_x;
        s_view.roi_y       = roi_y;
        s_view.roi_size    = roi_size;
        s_view.state       = r->state;
        s_view.frames      = frames;
        s_view.pill_frames = hand_frames;
        s_view.valid       = true;

        if (intake_fsm_is_terminal(r)) {
            break;
        }
        osal_delay_ms(INTAKE_FRAME_PERIOD_MS);
    }

    intake_camera_stop();
    s_active   = false;
    s_stop_req = false;
    s_view.valid = false;

    /* One line, from task context, once per dose. Nothing prints inside the
     * loop, because of what putting a
     * printf on a repeating path costs on this board. */
    /* One line, from task context, once per dose - but a line that can be
     * acted on. The first hardware round reported "0 with a pill", and that
     * single number could not distinguish three completely different
     * failures: nothing held up, ROI aimed wrong, or no frames arriving. */
    {
        const int bc = (int)(best_conf * 100.0f);
        printf("intake: watched %lu frames (%lu with a hand), best conf=%d%%, "
               "ROI %dx%d at (%d,%d), roi_mean %u..%u, verdict=%s (%s).\r\n",
               (unsigned long)frames, (unsigned long)hand_frames, bc,
               (int)roi_size, (int)roi_size, (int)roi_x, (int)roi_y,
               (unsigned)mean_min, (unsigned)mean_max,
               intake_verdict_text(&s_public),
               intake_reason_text(s_public.reason));

        /* Stage 1C, separately. Two numbers decide whether the model is
         * doing its job and whether it can be afforded:
         *
         *   hand seen 0, best conf ~0.01   the model never sees a hand. It is
         *                                  trained on TIGHT hand crops and is
         *                                  being given a wide one; that is the
         *                                  known risk of using it without a
         *                                  palm detector in front.
         *   hand seen 0, best conf 0.3-0.5 it half-sees one. Lower PRESENCE_T
         *                                  towards the measured peak.
         *   run time > ~150 ms             raise HANDNET_EVERY_N, or accept a
         *                                  lower sampling rate.
         *
         * The offline evidence covered only the NEGATIVE side (a face scores
         * 0.0078, noise 0.0078, flat grey 0.0117). The positive side has never
         * been measured, and this line is where it gets measured. */
        {
            uint32_t ms = 0u; float bc = 0.0f;
            intake_handnet_stats(&ms, &bc);
            printf("intake: hand seen in %lu of %lu frames, best presence %d%%, "
                   "last run %lums.\r\n",
                   (unsigned long)hand_frames, (unsigned long)frames,
                   (int)(bc * 100.0f), (unsigned long)ms);
            printf("intake: pill detector (corroboration only) saw a pill in %lu "
                   "frames, best conf=%d%%.\r\n",
                   (unsigned long)pill_frames, (int)(pill_best * 100.0f));

            /* The peak to four decimals, and the whole distribution. A
             * rounded percentage cannot tell a saturated output from a real
             * one, and this has now reported 50% five watches running. */
            uint32_t bd[6] = {0};
            intake_handnet_bands(bd);
            printf("intake: presence peak %d.%04d; frames by band "
                   "<.1=%lu <.2=%lu <.3=%lu <.4=%lu <.5=%lu >=.5=%lu\r\n",
                   (int)bc, (int)((bc - (float)(int)bc) * 10000.0f),
                   (unsigned long)bd[0], (unsigned long)bd[1],
                   (unsigned long)bd[2], (unsigned long)bd[3],
                   (unsigned long)bd[4], (unsigned long)bd[5]);

            /* Where the fingertip actually landed. The ROI centre is
             * (112,112) and IS the mouth, so a tight cluster there means the
             * model is not localising and the geometric match is vacuous. */
            uint32_t tn = 0u; int16_t tmx = -1, tmy = -1, tsx = -1, tsy = -1;
            intake_handnet_tips(&tn, &tmx, &tmy, &tsx, &tsy);
            printf("intake: fingertip over %lu detections: mean (%d,%d) span (%d,%d); "
                   "ROI centre is (112,112).\r\n",
                   (unsigned long)tn, (int)tmx, (int)tmy, (int)tsx, (int)tsy);
        }

        {
            uint32_t od = 0u, oa = 0u, oi = 0u;
            ai_overlay_stats(&od, &oa, &oi);
            printf("intake: overlay drew %lu times "
                   "(skipped: %lu not-active, %lu no-data).\r\n",
                   (unsigned long)od, (unsigned long)oa, (unsigned long)oi);
        }

        if ((mean_max == 0u) || (mean_min == mean_max)) {
            printf("intake: WARNING roi_mean is flat (%u) - the DCMIPP is "
                   "probably not delivering frames to PSRAM.\r\n",
                   (unsigned)mean_max);
        }
    }
}

#else  /* !MEDSIGHT_ACTION_RECOGNITION */

/* With the feature cut, every entry point is an empty function and the
 * firmware behaves exactly as it did before the feature existed. The project plan lists
 * action recognition first in the cut order, and that is what makes it a
 * one-line change rather than an unpicking. */
static intake_result_t s_public_off;
void intake_begin(uint8_t pills_required) { (void)pills_required; }
void intake_end(void)                     { }
const intake_result_t *intake_peek(void)  { return &s_public_off; }
bool intake_is_active(void)               { return false; }
static intake_view_t s_view_off;
const intake_view_t *intake_view(void)    { return &s_view_off; }
void intake_service_run(void)             { }

#endif /* MEDSIGHT_ACTION_RECOGNITION */
