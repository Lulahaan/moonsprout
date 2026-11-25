// Software latency probe (docs/ONLINE_MULTIPLAYER.md 2.10, M5.1a): press-to-present on the live
// input path. Host side; the game-side helpers are in port/game/net/probe_patch.cpp.
//
// A key or pad button press (SDL event time) is matched with the first pass whose
// JUTGamePad::read shows a new button bit on port 0 (the pad edge, the pass whose update used the
// press). In a draw-first pass (the stock order: draw, then update) the first picture that can
// show its effect is drawn in the next pass; in an update-first pass (M5.1b, p2_net_pass_ran_update_first)
// it is that same pass's picture. That picture gets a white square at the top left, and the sample
// ends when its present has completed. The square stays for 3 presented frames in total.
//
// Smooth Motion. An in-between picture drawn after the pass that read the press (that pass has
// captured its state, so the picture already moves toward the press's effect) is the first picture
// of the press: it draws the square and ends the sample, and the sample line says "in-between
// picture". The in-between pictures also keep the square while it is held. A preserved frame
// (HostFrames with Smooth Motion off) presents the tick's picture again, square included.
//
// The press is a key or button down event; the pad edge is a new button bit, the main stick
// crossing half deflection (bits 16 to 19 of p2_net_probe_pad_buttons) or the C-stick crossing
// half deflection (bits 20 to 23), so walking, turning and the camera are measured too.
//
// Press time. Wayland and the gamepad backends stamp an event when it happens. SDL's X11 backend
// stamps it when the queue is drained (X11_GetEventTimestamp returns SDL_GetTicksNS, a FIXME in
// SDL 3), which would hide the wait from the press to the next pump (on average half a pass).
// With late sampling (M5.1b) a pass pumps twice: SDL_PumpEvents just before the pad read, and
// pumpEvents (which dispatches the events) after the present. The probe keeps the start and end
// of the last pumps. An event stamped inside one of those intervals is a dispatch stamp: the press
// then counts as the middle of the time from the previous pump's start to that pump's start,
// which is unbiased for a press at a random time. The sample line says which kind of press time
// it used, and the interval.
//
// Off unless --latency-probe / MPPM_LATENCY_PROBE=1 is given, and never in a headless run. Every
// call site tests mppm::probe::g_on first, so the probe costs one predictable branch when off.
#pragma once

#include <SDL3/SDL_events.h>

namespace mppm::probe {

extern bool g_on;

// main(), after p2_net_init. lateSampleUs: the late-sampling margin, 0 when it is off (the 'on' line).
void init(long long lateSampleUs);
// Before every pump of SDL's queue (pumpEvents, and the late-sampling SDL_PumpEvents), and after
// it: the press-time estimate for stamp-at-dispatch backends.
void pump_begin();
void pump_end();
// pumpEvents: every SDL event.
void on_event(const SDL_Event& event);
// After the late-sampling SDL_PumpEvents: the presses the next pad read will see are still in
// SDL's queue (pumpEvents dispatches them after the present), so they are looked at here. A press
// seen here is not counted again when on_event gets it.
void peek_presses();
// Top of p2_host_present (the end of a pass): reads port 0's buttons and looks for the pad edge.
// Asks p2_net_pass_ran_update_first() which order the pass ran in.
void pass_end();
// Before the frame is ended (only while the frame is open): draws the patch when one is due.
void draw_patch();
// Smooth Motion: after an in-between picture is drawn, before it is ended. Draws the patch from the
// pass that read the press on (that pass's state is already captured), and while it is held.
void draw_patch_between();
// HostFrames (no Smooth Motion): a preserved picture is presented again; it has the patch when the
// tick's picture had it.
void preserved_frame();
// After aurora_end_frame / aurora_end_frame_no_present, inside the block region; also after the
// HostFrames loop's in-between and preserved frames (shown).
void frame_ended(bool shown);
// Prints the summary line when there are samples (every 10 samples and on exit).
void summary();

} // namespace mppm::probe
