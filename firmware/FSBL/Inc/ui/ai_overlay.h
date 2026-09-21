/* ai_overlay.h - draw what the models actually see.
 *
 * ── WHY THIS EXISTS ──────────────────────────────────────────────────────
 *
 * Every claim this project makes about its AI is, on the device itself,
 * invisible: a face is matched, a pill is tracked, a verdict is written, and
 * all a person watching sees is a screen that changes. The log knows. The
 * viewer does not.
 *
 * This module renders the models' own output onto the LCD - the detector's
 * face box, the five CenterFace landmarks, the pill detector's box, the ROI it
 * searched, and the intake state machine's current state. It is the difference
 * between telling a judge the NPU is working and showing them.
 *
 * ── THE FRAME-BUFFER RULE, WHICH DIFFERS BETWEEN THE TWO VIEWS ───────────
 *
 * This is the part to understand before changing anything here, because the
 * two views have genuinely different constraints and only one of them is
 * obvious.
 *
 * **The face view must wait.** Both face networks' activation scratch is
 * hardcoded by ST's codegen to overlap BUFFER_ADDRESS, so between
 * ai_vision_capture_request() and ai_vision_capture_wait() the AI task owns
 * the framebuffer and NOTHING here may draw. ai_overlay_draw_capture() is
 * therefore called only after the capture handshake has completed, when the
 * hold buffer is stable and the NPU has let go.
 *
 * **The intake view does not have to wait**, and that is a consequence of a
 * deliberate decision rather than luck. The pill detector's
 * activations were placed in AI_ARENA at 0x34388000 - chosen precisely because
 * it overlaps nothing the display uses (the technical reference) - and the camera
 * streams to PSRAM at 0x90400000 rather than to the framebuffer. So during an
 * intake watch the UI owns BUFFER_ADDRESS outright, and can render a LIVE
 * overlay while the NPU is mid-inference. The device has never been able to
 * show a camera image with anything drawn on top before; the "Bug 2"
 * is the record of the last attempt.
 *
 * ── COST ─────────────────────────────────────────────────────────────────
 *
 * Both views scale the source image down with a nearest-neighbour CPU loop
 * into a small window rather than blitting a full 800x480 frame. At the sizes
 * below that is ~60k pixel writes per refresh, and the live view is throttled
 * well under the UI task's 10 ms tick. Nothing here uses DMA2D, so nothing
 * here needs a new LPEN bit - see the sleep-clock note for why that
 * sentence is in this header at all.
 */
#ifndef UI_AI_OVERLAY_H
#define UI_AI_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Build switch. Shares MEDSIGHT_ACTION_RECOGNITION's spirit: this is
 * demonstration value, not function, and must be removable without trace. */
#ifndef MEDSIGHT_AI_OVERLAY
#define MEDSIGHT_AI_OVERLAY 1
#endif

/** Open the live face preview: repoint the camera at PSRAM, compose a full
 *  screen around a rounded viewport, and put `greeting` in the title bar with
 *  `hint` underneath the viewport in secondary text.
 *
 *  This REPLACES the raw full-screen camera dump the capture states used to
 *  show. The seconds before a capture are the moment the user most needs to
 *  be told what to do, and until now they were the only moment the device
 *  could not say anything at all - the DCMIPP overwrote every pixel of any
 *  text within one frame.
 *
 *  The caller MUST pair this with ai_overlay_preview_end() AND then let a
 *  whole camera frame land in BUFFER_ADDRESS before requesting a capture.
 *  See the handover note in ai_overlay.c. */
void ai_overlay_preview_begin(const char *greeting, const char *hint,
                               uint16_t accent);

/** Repaint the viewport from the newest PSRAM frame. Self-throttling; call it
 *  every UI tick while the preview is open. Returns false when it declined to
 *  draw or the preview is not open. */
bool ai_overlay_preview_tick(void);

/** Close the preview and point the camera pipe back at BUFFER_ADDRESS. */
void ai_overlay_preview_end(void);

/** Draw "what the face detector saw": the frozen crop the NPU ran on, with its
 *  chosen box and the five landmarks marked, plus the confidence, under `title` (NULL for a generic one).
 *
 *  Call ONLY after a capture has completed. Returns false if there is nothing
 *  to show (no successful detection yet, or the feature is compiled out), in
 *  which case the caller should carry on exactly as before. */
bool ai_overlay_draw_capture(const char *title);

/** Draw the capture result screen. `ms` is ADVISORY and currently ignored:
 *  the dwell became STATE_CAPTURE_RESULT, which waits for the NEXT button
 *  instead of timing out. The parameter is kept so the call sites still read
 *  as "show the result" rather than "draw", and so reinstating a timed dwell
 *  would not need a signature change.
 *
 *  Returns false if there is nothing to show, in which case the caller must
 *  carry the flow on itself rather than parking the user on a blank screen.
 *  */
bool ai_overlay_show_capture(const char *title, uint32_t ms);

/** Draw the live intake view: camera frame from PSRAM, the searched ROI, the
 *  pill box when the detector has one, the mouth position, and the state
 *  machine's current state.
 *
 *  Safe to call every UI tick; it throttles itself and returns false when it
 *  chose not to draw. Only meaningful while an intake watch is active. */
bool ai_overlay_tick(void);

/** How many times the live view has drawn, and how many times it declined,
 *  since the last reset. Reported once per dose so "no overlay appeared" is a
 *  diagnosable statement rather than an impression. */
void ai_overlay_stats(uint32_t *drawn, uint32_t *skipped_inactive,
                      uint32_t *skipped_invalid);

/** Forget any drawn state, so the next screen starts clean. Called on every
 *  state change, the same way carer_ui's clock strip is retired. */
void ai_overlay_reset(void);

#endif /* UI_AI_OVERLAY_H */
