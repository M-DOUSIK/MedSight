/* intake.h - action recognition: did the patient actually take the pill?
 *
 *
 * ── WHAT THIS IS, AND WHAT IT IS NOT ─────────────────────────────────────
 *
 * This subsystem CORROBORATES the "✓ I Took It" button. It never replaces
 * it and it never gates a dose. That was decided by the project owner
 * before any of this was built, and the
 * reason is worth restating at the top of the header rather than buried:
 * the button is proven on hardware and a patient understands it, whereas a
 * model failure that could block a confirmation would mean a dose this
 * device is unable to record. The model's verdict becomes EVIDENCE in the
 * audit log - "(on time, gesture confirmed)" - and nothing more.
 *
 * Everything here is behind MEDSIGHT_ACTION_RECOGNITION (see below). With
 * it at 0 the firmware behaves exactly as it did before.
 *
 * ── THE THREE STAGES, AND WHERE EACH ONE RUNS ────────────────────────────
 *
 * The design is a collaborator's (the collaborator's summary). It
 * is a three-stage pipeline, NOT the temporal CNN this project assumed for
 * four months, and that distinction is why this was affordable:
 *
 *   Stage 1A  pill detector          YOLOv8n INT8 on the Neural-ART NPU
 *   Stage 1B  mouth position         DECODED from the CenterFace detector
 *                                    that has run from the start
 *   Stage 2   geometric features     plain C, no NPU        (intake_features.c)
 *   Stage 3   temporal decision      rule-based state machine (intake_fsm.c)
 *
 * Stage 1B is the cheap surprise here. CenterFace emits five
 * landmarks - two eyes, a nose and BOTH MOUTH CORNERS - on the same 32x32
 * grid as its heatmap, on a tensor ai_vision.c has always #define'd as
 * FD_OUT_LANDMARKS and has never once read. So mouth tracking costs no
 * second model, no extra inference and no extra memory: it is a decode of
 * something the existing 209 ms detector already produced.
 *
 * Stage 3 needs no neural network at all. The collaborator's own
 * main/main.py implements it as GuardedIntakeStateMachine - named states
 * and thresholds - even though their the collaborator's summary proposes a "Tiny TCN" as
 * future work. What is ported here is what they actually built and ran.
 *
 * ── WHAT WE LOST RELATIVE TO THEIR PYTHON, STATED PLAINLY ────────────────
 *
 * Their Stage 1B was MediaPipe Face Mesh (468 landmarks); ours is
 * CenterFace (5). Their pill tracker also used MediaPipe Hands. Neither
 * runs on this part. Three capabilities therefore do not exist here, and
 * they are proxied or dropped - never faked:
 *
 *  1. MOUTH-OPEN DETECTION IS GONE. Two mouth corners give a centre and a
 *     width. They do not give an upper and a lower lip, so there is no
 *     signal behind mouth_opening and none behind is_open. Their state
 *     machine used is_open in two transitions. Rather than invent a value,
 *     the transition that depended on it now resolves to UNCERTAIN - see
 *     intake_fsm.c's AT_MOUTH case. A device that says "I could not tell"
 *     is behaving correctly; one that says CONSUMED on no evidence is not.
 *
 *  2. THE INNER-LIP POLYGON IS GONE. Their APPROACHING->AT_MOUTH test was
 *     a point-in-polygon against the inner lip contour. Ours is a radial
 *     test about the mouth centre, scaled by mouth width. That is a proxy
 *     for the POLYGON, and it is a fair one because a mouth is roughly an
 *     ellipse about that centre. It is NOT a proxy for is_open, and must
 *     never be read as one.
 *
 *  3. HAND TRACKING IS GONE, so there is no pinch point and no kinematic
 *     estimation of an occluded pill. Their tracker kept a pill "alive"
 *     for up to 30 frames behind a hand by offsetting from the pinch
 *     point. We cannot, so an occluded pill is simply not visible, and
 *     Stage 3's tolerance for that is what RETREAT_CONFIRM_FRAMES buys.
 *     It also means Stage 1A runs on a MOUTH-centred ROI rather than a
 *     HAND-centred one.
 *
 * ── FRAME-BUFFER OWNERSHIP (do not break this) ───────────────────────────
 *
 * the technical reference requires exactly one owner of BUFFER_ADDRESS
 * at a time. Action recognition runs on the EXISTING ai task at priority 3
 * with the same event-flag handshake - there is no second AI task - and it
 * reads camera frames from the PSRAM ring, never from
 * BUFFER_ADDRESS. That is what lets the UI keep drawing the confirm screen
 * while the camera runs.
 */
#ifndef AI_INTAKE_H
#define AI_INTAKE_H

#include <stdbool.h>
#include <stdint.h>

/* ── Build switch ────────────────────────────────────────────────────────
 * On the MEDSIGHT_PHYSICAL_DISPENSER pattern:
 * the feature stays cuttable near the deadline, and the cut is a one-line
 * change rather than an unpicking. The project plan lists this feature
 * first in the cut order precisely because of this switch. */
#ifndef MEDSIGHT_ACTION_RECOGNITION
#define MEDSIGHT_ACTION_RECOGNITION 1
#endif

/* ── SIMPLE MODE (default ON for the prototype) ──────────────────────────
 *
 * Project owner's call, taken after three hardware rounds:
 *
 *   "if it goes enough... taken... else not taken... for prototype only na"
 *
 * With it at 1, a pill that reaches the mouth zone is counted as taken. The
 * staged approach tracking and the empty-retreat verification are skipped.
 *
 * WHAT THIS COSTS, STATED SO IT IS NOT REDISCOVERED LATER. The guarded
 * machine exists to separate "swallowed" from "held at the lips and taken
 * away", and this device has OBSERVED that difference three times on hardware
 * ("pill reappeared in hand during retreat"). In simple mode those three would
 * each be counted as taken. That is a real false positive on an adherence
 * record, and it is why the audit wording changes with the mode: simple mode
 * logs "pill reached mouth", never "gesture confirmed", because reaching the
 * mouth is precisely what was observed and swallowing is not.
 *
 * WHY IT IS NEVERTHELESS THE RIGHT SETTING TODAY. The full machine needs three
 * CONSECUTIVE detections to leave SEARCHING. Measured per-frame detection with
 * a 12 mm object is ~7%, which makes that lock essentially unreachable, so the
 * guarded machine cannot render a verdict at all - and an unreachable guard
 * protects nothing. A simpler rule that fires is more useful than a stricter
 * one that never does, provided the log says what was actually seen.
 *
 * Set to 0 to restore the full GuardedIntakeStateMachine, which is intact. */
#ifndef MEDSIGHT_INTAKE_SIMPLE
#define MEDSIGHT_INTAKE_SIMPLE 1
#endif

/* ── SIMPLE MODE (default ON for the prototype) ──────────────────────────
 *
 * Project owner's call, taken after three hardware rounds:
 *
 *   "if it goes enough... taken... else not taken... for prototype only na"
 *
 * With it at 1, a pill that reaches the mouth zone is counted as taken. The
 * staged approach tracking and the empty-retreat verification are skipped.
 *
 * WHAT THIS COSTS, STATED SO IT IS NOT REDISCOVERED LATER. The guarded
 * machine exists to separate "swallowed" from "held at the lips and taken
 * away", and this device has OBSERVED that difference three times on hardware
 * ("pill reappeared in hand during retreat"). In simple mode those three would
 * each be counted as taken. That is a real false positive on an adherence
 * record, and it is why the audit wording changes with the mode: simple mode
 * logs "pill reached mouth", never "gesture confirmed", because reaching the
 * mouth is precisely what was observed and swallowing is not.
 *
 * WHY IT IS NEVERTHELESS THE RIGHT SETTING TODAY. The full machine needs three
 * CONSECUTIVE detections to leave SEARCHING. Measured per-frame detection with
 * a 12 mm object is ~7%, which makes that lock essentially unreachable, so the
 * guarded machine cannot render a verdict at all - and an unreachable guard
 * protects nothing. A simpler rule that fires is more useful than a stricter
 * one that never does, provided the log says what was actually seen.
 *
 * Set to 0 to restore the full GuardedIntakeStateMachine, which is intact. */
#ifndef MEDSIGHT_INTAKE_SIMPLE
#define MEDSIGHT_INTAKE_SIMPLE 1
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Stage 1 output - the interface between detection and geometry.
 * Mirrors the collaborator's "STAGE 1 OUTPUT FRAME OBJECT (F_data)" at the
 * end of the collaborator's summary, minus the fields no signal backs (see the header note).
 * ══════════════════════════════════════════════════════════════════════════ */

/** Where a pill observation came from. Kept because the collaborator's state
 *  machine gates its initial lock on "a real detector said so" rather than on
 *  any estimate - their `is_direct_yolo`. We have fewer modes than they do
 *  (no kinematic estimation without hand tracking), but the distinction is
 *  the load-bearing part and it survives. */
typedef enum {
    PILL_SRC_NONE = 0,     /* nothing found this frame                      */
    PILL_SRC_YOLO_ROI,     /* detector fired inside the mouth-centred ROI   */
    PILL_SRC_YOLO_FRAME    /* detector fired on the downscaled full frame   */
} pill_source_t;

typedef struct {
    bool          detected;
    pill_source_t source;
    int16_t       cx, cy;        /* centre, pixels in ROI/frame space        */
    int16_t       x, y, w, h;    /* bounding box, same space                 */
    float         confidence;    /* 0..1                                     */
    bool          drop_detected; /* rapid downward motion - pill was dropped */
    int16_t       dy;            /* vertical velocity, px/frame              */
    /* Set when Stage 1A saw a pill in the SAME frame as Stage 1C
     * saw the hand. It is the difference between the record being able to say
     * "a pill reached the mouth" and only "a hand reached the mouth", and
     * the two are not the same claim about a person's medication. */
    bool          corroborated;
} pill_obs_t;

typedef struct {
    bool     detected;
    int16_t  cx, cy;             /* mouth centre: midpoint of the two corners */
    int16_t  width;              /* corner-to-corner distance, px             */
    int16_t  face_w;             /* face box width - the normalisation base   */
    float    confidence;         /* the detector's heatmap confidence         */
    /* NO mouth_height and NO is_open. See the header. Adding either without
     * a real signal behind it would be the one thing this project must not
     * do. */
} mouth_obs_t;

/* ══════════════════════════════════════════════════════════════════════════
 * Stage 2 - geometric features.
 * Names deliberately match the collaborator's main/main.py and the collaborator's summary
 * so the C and the Python can be compared line for line when this is tuned.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    bool  pill_visible;
    bool  mouth_visible;

    /* Distance between pill centre and mouth centre, NORMALISED BY FACE
     * WIDTH - exactly their `norm_dist = raw_dist / face_width_px`. This is
     * what makes every threshold below independent of how far the patient is
     * standing from the camera, which is the whole reason they normalised. */
    float norm_dist;
    float norm_dist_prev;
    float distance_velocity;      /* norm_dist_prev - norm_dist; +ve = closing */
    float distance_acceleration;

    bool  in_mouth_zone;          /* radial proxy for their inner-lip polygon */
    float pill_confidence;
} intake_features_t;

/* ══════════════════════════════════════════════════════════════════════════
 * Stage 3 - the guarded state machine.
 * A direct port of GuardedIntakeStateMachine (tools/action_recogntion/
 * main/main.py). State names are kept identical on purpose.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    INTAKE_SEARCHING = 0,          /* SEARCHING_PILL                          */
    INTAKE_LOCKED,                 /* PILL_LOCKED                             */
    INTAKE_APPROACHING,            /* APPROACHING_MOUTH                       */
    INTAKE_AT_MOUTH,               /* AT_MOUTH_CONTACT                        */
    INTAKE_RETREATING_CHECK,       /* VERIFYING_EMPTY_RETREAT                 */

    /* Terminal outcomes */
    INTAKE_CONSUMED,               /* one pill down; re-arms for the next     */
    INTAKE_ALL_CONSUMED,           /* the whole prescribed dose               */
    INTAKE_NOT_CONSUMED_DROPPED,
    INTAKE_NOT_CONSUMED_RETREATED,
    /* Their NOT_CONSUMED_MOUTH_CLOSED cannot be asserted here - it required
     * is_open. The path that reached it now reaches INTAKE_UNCERTAIN. */
    INTAKE_UNCERTAIN
} intake_state_t;

/** UNCERTAIN is a first-class outcome and must stay one.
 *  The rule it was built to: "A system that only ever says CONSUMED or
 *  NOT CONSUMED will be wrong confidently, which is the worst behaviour for
 *  this device." A false "medication taken" is worse than asking again. */
typedef struct {
    intake_state_t state;
    float          confidence;
    uint8_t        pills_consumed;
    uint8_t        pills_required;
    /* Index into the reason table in intake_fsm.c - a small integer rather
     * than a string, so nothing here allocates and the audit line is built
     * by the caller in task context. */
    uint8_t        reason;
    /* True when Stage 1A corroborated the observation that produced this
     * verdict. Read only to choose the wording of the audit suffix. */
    bool           corroborated;
} intake_result_t;

/* ── Stage 2 ─────────────────────────────────────────────────────────────── */
void intake_features_reset(void);
/** Build this frame's feature vector. Safe to call with either observation
 *  not detected; the flags say so and Stage 3 handles it. */
void intake_features_update(const pill_obs_t *pill, const mouth_obs_t *mouth,
                            intake_features_t *out);

/* ── Stage 3 ─────────────────────────────────────────────────────────────── */
void intake_fsm_reset(uint8_t pills_required);
/** Advance one frame. Returns the current result; terminal states latch until
 *  the machine re-arms itself for the next pill (their ~2.5 s cooldown). */
const intake_result_t *intake_fsm_update(const intake_features_t *f,
                                          const pill_obs_t *pill);
/** Human-readable reason for the current state, for the audit log only.
 *  Never NULL. */
const char *intake_reason_text(uint8_t reason);
/** True once the machine has reached a terminal outcome for the whole dose. */
bool intake_fsm_is_terminal(const intake_result_t *r);
/** One short word for the log line: "confirmed", "not observed", "uncertain". */
const char *intake_verdict_text(const intake_result_t *r);

/* ══════════════════════════════════════════════════════════════════════════
 * Stage 1A - the pill detector.
 *
 * Deliberately a NARROW interface with exactly one implementation behind it,
 * because the detector is the one piece of this work that depends on a
 * generated NPU network and on a trained model. Keeping it behind three
 * functions means the rest of the pipeline - Stages 1B, 2 and 3, all of which
 * are testable without any model at all - never has to change when the
 * detector is regenerated, retrained or replaced.
 * ══════════════════════════════════════════════════════════════════════════ */

/** Detector input edge length, in pixels (square). The model is trained and
 *  exported at this size; changing it means regenerating, not just editing.
 *
 *  160 IS A MEASURED CHOICE, not a guess. ST Edge AI reports the INT8
 *  activation working set as:
 *
 *      320x320 (the collaborator's size)   ~1.1 MB   (FP32 measured 4,505,600)
 *      192x192                              267,264 bytes
 *      160x160                              201,600 bytes
 *
 *  AI_ARENA is 225,280 bytes (the technical reference), so 192 overruns it by 42 KB
 *  and 160 fits with 23,680 bytes spare. The arena cannot simply be grown:
 *  AXISRAM6 ends at 0x343BFFF7, which caps it at 229,368 bytes - still short
 *  of 192's requirement.
 *
 *  The alternative was PSRAM, which is NPU-reachable and has 12.94 MB free.
 *  It was rejected on latency, not capacity: PSRAM is THROUGHPUT=MID
 *  LATENCY=HIGH against the arena's HIGH/LOW, and the ported state machine's
 *  frame counts (LOCK_FRAMES 3, RETREAT_CONFIRM_FRAMES 15) are calibrated to
 *  ~30 fps. A detector running at a few frames per second would stretch a
 *  15-frame retreat confirmation into several seconds and change what the
 *  thresholds mean.
 *
 *  And 160 costs nothing measurable: on 90 held-out images the INT8 160px
 *  model detects 84, exactly as the FP32 192px model does. The reason it is
 *  free is that this device feeds a TIGHT mouth-centred ROI (two face widths)
 *  rather than a whole scene, so the pill is already large in frame. */
#define PILL_DET_SIZE 160

/** Bring up the pill-detection network. Call once, from the AI task, after
 *  aiPreInitialize(). Returns false if the network is absent or failed to
 *  initialise - in which case the whole subsystem stays off and the button
 *  continues to confirm doses exactly as it always has. */
bool intake_detect_init(void);

/** True once intake_detect_init() has succeeded. */
bool intake_detect_ready(void);

/** The network's OWN input buffer, to be filled with CHW planar RGB bytes
 *  (0..255, R plane then G then B). Returns NULL if the detector is not ready.
 *
 *  Exposed rather than taking a caller-owned buffer so that there is exactly
 *  ONE image buffer in the system instead of two. At 3 x 160 x 160 that is
 *  76,800 bytes of.bss saved, and one full copy per frame avoided, on a build
 *  whose RAM region is the constrained one. */
uint8_t *intake_detect_input(void);

/** Run the detector on whatever is currently in intake_detect_input().
 *  Coordinates in `out` are in that image's own space; the caller maps them
 *  back to frame space. Returns false only on a runtime error - "no pill in
 *  this frame" is a successful call with out->detected == false. */
bool intake_detect_run(pill_obs_t *out);

/* ══════════════════════════════════════════════════════════════════════════
 * The service - orchestration, on the EXISTING ai task.
 *
 * There is no second AI task and there must not be one: the frame-buffer
 * ownership argument in the technical reference depends on exactly one
 * owner of BUFFER_ADDRESS at a time, and adding a second NPU task would make
 * that a race rather than an invariant.
 * ══════════════════════════════════════════════════════════════════════════ */

/** Called by the UI when STATE_CONFIRM_TAKEN is entered. Starts the camera
 *  into PSRAM and asks the AI task to begin watching. Never blocks, and
 *  never fails in a way the caller must handle - if anything is not ready the
 *  verdict is simply "uncertain" and the button still confirms the dose. */
void intake_begin(uint8_t pills_required);

/** Called by the UI when STATE_CONFIRM_TAKEN is left, for any reason. Stops
 *  the camera and restores the DCMIPP to the display framebuffer. Idempotent. */
void intake_end(void);

/** Non-blocking read of the latest verdict, for the UI to put in the log line
 *  at the moment the patient taps the button. Never NULL. */
const intake_result_t *intake_peek(void);

/** True while the AI task is actively watching. */
bool intake_is_active(void);

/* ── Live view for the on-screen overlay ────────────────────
 *
 * What the intake watcher is currently looking at and what it currently
 * believes. Published by the AI task each frame; read by the UI task to draw.
 *
 * DRAWING WHILE INFERENCE RUNS IS SAFE HERE, and it is worth understanding
 * why, because it is NOT true of the face pipeline. The two face networks'
 * activations are hardcoded to overlap BUFFER_ADDRESS, so the UI must not draw
 * during a face capture. The PILL detector's activations were deliberately
 * placed in AI_ARENA at 0x34388000 (the technical reference), which overlaps nothing
 * the display uses - and the camera writes to PSRAM, not the framebuffer. So
 * during an intake watch the UI owns the framebuffer outright and can render a
 * live overlay while the NPU works. That is a consequence of the memory
 * decisions, not a coincidence. */
typedef struct {
    bool           valid;
    pill_obs_t     pill;        /* frame coordinates                         */
    mouth_obs_t    mouth;       /* frame coordinates                         */
    int16_t        roi_x, roi_y, roi_size;
    intake_state_t state;
    uint32_t       frames;      /* frames watched so far                     */
    uint32_t       pill_frames; /* of those, how many contained a pill       */
    /* Second pass: the HAND is now the tracked object and the
     * pill only corroborates (Inc/ai/intake_hand.h). `pill` above carries
     * whatever the FSM is actually following, so the overlay always draws
     * the box the decision was made from; these say what it is and how the
     * two stages are doing separately. */
    bool           tracked_is_hand;
    pill_obs_t     hand;
    uint32_t       hand_frames;
} intake_view_t;

/** Non-blocking snapshot for the overlay. Never NULL. */
const intake_view_t *intake_view(void);

/** Served by the AI task from inside its own loop - not for anyone else to
 *  call. Declared here only so ai_vision.c can reach it. */
void intake_service_run(void);

#endif /* AI_INTAKE_H */
