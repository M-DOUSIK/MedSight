/* intake_detect.c - Stage 1A: the pill detector.
 *
 * ── WHAT THIS MODEL IS ───────────────────────────────────────────────────
 *
 * A single-class YOLOv8n, 160x160, INT8, trained for this project on the
 * Roboflow RF100 `pills` dataset (451 images, CC BY 4.0). The build pipeline
 * lives in tools/pill_detector/build_pill_detector.py and is reproducible
 * end to end.
 *
 * The weights are this project's own. The action-recognition design came
 * from a collaborator: the three-stage pipeline and the state machine in
 * intake_fsm.c are theirs, and they are the valuable part. The weights that
 * came with that design could not be calibrated for 8-bit against data they
 * had not seen, because their training images are not available, so this
 * project trained its own on a dataset that is public, citable and
 * reproducible end to end.
 *
 * ── WHY THE HEAD IS CUT ──────────────────────────────────────────────────
 *
 * YOLOv8's decode tail does not survive 8-bit quantisation. A softmax over
 * the DFL bins, then Slice/Sub/Add/Div box arithmetic, then a Concat of box
 * coordinates with class scores: those tensors have dynamic ranges that
 * differ by orders of magnitude from each other and from the convolutional
 * feature maps, and per-tensor activation quantisation cannot serve them
 * all at once. The convolutional body, on its own, quantises perfectly
 * well.
 *
 * So the graph is cut after the six raw head convolutions. The NPU runs the
 * convolutional body; this file runs the decode on the Cortex-M55, which is
 * idle ~88% of the time. Measured after the cut:
 *
 *     (90 held-out images, one pill in each)
 *     FP32, cut + this decode   84/90   93.3%   <- decode verified correct
 *     INT8, cut + this decode   85/90   94.4%
 *     agreement, FP32 vs INT8   89/90   98.9%
 *
 * Quantisation is effectively free once the head is off. That is the whole
 * finding, and it is why the model fits on this device at all.
 *
 * ── THE OUTPUT GEOMETRY ──────────────────────────────────────────────────
 *
 * Three scales, strides 8 / 16 / 32 over a 160 px input:
 *
 *     scale 0   20x20 grid   box [1,64,20,20]   cls [1,1,20,20]   OUT_1 / OUT_2
 *     scale 1   10x10 grid   box [1,64,10,10]   cls [1,1,10,10]   OUT_3 / OUT_4
 *     scale 2    5x5  grid   box [1,64, 5, 5]   cls [1,1, 5, 5]   OUT_5 / OUT_6
 *
 * Output order is box0, cls0, box1, cls1, box2, cls2 - confirmed against
 * STAI_PILL_OUT_SIZES_BYTES { 25600, 400, 6400, 100, 1600, 25 }, which is
 * 64*20*20, 1*20*20, 64*10*10, 1*10*10, 64*5*5, 1*5*5. Both branches are
 * STAI_FLAG_CHANNEL_FIRST, which is what the box indexing below assumes.
 *
 * 64 box channels are 4 sides x 16 DFL bins. The class branch is 1 channel
 * because there is exactly one class, `pill`.
 *
 * SINGLE BEST BOX, NO NMS. This device tracks one pill on its way to one
 * mouth, so the argmax anchor is the answer and a full multi-box decode plus
 * non-maximum suppression would be pure cost. ai_vision.c makes exactly the
 * same simplification for faces, for exactly the same reason.
 */

#include "ai/intake.h"
#include "main.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#if MEDSIGHT_ACTION_RECOGNITION

/* The generated network. Produced by tools/pill_detector/build_pill_detector.py
 * via ST Edge AI; weights live in external OSPI NOR tagged.xspi2 and are
 * flashed separately, exactly as the two face networks are (AI_LESSONS.md). */
#if __has_include("stai_pill.h")
#include "stai_pill.h"
#define PILL_NETWORK_PRESENT 1
#else
/* The firmware must build and run before the network has been generated -
 * the rest of the pipeline (Stages 1B, 2, 3, the camera path) is testable
 * without it, and the project plan lists this feature as cuttable.
 * With the network absent the subsystem reports "not available" once and the
 * button confirms doses exactly as it did. */
#define PILL_NETWORK_PRESENT 0
#endif

#define DET_SCALES   3
#define REG_MAX      16
#define NUM_SIDES    4
#define PILL_CONF_THRESHOLD  0.35f   /* the collaborator's conf_thresh, kept */

static const int s_stride[DET_SCALES] = { 8, 16, 32 };
static const int s_grid[DET_SCALES]   = { PILL_DET_SIZE / 8,
                                          PILL_DET_SIZE / 16,
                                          PILL_DET_SIZE / 32 };

static bool s_ready = false;

#if PILL_NETWORK_PRESENT
static stai_network s_net[STAI_PILL_CONTEXT_SIZE]
    __attribute__((aligned(STAI_PILL_CONTEXT_ALIGNMENT)));
static stai_ptr s_in[STAI_PILL_IN_NUM];
static stai_ptr s_out[STAI_PILL_OUT_NUM];

/* ── The generated network's actual contract, read out of stai_pill.h rather
 *    than assumed. Three things differ from the two face networks and each
 *    one would be a silent wrong-answer bug if carried over by habit:
 *
 *  1. THE INPUT IS PREALLOCATED (STAI_PILL_IN_1_FLAGS has
 *     STAI_FLAG_PREALLOCATED). So it is fetched with stai_pill_get_inputs(),
 *     like FaceID's input - NOT bound with set_inputs() like the CenterFace
 *     detector's.
 *
 *  2. THE INPUT IS SIGNED int8 with scale 1/255 and zero point -128:
 *         quantised = (rgb / 255) / (1/255) + (-128) = rgb - 128
 *     ai_vision.c's two networks both take raw 0..255 bytes, so writing raw
 *     bytes here - the obvious thing to do - would offset every pixel by 128
 *     and feed the model an image with its midpoint at white.
 *
 *  3. THE OUTPUTS ARE SIGNED int8, each with its own scale and zero point.
 *     There is no aggregate STAI_PILL_OUT_SCALES macro; only per-output ones,
 *     so the tables are assembled here.                                     */
/* The generated SCALES/OFFSETS macros expand to a BRACED INITIALISER LIST,
 * not to an array, so `STAI_PILL_OUT_1_SCALES[0]` does not compile - it
 * subscripts `{ 0.111f }`. A compound literal indexes correctly but is not a
 * constant expression, so it cannot initialise a static either. The tables are
 * therefore filled once at init, by LOAD_QPARAMS below.
 *
 * Doing it this way rather than transcribing the six scales and six zero
 * points as literals is deliberate: transcribed values would silently go stale
 * the next time the model is regenerated, and a wrong dequantisation scale
 * does not fail loudly - it produces confident, plausible, wrong boxes. */
static float s_out_scale[STAI_PILL_OUT_NUM];
static int   s_out_zp[STAI_PILL_OUT_NUM];

#define LOAD_QPARAMS(n)                                                       do {                                                                          const float   _sc[] = STAI_PILL_OUT_##n##_SCALES;                         const int32_t _zp[] = STAI_PILL_OUT_##n##_OFFSETS;                        s_out_scale[(n) - 1] = _sc[0];                                            s_out_zp[(n) - 1]    = (int)_zp[0];                                   } while (0)

static const stai_size s_out_bytes[STAI_PILL_OUT_NUM] = STAI_PILL_OUT_SIZES_BYTES;
#endif

bool intake_detect_ready(void)
{
    return s_ready;
}

bool intake_detect_init(void)
{
#if !PILL_NETWORK_PRESENT
    printf("intake: pill detector NOT COMPILED IN (stai_pill.h absent). "
           "Action recognition disabled; the button still confirms doses.\r\n");
    s_ready = false;
    return false;
#else
    /* aiPreInitialize() and stai_runtime_init() have already been called by
     * ai_vision_init() on this same task - this network joins the two that
     * are already up rather than bringing the runtime up again. */
    stai_return_code err = stai_pill_init(s_net);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_pill_init failed: %d\r\n", err);
        return false;
    }

    /* Preallocated input - fetch the network's own buffer (see the note on
     * s_out_scale above). */
    stai_size n = STAI_PILL_IN_NUM;
    err = stai_pill_get_inputs(s_net, s_in, &n);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_pill_get_inputs failed: %d\r\n", err);
        return false;
    }

    n = STAI_PILL_OUT_NUM;
    err = stai_pill_get_outputs(s_net, s_out, &n);
    if (err != STAI_SUCCESS) {
        printf("intake: stai_pill_get_outputs failed: %d\r\n", err);
        return false;
    }

    LOAD_QPARAMS(1); LOAD_QPARAMS(2); LOAD_QPARAMS(3);
    LOAD_QPARAMS(4); LOAD_QPARAMS(5); LOAD_QPARAMS(6);

    s_ready = true;
    printf("intake: pill detector ready (YOLOv8n %dx%d INT8, head cut, "
           "decode on CPU).\r\n", PILL_DET_SIZE, PILL_DET_SIZE);
    return true;
#endif
}

#if PILL_NETWORK_PRESENT
/* Dequantise one signed-INT8 output element to float. The generated header
 * carries a scale and a zero point per output tensor; using them is not
 * optional, because the DFL expectation below is a weighted sum whose weights
 * only mean anything in real units. */
static inline float deq(const void *base, int idx, float scale, int zp)
{
    return ((float)((int)((const int8_t *)base)[idx] - zp)) * scale;
}
#endif

uint8_t *intake_detect_input(void)
{
#if PILL_NETWORK_PRESENT
    return s_ready ? (uint8_t *)s_in[0] : NULL;
#else
    return NULL;
#endif
}

bool intake_detect_run(pill_obs_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

#if !PILL_NETWORK_PRESENT
    return false;
#else
    if (!s_ready) {
        return false;
    }

    /* Bias the caller's raw 0..255 CHW bytes IN PLACE into the network's
     * signed int8 input: quantised = rgb - 128, from scale 1/255 and zero
     * point -128. In place because the caller wrote straight into this buffer
     * (intake_detect_input()), which is what removes the second 76,800-byte
     * image buffer and the copy that went with it.
     *
     * Writing raw bytes through unbiased - which is what BOTH face networks
     * want, so it is the habit to resist - would shift every pixel by half the
     * range and hand the model an image whose mid-grey is white. */
    {
        uint8_t *src = (uint8_t *)s_in[0];
        int8_t  *dst = (int8_t  *)s_in[0];
        for (int i = 0; i < (int)STAI_PILL_IN_1_SIZE_BYTES; i++) {
            dst[i] = (int8_t)((int)src[i] - 128);
        }
    }
    SCB_CleanDCache_by_Addr((uint32_t *)s_in[0], STAI_PILL_IN_1_SIZE_BYTES);

    if (stai_pill_run(s_net, STAI_MODE_SYNC) != STAI_SUCCESS) {
        return false;
    }

    for (int i = 0; i < STAI_PILL_OUT_NUM; i++) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)s_out[i], s_out_bytes[i]);
    }

    float best_conf = -1.0f;
    int   best_s = 0, best_gx = 0, best_gy = 0;

    /* Pass 1: find the single most confident anchor across all three scales.
     * Only the class branch is touched here - the box branch is decoded once,
     * for the winner, rather than 756 times for anchors we discard. */
    for (int s = 0; s < DET_SCALES; s++) {
        const int   g   = s_grid[s];
        const void *cls = (const void *)s_out[(s * 2) + 1];
        const float sc  = s_out_scale[(s * 2) + 1];
        const int   zp  = s_out_zp[(s * 2) + 1];

        for (int i = 0; i < (g * g); i++) {
            const float logit = deq(cls, i, sc, zp);
            /* Sigmoid. The class branch is a raw logit - the Sigmoid that
             * used to follow it was part of the tail that got cut. */
            const float conf = 1.0f / (1.0f + expf(-logit));
            if (conf > best_conf) {
                best_conf = conf;
                best_s    = s;
                best_gy   = i / g;
                best_gx   = i % g;
            }
        }
    }

    out->confidence = best_conf;
    if (best_conf < PILL_CONF_THRESHOLD) {
        out->detected = false;
        out->source   = PILL_SRC_NONE;
        return true;              /* ran fine; there is simply no pill */
    }

    /* Pass 2: decode the winner's box. 64 channels = 4 sides x 16 DFL bins,
     * channel-first, so side k bin j is channel (k*16 + j) at this cell. */
    {
        const int   g   = s_grid[best_s];
        const int   cell = (best_gy * g) + best_gx;
        const void *box = (const void *)s_out[best_s * 2];
        const float sc  = s_out_scale[best_s * 2];
        const int   zp  = s_out_zp[best_s * 2];
        float dist[NUM_SIDES];

        for (int k = 0; k < NUM_SIDES; k++) {
            float bins[REG_MAX];
            float mx = -1.0e30f;
            for (int j = 0; j < REG_MAX; j++) {
                const int idx = (((k * REG_MAX) + j) * g * g) + cell;
                bins[j] = deq(box, idx, sc, zp);
                if (bins[j] > mx) mx = bins[j];
            }
            /* Softmax, then its expectation over the bin indices - this is
             * the "distribution focal loss" decode, and the expectation is
             * the distance from the anchor to that side of the box, in grid
             * units. Max-subtraction keeps expf() from overflowing. */
            float sum = 0.0f;
            for (int j = 0; j < REG_MAX; j++) {
                bins[j] = expf(bins[j] - mx);
                sum += bins[j];
            }
            float acc = 0.0f;
            if (sum > 0.0f) {
                for (int j = 0; j < REG_MAX; j++) {
                    acc += ((float)j) * (bins[j] / sum);
                }
            }
            dist[k] = acc;
        }

        const float stride = (float)s_stride[best_s];
        const float ax = ((float)best_gx) + 0.5f;   /* anchor centre */
        const float ay = ((float)best_gy) + 0.5f;
        const float x1 = (ax - dist[0]) * stride;
        const float y1 = (ay - dist[1]) * stride;
        const float x2 = (ax + dist[2]) * stride;
        const float y2 = (ay + dist[3]) * stride;

        out->detected = true;
        out->source   = PILL_SRC_YOLO_ROI;
        out->x = (int16_t)x1;
        out->y = (int16_t)y1;
        out->w = (int16_t)(x2 - x1);
        out->h = (int16_t)(y2 - y1);
        out->cx = (int16_t)((x1 + x2) * 0.5f);
        out->cy = (int16_t)((y1 + y2) * 0.5f);
    }
    return true;
#endif
}

#else  /* !MEDSIGHT_ACTION_RECOGNITION */

bool intake_detect_init(void)                               { return false; }
bool intake_detect_ready(void)                              { return false; }
uint8_t *intake_detect_input(void)          { return NULL; }
bool intake_detect_run(pill_obs_t *out)     { (void)out; return false; }

#endif /* MEDSIGHT_ACTION_RECOGNITION */
