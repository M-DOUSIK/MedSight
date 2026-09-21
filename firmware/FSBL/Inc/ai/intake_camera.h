/* intake_camera.h - the camera path for action recognition.
 *
 * ── THE HARD PART, AND IT IS NOT ABOUT THE MODEL ─────────────────────────
 *
 * The DCMIPP has always DMA'd straight into BUFFER_ADDRESS
 * (0x34200000), which IS the display framebuffer - camera preview works by
 * the camera and the display sharing one buffer. That is why resuming the
 * camera behind a drawn
 * screen erases it at ~30 fps, and why every screen from the face capture
 * onward is static UI with the camera STOPPED.
 *
 * Action recognition breaks that arrangement: it needs the camera running for
 * the whole STATE_CONFIRM_TAKEN window WHILE the UI keeps drawing the confirm
 * screen. Both cannot own one buffer. So the camera is pointed somewhere else
 * entirely - external PSRAM - and the framebuffer stays the UI's alone.
 *
 * ── WHERE, AND WHY THAT ADDRESS ──────────────────────────────────────────
 *
 * PSRAM (XSPI1) is 16 MB at 0x90000000. It is NOT empty: MobileFaceNet uses
 * it as activation scratch. Its extent was established the same way
 * the AXISRAM one was - by extracting every address
 * literal from the generated sources rather than trusting the pool
 * declaration, which claims all 16 MB:
 *
 *     0x90000000.. 0x90310000     3,211,264 bytes   MobileFaceNet scratch
 *     0x90310000.. 0x91000000    12.94 MB           free
 *
 * The intake frame sits at 0x90400000 - the next 4 MB boundary, leaving
 * ~0.9 MB of unclaimed slack above the embedder rather than butting against
 * it. One 800x480 RGB565 frame is 768,000 bytes, so it ends at 0x904BB800.
 *
 * ── LPEN: THE PART THAT COST SIX ROUNDS LAST TIME ────────────────────────
 *
 * The sleep-clock rule is a standing constraint, and this module is
 * the first thing in a long time to add a DMA destination. WFI on this part
 * enters CSleep, which stops the clock of every peripheral, bus and memory
 * whose LPEN bit is clear. A DMA master writing into a gated destination
 * produces a silent, intermittent fault that no register dump can see,
 * because the CPU only reads when it is awake.
 *
 * ms_configure_sleep_clocks() covers AXISRAM3-6, the AXI bus matrix, LTDC,
 * DCMIPP, CSI, DMA2D, SDMMC2 and the NPU. It does NOT cover XSPI1, because
 * until now nothing DMA'd there. TWO bits are needed, not one:
 *
 *     RCC_AHB5LPENR_XSPI1LPEN    the PSRAM controller itself
 *     RCC_AHB5LPENR_XSPIMLPEN    the XSPI MANAGER, which every memory-mapped
 *                                access is routed through - gating it stalls
 *                                the transfer just as completely
 *
 * Both are added in ms_configure_sleep_clocks() in the same change as this
 * file, which is what the standing rule requires, and the whole thing MUST be
 * tested from a COLD BOOT - the only condition under which the original fault
 * ever appeared.
 *
 * Worth recording while here: XSPI2LPEN is also clear, and the NPU reads its
 * weights from OSPI NOR at 0x71000000. That has never bitten only because the
 * ST runtime is built LL_ATON_OSAL_BARE_METAL and POLLS, so the CPU is awake
 * for the whole inference and never sleeps mid-run. It is latent, not live -
 * but it is the same fault shape, and if inference is ever made to block on
 * an OS primitive instead of spinning, that bit becomes mandatory.
 */
#ifndef AI_INTAKE_CAMERA_H
#define AI_INTAKE_CAMERA_H

#include "ai/intake.h"
#include <stdbool.h>
#include <stdint.h>

/* One 800x480 RGB565 frame, above MobileFaceNet's PSRAM working set. */
#define INTAKE_FRAME_ADDR   0x90400000u
#define INTAKE_FRAME_W      800
#define INTAKE_FRAME_H      480
#define INTAKE_FRAME_BYTES  (INTAKE_FRAME_W * INTAKE_FRAME_H * 2)

/** Point the DCMIPP at PSRAM and start it. The display framebuffer is not
 *  touched, so the caller may keep drawing throughout - that is the whole
 *  point of this module.
 *
 *  Returns false if the pipe would not start; the caller must treat that as
 *  "no corroboration this dose" and carry on, never as a dose failure. */
bool intake_camera_start(void);

/** Stop the DCMIPP and restore it to the display framebuffer, so that the
 *  next ordinary preview (STATE_CAMERA_DISPENSE) behaves exactly as it always
 *  has. Safe to call when not started. */
void intake_camera_stop(void);

/** True between a successful start and the matching stop. */
bool intake_camera_is_running(void);

/** Copy the region of interest out of the PSRAM frame into a caller-owned
 *  buffer, downscaling to the pill detector's square input.
 *
 *  The ROI is centred on the mouth when one is known, because that is where a
 *  pill about to be swallowed is - and because the detector is a CLOSE-UP
 *  detector, so feeding it a whole 800x480 scene would shrink a pill to a few
 *  pixels. Falls back to the frame centre when no mouth is known yet.
 *
 *  Snapshots rather than referencing: the DCMIPP is still writing the frame,
 *  so inference must run on a copy. This is the same hold-buffer idiom
 *  ai_vision_run_pipeline() has always used, for the same reason. */
bool intake_camera_grab_roi(const mouth_obs_t *mouth,
                             uint8_t *out_chw, int out_size,
                             int16_t *out_roi_x, int16_t *out_roi_y,
                             int16_t *out_roi_size);

/** The same ROI, written HWC interleaved and UNSIGNED, for the hand landmark
 *  model. See the note at the definition: the two networks' generated headers
 *  demand different layouts and the difference is not cosmetic. */
bool intake_camera_grab_roi_hwc(const mouth_obs_t *mouth,
                                 uint8_t *out_hwc, int out_size,
                                 int16_t *out_roi_x, int16_t *out_roi_y,
                                 int16_t *out_roi_size);

/** Mean green-channel value of the last ROI grabbed, 0..255.
 *
 *  A diagnostic, and a load-bearing one. "0 frames with a pill" is equally
 *  consistent with "nothing was held up", "the ROI is aimed at the wrong
 *  place" and "the DCMIPP is not delivering frames to PSRAM at all" - and the
 *  first hardware round could not tell those apart. A mean that is 0, 255, or
 *  frozen across frames says the camera path is dead; a plausible, varying
 *  mean says frames are arriving and the detector genuinely saw no pill. */
uint8_t intake_camera_last_roi_mean(void);

#endif /* AI_INTAKE_CAMERA_H */
