// Host entry point: bring up Aurora (window, GPU, input, audio device), open the player's disc
// image, then run the game's original boot sequence.
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/main.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <mppm/frame_policy.hpp>
#include <mppm/input_defaults.hpp>
#include <mppm/launcher.hpp>

#include "frame_pacer.hpp"
#include "net/probe.h"
#include "net/session/session.hpp"
#include "p2_host.h"
#include "p2_mods.h"
#include "p2_wide.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int p2_game_main();       // src/sysBootupU/sysBootup.cpp
extern "C" void p2_os_thread_init(); // port/sdk/os_thread.cpp
extern "C" void p2_host_block_begin();
extern "C" void p2_host_block_end();
extern "C" void p2_os_shutdown();    // port/sdk/os_thread.cpp
extern "C" void p2_audio_shutdown(); // port/audio/audio.cpp
extern "C" void p2_game_audio_hold_frame(); // port/game/ui_sound.cpp

extern "C" long long p2_vi_retrace_time_ns(unsigned count); // port/sdk/os_thread.cpp
// Smooth Motion, game side (port/game/present/Present.cpp), beside the p2_host.h contract.
extern "C" void p2_game_present_enable(int on);

static bool s_frameOpen = false;
static SDL_Window* s_window = nullptr;
static bool s_startVsync = true; // AuroraConfig.vsync after the launcher's preinit

// ---- Frame pacing (fps_design phase 1, package A) ---------------------------------------------
// The game's logic rate never changes: ticks stay on the 59.94 Hz retrace queue. The host only
// decides which tick pictures are presented (a cap below the tick rate skips presents), lets the
// start-menu scene run one tick per presented frame (FreeRun), and draws extra UI-only frames while
// a launcher menu is open over the running game (HostFrames).
namespace {

using Clock = FramePacer::clock;
using Policy = mppm::launcher::FramePolicy;
constexpr int64_t kRetraceNs = 1000000000LL * 1001 / 60000;

int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

Clock::time_point at_ns(int64_t ns)
{
	return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns)));
}

// -- Policy: derived from video.frameRate, VSync and the display's refresh rate by the settings
// (frame_policy(), which also logs every new policy under MPPM_FPS).
const Policy& host_policy(int64_t)
{
	return mppm::launcher::frame_policy();
}

// -- State
FramePacer s_presentPacer; // FramePacer::due schedule for presents (game ticks and UI frames)
FramePacer s_sleepPacer;   // FreeRun frame slots
FramePacer s_uiPacer;      // hold loop and standalone launcher
Policy s_applied{};
bool s_appliedValid = false;

bool s_armed       = false; // p2_host_arm_tick for this iteration of Section::main
bool s_freeRunReq  = false; // p2_host_free_run, renewed by the scene every tick
bool s_freeRunNow  = false; // the request as consumed by this tick's wait
bool s_freeRunTick = false; // this tick was made due by FreeRun
float s_tickDt     = 0.0f;
int s_soundFrames  = 1;
double s_soundAcc  = 0.0;
int64_t s_lastTickStart = 0;
int64_t s_tickStart     = 0; // tick-wait return of the running armed tick
bool s_uiTick           = false; // the running tick was reached through HostFrames
int64_t s_uiInterval    = 0;     // HostFrames present spacing
// Smooth Motion (fps_design 7.2)
bool s_smoothTick       = false; // the running tick was reached through the Smooth Motion loop
uint32_t s_dueCount     = 0;     // retrace count that made the newest tick due (alpha clock)
uint32_t s_tickTime     = 1;     // retraces per tick of the running section (waitForTick `time`)
int64_t s_passNs        = 0;     // running estimate of one in-between pass, wait to end of frame
// Tick order (M5.1b, plan 2.10 item 4): p2_host_update_first's latch for the armed pass, -1 until
// its first call in the pass.
int s_updateFirst       = -1;
// Late sampling (M5.1b, plan 2.10 item 1): the pacing wait moves to the end of p2_host_present,
// before the next pass's pad read.
OSMessageQueue* s_tickQueue = nullptr; // the retrace queue the armed waits use
uint32_t s_nextCount    = 0;     // retrace count the next armed tick becomes due at
bool s_nextCountValid   = false; // s_nextCount was set by this pass's tick wait
bool s_heldDue          = false; // the pre-read wait took a due retrace message off the queue
uint32_t s_heldCount    = 0;
bool s_preSmooth        = false; // the pre-read wait showed an in-between picture
// M5.3b online (docs/online/M5_3B_LOCKSTEP.md "Host"): the next armed tick's retrace is known (an armed
// tick wait set s_nextCount since the last unarmed one), and the barrier alignment of p2_host_align_tick.
bool s_dueKnown         = false;
bool s_alignPending     = false;
uint32_t s_alignCount   = 0;

// -- Counters and log (fps_design 5.6)
struct Counters {
	int ticks = 0, shown = 0, skipped = 0, ui = 0, interp = 0;
	double gameSec = 0.0; // tick seconds of ticks after the window's reference tick
	int64_t workNs = 0;
	int workN = 0;
};
Counters s_win, s_total;
int64_t s_winStart = 0;
int64_t s_refTick = 0;    // last tick before the window (speed span start)
int64_t s_lastTick = 0;   // last tick in the window
int64_t s_firstTickEver = 0;
int64_t s_lastTickEver  = 0;
const char* s_mode = "game";
float s_pictureFps = -1.0f;
bool s_winHeld     = false; // the window holds menu-hold frames: its picture rate is not the game's

void maybe_log(int64_t now)
{
	if (s_winStart == 0) {
		s_winStart = now;
		return;
	}
	if (now - s_winStart < 1000000000LL) {
		return;
	}
	const double secs = (now - s_winStart) / 1e9;
	// A window that straddles a menu hold counts only part of a second of game pictures: keep the
	// last clean reading rather than publish a bogus low one (the overlay's pill and the bar read it).
	if (!s_winHeld) {
		s_pictureFps = static_cast<float>((s_win.shown - s_win.ui) / secs);
	}
	s_winHeld = false;
	if (mppm::launcher::log_fps()) {
		const double span  = s_refTick != 0 && s_lastTick > s_refTick ? (s_lastTick - s_refTick) / 1e9 : 0.0;
		const double speed = span > 0.0 ? s_win.gameSec / span : 0.0;
		const double work  = s_win.workN > 0 ? s_win.workNs / 1e6 / s_win.workN : 0.0;
		std::fprintf(stderr, "mppm: %d fps (ticks %d, shown %d, skipped %d, ui %d, interp %d, speed %.3f, work %.1f ms, mode %s)\n",
		             s_win.ticks, s_win.ticks, s_win.shown, s_win.skipped, s_win.ui, s_win.interp, speed, work, s_mode);
	}
	s_win      = {};
	s_winStart = now;
	if (s_lastTick != 0) {
		s_refTick = s_lastTick;
	}
}

void count_tick(bool shown, int64_t now)
{
	const float dt = p2_game_tick_seconds();
	for (Counters* c : { &s_win, &s_total }) {
		c->ticks++;
		(shown ? c->shown : c->skipped)++;
		if (s_tickStart != 0) {
			c->workNs += now - s_tickStart;
			c->workN++;
		}
	}
	if (s_refTick == 0) {
		s_refTick = now; // first tick ever: the reference, not counted
	} else {
		s_win.gameSec += dt;
		s_total.gameSec += dt;
	}
	if (s_firstTickEver == 0) {
		s_firstTickEver = now;
	}
	s_lastTick      = now;
	s_lastTickEver  = now;
	s_tickStart     = 0;
	maybe_log(now);
}

void count_interp(int64_t now)
{
	s_win.interp++;
	s_win.shown++;
	s_total.interp++;
	s_total.shown++;
	maybe_log(now);
}

void count_ui(int64_t now)
{
	s_win.ui++;
	s_win.shown++;
	s_total.ui++;
	s_total.shown++;
	maybe_log(now);
}

void print_totals()
{
	if (!mppm::launcher::log_fps()) {
		return;
	}
	const double span  = s_lastTickEver > s_firstTickEver ? (s_lastTickEver - s_firstTickEver) / 1e9 : 0.0;
	const double speed = span > 0.0 ? s_total.gameSec / span : 0.0;
	std::fprintf(stderr, "mppm: totals (ticks %d, shown %d, skipped %d, ui %d, interp %d, speed %.4f over %.1f s, work %.1f ms)\n",
	             s_total.ticks, s_total.shown, s_total.skipped, s_total.ui, s_total.interp, speed, span,
	             s_total.workN > 0 ? s_total.workNs / 1e6 / s_total.workN : 0.0);
}

// Notes a policy change (live setting change, display change): restart the schedules.
const Policy& refresh_policy(int64_t now)
{
	const Policy& p = host_policy(now);
	if (!s_appliedValid || p.original != s_applied.original || p.capHz != s_applied.capHz || p.sleepNs != s_applied.sleepNs ||
	    p.uiSleepNs != s_applied.uiSleepNs || p.refreshHz != s_applied.refreshHz) {
		s_applied      = p;
		s_appliedValid = true;
		s_presentPacer.reset();
		s_sleepPacer.reset();
		s_uiPacer.reset();
	}
	return p;
}

bool free_run_in_effect(const Policy& p)
{
	return s_freeRunNow && !p.original && !mppm::launcher::hold_game();
}

// HostFrames for UI frames (5.5): a launcher menu is open over the running (not held) game and
// the cap allows more pictures than the section's tick rate.
bool host_frames_wanted(const Policy& p, uint32_t time)
{
	if (p.original || !mppm::launcher::ui_visible() || mppm::launcher::hold_game()) {
		return false;
	}
	const double tickHz = 60000.0 / 1001.0 / static_cast<double>(time);
	return p.capHz <= 0.0f || p.capHz > tickHz + 5.0;
}

// Smooth Motion (fps_design 7): in-between pictures of the game itself while waiting for the
// tick, when the setting is on, the game is not held, and the cap allows more pictures than the
// section's tick rate. The game side adds its own guards (p2_game_present_ready).
bool smooth_wanted(const Policy& p, uint32_t time)
{
	// Harness runs (M5.1a): Smooth Motion re-runs the game's draw, and the HUD calls rand() there.
	if (!p.smoothMotion || p.original || p2_net_active() || mppm::launcher::hold_game()) {
		return false;
	}
	const double tickHz = 60000.0 / 1001.0 / static_cast<double>(time);
	return p.capHz <= 0.0f || p.capHz > tickHz + 5.0;
}

// Where the newest tick's picture sits between the two newest captures: 0 when that tick became
// due, 1 one tick period later, linear in wall time (fps_design 7.2).
float smooth_alpha(int64_t now, uint32_t time)
{
	// Test hook: MPPM_SMOOTH_ALPHA=<-8..1> draws every in-between picture at that fixed alpha
	// (1 must look exactly like the next tick's own picture, 0 like the current one).
	// A negative value extrapolates backwards (down to -8), which magnifies anything drawn at the
	// tick's state instead of the lerped one.
	static const char* s_fixedEnv = std::getenv("MPPM_SMOOTH_ALPHA");
	static const float s_fixed    = s_fixedEnv != nullptr && *s_fixedEnv != '\0' ? static_cast<float>(std::atof(s_fixedEnv)) : NAN;
	if (!std::isnan(s_fixed)) {
		return std::clamp(s_fixed, -8.0f, 1.0f);
	}
	const int64_t due = p2_vi_retrace_time_ns(s_dueCount);
	const double a    = static_cast<double>(now - due) / (static_cast<double>(time) * kRetraceNs);
	return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// 5.3: whether a finished tick's picture is presented. Never delays the tick beyond the bounded
// wait for the previous present. `limit` is 2 ms before the next retrace.
bool present_due(const Policy& p, int64_t now, int64_t limit)
{
	if (s_uiTick) {
		// HostFrames: the tick's new picture always takes a slot (UI frames leave room for it),
		// and the UI frames continue one interval after it.
		s_presentPacer.restart(at_ns(now + s_uiInterval));
	} else if (!s_presentPacer.due(at_ns(now), std::chrono::nanoseconds(p.presentNs))) {
		return false;
	}
	while (aurora_presents_in_flight() > 0 && now_ns() < limit) {
		SDL_DelayNS(200000);
	}
	return aurora_presents_in_flight() == 0;
}

// Sleep target for a loop that polls the tick queue: the next present slot or the tick's retrace,
// whichever is first, and at least 200 us so the poll does not spin.
void sleep_poll(Clock::time_point target)
{
	const Clock::time_point floor = Clock::now() + std::chrono::microseconds(200);
	FramePacer::sleep_until(target > floor ? target : floor);
}

} // namespace

// Leaves the running game: the window was closed, or the harness says the run is over
// (--exit-after-ticks, end of a replay, a checksum mismatch). Other game threads, the audio threads
// and the scheduler's timer are still alive: park them, close the audio device, then leave without
// running static destructors (they would tear down state those threads still use). Memory card
// writes are already committed to disk as they happen.
[[noreturn]] static void host_quit(int code)
{
	p2_net_shutdown(); // flushes and closes the harness logs; writes and verifies the last lines
	if (code == 0) {
		code = p2_net_exit_code(); // read after the shutdown: the last L and the E line are verified there
	}
	if (mppm::probe::g_on) {
		mppm::probe::summary();
	}
	print_totals();
	p2_os_shutdown();
	p2_audio_shutdown();
	aurora_dvd_close();
	aurora_shutdown();
	std::fflush(nullptr);
	std::_Exit(code);
}

// Window closed during a harness session: the quit waits for the end of the pass (the check after
// the pump in p2_host_present), so the logs never end on a tick that was read but not simulated.
// A pump inside the pacing wait would otherwise quit between a tick's read and its update.
static int64_t s_quitRequestNs = 0;
constexpr int64_t kQuitDeferMaxNs = 2000000000; // a pass that never ends (a hang) still lets the window close

static void pumpEvents()
{
	if (mppm::probe::g_on) {
		mppm::probe::pump_begin();
	}
	if (s_quitRequestNs != 0 && now_ns() - s_quitRequestNs > kQuitDeferMaxNs) {
		host_quit(0);
	}
	for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; event++) {
		if (event->type == AURORA_EXIT) {
			if (!p2_net_session_active()) {
				host_quit(0);
			}
			if (s_quitRequestNs == 0) {
				s_quitRequestNs = now_ns();
			}
			continue;
		}
		if (event->type == AURORA_SDL_EVENT) {
			if (mppm::probe::g_on) {
				mppm::probe::on_event(event->sdl);
			}
			mppm::launcher::handle_event(event->sdl);
		} else {
			mppm::launcher::handle_aurora_event(*event); // window resized, controllers, ...
		}
	}
	if (mppm::probe::g_on) {
		mppm::probe::pump_end();
	}
}

static void beginHostFrame()
{
	pumpEvents();
	while (!(s_frameOpen = aurora_begin_frame())) {
		// Minimized or paused: wait until the window can present again, without spinning.
		SDL_Delay(5);
		pumpEvents();
	}
}

namespace {

// Late sampling settings: MPPM_LATE_SAMPLE=0 turns it off; MPPM_LATE_SAMPLE_US is the margin left
// between the pad read and the tick's retrace (default 1000 us). Returns the margin, 0 when off.
int64_t late_sample_margin_us()
{
	static const int64_t s_us = [] {
		const char* off = std::getenv("MPPM_LATE_SAMPLE");
		if (off != nullptr && std::strcmp(off, "0") == 0) {
			return int64_t{0};
		}
		const char* us = std::getenv("MPPM_LATE_SAMPLE_US");
		const long v   = us != nullptr && *us != '\0' ? std::strtol(us, nullptr, 10) : 1000L;
		return static_cast<int64_t>(std::clamp(v, 100L, 30000L));
	}();
	return s_us;
}

// The next retrace message of an armed wait: the one the pre-read wait already took off the queue
// (a due tick found while it polled), else the queue's.
bool receive_tick_msg(OSMessageQueue* q, OSMessage* msg, s32 flags)
{
	if (s_heldDue) {
		s_heldDue = false;
		*msg      = reinterpret_cast<OSMessage>(static_cast<uintptr_t>(s_heldCount));
		return true;
	}
	return OSReceiveMessage(q, msg, flags);
}

// M5.3b barrier alignment: after p2_host_align_tick(C) the next armed tick accepts no retrace below C
// (the one-slot queue may hold a stale count from a hold, which would make the tick due at once).
bool below_align(uint32_t count)
{
	return s_alignPending && static_cast<int32_t>(count - s_alignCount) < 0;
}

// M5.3b online: the next armed tick's retrace on a fixed grid. Offline (the original rule) a tick
// accepted late moves every later tick: next = accepted + time. Online every peer keeps the grid of
// the last barrier release (next = previous + time) and catches up after a long tick, one tick per
// retrace, so a long tick on one peer leaves no lasting phase difference that the slew (1 ms per
// retrace) would take seconds to remove while the others stall. Rebased after a release (alignment)
// and when kCatchUpTicks or more behind: the stage load's long tick (Battle and Challenge load
// inside sim ticks, about 0.9 s) would otherwise run some 30 ticks at double speed on every peer,
// each waiting for the slowest, which stalls longer than a rebase and the slew.
// p2_host_tick_shift (the client's phase controller, net thread) moves the grid by whole ticks: earlier
// is caught up the same way (allowed beyond kCatchUpTicks), later is one longer wait.
constexpr uint32_t kCatchUpTicks = 4;
std::atomic<int> s_tickShift{0};
int s_catchUpAllow = 0;
uint32_t next_tick_count(uint32_t accepted, uint32_t prevNext, uint32_t time)
{
	const int shift = s_tickShift.exchange(0, std::memory_order_relaxed);
	if (!(p2_net_online() && p2_net_session_active()) || s_alignPending || time == 0) {
		s_catchUpAllow = 0;
		return accepted + time;
	}
	uint32_t next = prevNext + time;
	if (shift > 0) {
		next -= static_cast<uint32_t>(shift) * time;
		s_catchUpAllow = shift;
	} else if (shift < 0) {
		next += static_cast<uint32_t>(-shift) * time;
	}
	const int32_t behind = static_cast<int32_t>(accepted + time - next); // > 0: the next tick is due already
	if (behind >= static_cast<int32_t>((kCatchUpTicks + s_catchUpAllow) * time)) {
		s_catchUpAllow = 0;
		return accepted + time;
	}
	if (behind <= 0) {
		s_catchUpAllow = 0;
	}
	return next;
}

// One step of the HostFrames loop (fps_design 5.5, 7): one in-between picture (Smooth Motion) or
// one preserved UI frame when one is due, else a short sleep. `tickDueNs` is the tick's retrace: a
// frame is drawn only if it fits before it. `sleepEndNs` (default `tickDueNs`) caps every sleep: the
// late-sampling wait passes its pre-read deadline, so the pad read is not slept past, while a frame
// may still be drawn in the same window as in the tick wait (one ending inside the last
// millisecond would otherwise be lost, M5.1b fix round 1). Break: no frame can be drawn this wait
// (let the tick draw).
enum class Step { Continue, Break };
Step host_frame_step(uint32_t time, int64_t tickDueNs, bool& smoothLoop, int64_t sleepEndNs = -1)
{
	const int64_t now               = now_ns();
	const Policy& p                 = refresh_policy(now);
	const bool smooth               = smooth_wanted(p, time) && p2_game_present_ready();
	if (!smooth && !host_frames_wanted(p, time)) {
		return Step::Break; // Smooth Motion not ready yet (history, guards) and no menu open
	}
	const int64_t interval          = smooth ? p.presentNs : std::max(p.presentNs, p.uiSleepNs);
	const Clock::time_point sleepEnd = at_ns(sleepEndNs >= 0 && sleepEndNs < tickDueNs ? sleepEndNs : tickDueNs);
	s_uiInterval                    = interval;
	// No frame just before the tick: the tick's own picture takes that slot. An in-between pass
	// must also fit before the tick is due, so the tick is not delayed by it.
	const int64_t margin = smooth ? std::max(interval / 2, s_passNs + 1000000) : interval / 2;
	if (smooth && tickDueNs - now <= margin && !smoothLoop) {
		s_passNs -= s_passNs / 16; // no room this tick: let the estimate recover
	}
	// A present still in flight would make the next frame wait for a frame slot.
	if (smooth && aurora_presents_in_flight() > 0) {
		p2_host_block_begin();
		sleep_poll(std::min(Clock::now() + std::chrono::microseconds(500), sleepEnd));
		p2_host_block_end();
		return Step::Continue;
	}
	if (tickDueNs - now > margin && s_presentPacer.due(at_ns(now), std::chrono::nanoseconds(interval))) {
		if (smooth) {
			if (!s_frameOpen || !p2_game_present(smooth_alpha(now_ns(), time))) {
				return Step::Break; // let the tick draw
			}
			if (mppm::probe::g_on) {
				mppm::probe::draw_patch_between(); // an in-between can be the first picture of a press
			}
		} else if (!s_frameOpen || !aurora_preserve_frame()) {
			return Step::Break; // the picture is gone: let the tick draw a new one
		} else if (mppm::probe::g_on) {
			mppm::probe::preserved_frame(); // the tick's picture again, with its patch if it had one
		}
		p2_host_block_begin();
		aurora_end_frame();
		s_frameOpen = false;
		if (mppm::probe::g_on) {
			mppm::probe::frame_ended(true);
		}
		beginHostFrame();
		p2_wide_update();
		const int64_t done = now_ns();
		if (smooth) {
			// One slow pass (a stall, the purity check) must not stop the passes for good.
			const int64_t took = std::min<int64_t>(done - now, 20000000);
			s_passNs = s_passNs == 0 ? took : s_passNs + (took - s_passNs) / 8;
		}
		sleep_poll(std::min(s_presentPacer.next(), sleepEnd));
		p2_host_block_end();
		if (smooth) {
			smoothLoop = true;
			count_interp(now_ns());
		} else {
			count_ui(now_ns());
		}
		mppm::launcher::update();
	} else {
		p2_host_block_begin();
		sleep_poll(std::min(s_presentPacer.next(), sleepEnd));
		p2_host_block_end();
	}
	return Step::Continue;
}

// Late sampling (plan 2.10 item 1, M5.1b design 4.11): the end of an armed pass waits here, not
// at the start of the next one, until 1 ms before the next tick's retrace, so the next pass's pad
// read sees fresh input. The wait does what the tick wait would have done in that time (Smooth
// Motion in-between pictures, UI frames over the running game), or sleeps; a frame may be drawn
// whenever it fits before the tick's retrace, as in the tick wait, so the pad read follows the end
// of a late frame and Smooth Motion keeps main's picture count. Then it pumps SDL's
// event queue only (keyboard and gamepad state, read by PADRead): the events themselves are
// dispatched by the next pumpEvents, as before. A due retrace found while polling is kept for the
// tick wait (the queue holds one message, so a stale one is never left in it).
// Takes the queued retrace messages: older ones are dropped (the slot stays free for the due one), a
// due one is kept for the tick wait. True when the tick is due.
bool drain_tick_queue()
{
	if (s_heldDue) {
		return true;
	}
	OSMessage msg;
	while (s_tickQueue != nullptr && OSReceiveMessage(s_tickQueue, &msg, OS_MESSAGE_NOBLOCK)) {
		const uint32_t count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(msg));
		if (static_cast<int32_t>(count - s_nextCount) >= 0) {
			s_heldDue   = true; // late already: the tick wait takes it at once
			s_heldCount = count;
			return true;
		}
	}
	return false;
}

int online_tick_ready(void* arg)
{
	return p2_net_tick_ready(*static_cast<uint32_t*>(arg)) || mppm::session::over(*static_cast<uint32_t*>(arg));
}

void pre_read_wait(uint32_t time)
{
	// M5.3b (D14): online, the wait also needs the next tick ready, and it runs (readiness only) when
	// late sampling is off.
	const bool online      = p2_net_online() && p2_net_session_active();
	const int64_t marginNs = late_sample_margin_us() * 1000;
	const int64_t retrace  = p2_vi_retrace_time_ns(s_nextCount);
	const int64_t deadline = marginNs > 0 ? retrace - marginNs : now_ns();
	bool smoothLoop        = false;
	bool frames            = marginNs > 0; // HostFrames steps may still draw in this wait
	for (;;) {
		const bool due = drain_tick_queue();
		if (due || now_ns() >= deadline) {
			break;
		}
		if (frames) {
			const Policy& p = refresh_policy(now_ns());
			if ((smooth_wanted(p, time) || host_frames_wanted(p, time)) && host_frame_step(time, retrace, smoothLoop, deadline) == Step::Continue) {
				continue;
			}
			frames = false;
		}
		p2_host_block_begin();
		FramePacer::sleep_until(at_ns(deadline));
		p2_host_block_end();
	}
	if (online) {
		uint32_t next = p2_net_tick() + 1;
		// Ready by the tick's retrace: poll every 0.5 ms (and keep the retrace queue's slot free).
		while (!p2_net_tick_ready(next) && now_ns() < retrace && !drain_tick_queue()) {
			p2_host_block_begin();
			SDL_DelayNS(500000);
			p2_host_block_end();
		}
		if (!p2_net_tick_ready(next) && !mppm::session::over(next)) {
			// Late: the hold loop (UI frames over the last picture, the wait overlay).
			const int64_t since = now_ns();
			mppm::session::wait_state(true, next, since, false);
			p2_host_net_hold(online_tick_ready, &next);
			mppm::session::wait_state(false, next, since, false);
			if (p2_net_tick_ready(next)) {
				mppm::session::note_prewait(next, now_ns() - std::max(since, retrace));
			}
		}
	}
	s_preSmooth = smoothLoop;
	if (mppm::probe::g_on) {
		mppm::probe::pump_begin();
	}
	SDL_PumpEvents();
	if (mppm::probe::g_on) {
		mppm::probe::pump_end();
		mppm::probe::peek_presses();
	}
}

} // namespace

// While the menu holds the game, the sound system's frame work runs at the game's 30 Hz so the
// menu's cues start (p2_game_audio_hold_frame); the game's own sounds stay ducked.
static u64 s_lastSoundFrame = 0;
static void holdAudioFrame()
{
	const u64 now = SDL_GetTicks();
	if (now - s_lastSoundFrame >= 33) {
		s_lastSoundFrame = now;
		p2_game_audio_hold_frame();
	}
}

// Test aid MPPM_HOLD_AT_TICK=<n> (harness sessions): the picture of the pass that consumed tick n
// is presented and stays on screen for 3 s, so a screenshot shows exactly that tick. With
// --headless-ticks the passes of the 30 ticks before it are presented too: the game's picture uses
// copies of earlier frames, which a run that presents nothing does not make.
static bool hold_at_tick_show()
{
	static const long s_holdTick = [] {
		const char* v = std::getenv("MPPM_HOLD_AT_TICK");
		return v != nullptr && *v != '\0' ? std::strtol(v, nullptr, 10) : 0L;
	}();
	const long tick = static_cast<long>(p2_net_tick());
	return s_holdTick > 0 && p2_net_session_active() && tick >= s_holdTick - 30 && tick < s_holdTick;
}

static bool hold_at_tick_due()
{
	static const long s_holdTick = [] {
		const char* v = std::getenv("MPPM_HOLD_AT_TICK");
		return v != nullptr && *v != '\0' ? std::strtol(v, nullptr, 10) : 0L;
	}();
	static bool s_done = false;
	if (s_holdTick <= 0 || s_done || !p2_net_session_active() || static_cast<long>(p2_net_tick()) != s_holdTick) {
		return false;
	}
	s_done = true;
	return true;
}

static void hold_at_tick()
{
	std::fprintf(stderr, "mppm: net: holding tick %u\n", p2_net_tick());
	const uint64_t start = SDL_GetTicksNS();
	while (aurora_presents_in_flight() > 0 && SDL_GetTicksNS() - start < 100000000) {
		SDL_DelayNS(200000);
	}
	SDL_Delay(3000); // inside the block region: the other game threads may run
}

// Called by JFWDisplay::endFrame on the main game thread once the game has submitted a frame.
// Update-first (M5.1b) runs the tick's update inside JFWDisplay::beginRender, before the frame is
// drawn. A frame loop that the update starts (waitSyncLoad from the story file select, a section
// setup) begins with endFrame(), which would present the outer pass's frame with nothing drawn in
// it yet. port/game/net/tick.cpp detects that and asks for that one present to be dropped: the
// GX work stays in the open host frame and the nested loop's first real frame presents it.
static bool s_skipNextPresent = false;
extern "C" void p2_host_skip_next_present(void) { s_skipNextPresent = true; }

extern "C" void p2_host_present()
{
	if (s_skipNextPresent) {
		s_skipNextPresent = false;
		return;
	}
	s_lastSoundFrame = SDL_GetTicks(); // the game ran its sound frame work this frame
	int64_t now            = now_ns();
	const Policy& policy   = refresh_policy(now);
	const bool freeRun     = s_freeRunTick;
	const bool armed       = s_armed;
	// Headless tick runner (M5.1a): every tick is due at once and nothing is presented, except
	// every Nth tick with the debug aid MPPM_HEADLESS_SHOW_EVERY=N (a screenshot shows progress).
	const bool headless    = p2_net_headless() != 0;
	if (mppm::probe::g_on) {
		mppm::probe::pass_end(); // this pass's pad read and update are done
	}
	// Read before blocking: VIGetRetraceCount is a safe point.
	const int64_t limit    = p2_vi_retrace_time_ns(VIGetRetraceCount() + 1) - 2000000;
	// Smooth Motion: capture the state this tick left (S(N), C(N)) for the in-between pictures
	// drawn while waiting for the next tick. Frames outside Section::main's ticks drop the history.
	// A pass that ran update before draw (p2_host_update_first) has no in-between pictures.
	const bool smoothOn = s_armed && smooth_wanted(policy, s_tickTime) && p2_host_update_first() == 0;
	p2_game_present_enable(smoothOn ? 1 : 0);
	if (smoothOn) {
		p2_game_present_tick_end();
	}
	if (mppm::probe::g_on && s_frameOpen) {
		mppm::probe::draw_patch(); // the first picture drawn after the update that used a press
	}
	// Other game threads may run while this thread waits on the GPU (port/sdk/os_thread.cpp).
	p2_host_block_begin();
	if (s_frameOpen) {
		// Test hook: MPPM_SMOOTH_ONLY=1 presents only the in-between pictures while Smooth Motion
		// runs, so a screenshot always shows one (with MPPM_SMOOTH_ALPHA, a known in-between).
		static const bool s_smoothOnly = std::getenv("MPPM_SMOOTH_ONLY") != nullptr;
		bool show;
		if (headless) {
			static const long s_showEvery = [] {
				const char* v = std::getenv("MPPM_HEADLESS_SHOW_EVERY");
				return v != nullptr ? std::max(0L, std::strtol(v, nullptr, 10)) : 0L;
			}();
			static long s_headlessPasses = 0;
			show = s_showEvery > 0 && ++s_headlessPasses % s_showEvery == 0 && aurora_presents_in_flight() == 0;
		} else {
			show = (freeRun || present_due(policy, now, limit)) && !(s_smoothOnly && smoothOn && p2_game_present_ready());
		}
		const bool holdTick = armed && hold_at_tick_due();
		if (holdTick || (headless && armed && hold_at_tick_show() && aurora_presents_in_flight() == 0)) {
			show = true;
		}
		if (show) {
			aurora_end_frame();
		} else {
			aurora_end_frame_no_present();
		}
		s_frameOpen = false;
		if (mppm::probe::g_on) {
			mppm::probe::frame_ended(show); // a sample ends when its picture has been presented
		}
		count_tick(show, now_ns());
		if (holdTick) {
			hold_at_tick();
		}
	}
	beginHostFrame();
	p2_wide_update(); // the content size is stable for the whole game frame from here
	if (headless) {
		if (!armed) {
			// Unarmed waits (loads, fades) no longer block on retraces: give the lower-priority
			// game threads (DVD, memory card), which only run while this thread is blocked, a turn.
			SDL_DelayNS(1000000);
		}
	} else if (freeRun) {
		s_sleepPacer.wait(std::chrono::nanoseconds(policy.sleepNs)); // the scene's frame slot
	}
	p2_host_block_end();
	s_armed       = false;
	s_updateFirst = -1;
	s_freeRunTick = false;
	s_soundFrames = 1;
	s_mode        = freeRun ? "freerun" : (s_smoothTick ? "smooth" : (s_uiTick ? "ui" : "game"));
	s_uiTick      = false;
	s_smoothTick  = false;
	// M5.3b: START, END and notices of an online session, on the game thread (no-op offline).
	p2_net_game_poll();
	// UI work runs on the game thread with no other game thread active (settings callbacks may
	// touch game state); it is drawn by the next aurora_end_frame.
	mppm::launcher::update();
	if (s_quitRequestNs != 0 || p2_net_should_exit()) {
		host_quit(0); // host_quit reads the exit code after p2_net_shutdown
	}
	// Pause While Menu Is Open: keep presenting UI frames without returning to the game. These
	// frames have no game drawing, so Aurora keeps the last game picture under the UI.
	static int s_redrawn = 0; // times in a row the picture was lost
	static int s_redrawFrames = 0;
	while (mppm::launcher::hold_game()) {
		// A video change (aspect, internal resolution, window size, vsync) recreated the frame
		// buffer, so the held picture is gone: let the game draw two frames into it (the first
		// one after a resize has no EFB copies of the previous frame yet), already at the new
		// shape. Its input stays blocked and its sound stays ducked; the hold resumes after.
		if (s_redrawFrames > 0) {
			s_redrawFrames--;
			return;
		}
		if (!aurora_preserve_frame() && s_redrawn < 3) {
			s_redrawn++;
			s_redrawFrames = 1;
			return;
		}
		s_redrawn = 0;
		s_mode    = "hold";
		s_winHeld = true;
		holdAudioFrame();
		const Policy& holdPolicy = refresh_policy(now_ns());
		p2_host_block_begin();
		aurora_end_frame();
		s_frameOpen = false;
		beginHostFrame();
		p2_wide_update();
		s_uiPacer.wait(std::chrono::nanoseconds(holdPolicy.uiSleepNs));
		p2_host_block_end();
		count_ui(now_ns());
		mppm::launcher::update();
	}
	s_redrawFrames = 0;
	// Late sampling: wait for the next tick here, then sample the input. Not in FreeRun (its frame
	// slot sleep paces it), not headless (nothing waits), not after an unarmed pass (loads, fades).
	// Online (M5.3b) the wait also runs with late sampling off: it waits for the next tick's bundle.
	if (armed && !freeRun && !headless && s_nextCountValid && (late_sample_margin_us() > 0 || (p2_net_online() && p2_net_session_active()))) {
		s_nextCountValid = false;
		pre_read_wait(s_tickTime);
	}
}

// ---- Frame pacing contract (p2_host.h) ----
// Edit G1: called from waitForTick before the blocking retrace wait. Unarmed waits (loading loops,
// fades, waitBlanking) keep the original code. An armed tick is made due here by one of the modes
// of fps_design 5.5; Game mode runs the original rule on the same one-slot queue itself, so the
// time the tick became due is known for the `work` counter.
extern "C" int p2_host_tick_wait(void* queue, uint32_t* nextCount, uint32_t time)
{
	if (p2_net_headless()) {
		// Headless tick runner (M5.1a): every wait, armed or not, returns at once. The queued
		// retraces are dropped; VIGetRetraceCount is a safe point, so queued retraces and alarms
		// still run. The tick keeps its fixed delta time (p2_host_tick_dt stays 0).
		OSMessage m;
		while (OSReceiveMessage(static_cast<OSMessageQueue*>(queue), &m, OS_MESSAGE_NOBLOCK)) { }
		*nextCount = VIGetRetraceCount();
		s_heldDue = s_preSmooth = s_nextCountValid = false;
		if (s_armed) {
			s_freeRunTick = s_freeRunNow = s_freeRunReq = false;
			s_soundFrames = 1;
			s_uiTick = s_smoothTick = false;
			s_tickTime = time;
			s_dueCount = *nextCount;
			s_lastTickStart = s_tickStart = now_ns();
		}
		return 1;
	}
	s_tickQueue      = static_cast<OSMessageQueue*>(queue);
	s_nextCountValid = false;
	if (!s_armed) {
		s_preSmooth = false;
		s_dueKnown  = false; // a load or fade: the next armed tick's retrace is not known before its wait
		if (s_heldDue) {
			// The late-sampling wait took the due retrace, and the pass after it is unarmed (the
			// section ended): this wait is over, by the original rule.
			s_heldDue = false;
			if (static_cast<int32_t>(s_heldCount - *nextCount) >= 0) {
				*nextCount = s_heldCount + time;
				return 1;
			}
		}
		return 0;
	}
	OSMessageQueue* q      = static_cast<OSMessageQueue*>(queue);
	const Policy& policy   = refresh_policy(now_ns());
	s_freeRunTick          = false;
	s_soundFrames          = 1;
	s_uiTick               = false;
	s_smoothTick           = false;
	s_tickTime             = time;
	// A FreeRun request lasts one tick: the scene renews it every frame, so no later section can
	// inherit it and run fast.
	s_freeRunNow           = s_freeRunReq;
	s_freeRunReq           = false;
	if (free_run_in_effect(policy)) {
		// FreeRun: one tick per presented frame; the frame slot sleep at tick end paces it.
		OSMessage msg;
		while (OSReceiveMessage(q, &msg, OS_MESSAGE_NOBLOCK)) { }
		s_heldDue = s_preSmooth = false;
		*nextCount         = VIGetRetraceCount();
		const int64_t now  = now_ns();
		const int64_t prev = s_lastTickStart != 0 ? s_lastTickStart : now - kRetraceNs;
		s_tickDt           = std::clamp(static_cast<float>((now - prev) / 1e9), 0.001f, 0.050f);
		s_freeRunTick      = true;
		s_dueCount         = *nextCount;
		// The sound system's frame work stays at 59.94 Hz.
		s_soundAcc += s_tickDt * (60000.0 / 1001.0);
		s_soundFrames = std::min(3, static_cast<int>(s_soundAcc));
		s_soundAcc    = std::min(s_soundAcc - s_soundFrames, 3.0);
		s_lastTickStart = s_tickStart = now;
		return 1;
	}
	s_soundAcc = 0.0;
	// HostFrames: in-between pictures of the game (Smooth Motion) or UI-only frames while waiting
	// for the tick (the original rule on the same queue).
	// The late-sampling wait at the end of the previous pass may have shown in-between pictures.
	bool smoothLoop = s_preSmooth; // this wait has shown an in-between picture
	s_preSmooth     = false;
	while (smooth_wanted(policy, time) || host_frames_wanted(policy, time)) {
		OSMessage msg;
		if (receive_tick_msg(q, &msg, OS_MESSAGE_NOBLOCK)) {
			const uint32_t count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(msg));
			if (static_cast<int32_t>(count - *nextCount) >= 0 && !below_align(count)) {
				*nextCount   = next_tick_count(count, *nextCount, time);
				s_nextCount  = *nextCount;
				s_nextCountValid = true;
				s_dueKnown       = true;
				s_alignPending   = false;
				s_dueCount   = count;
				s_uiTick     = true; // the tick's own picture always takes a slot (present_due)
				s_smoothTick = smoothLoop;
				s_lastTickStart = s_tickStart = now_ns();
				return 1;
			}
			continue; // stale retrace: the tick is not due yet
		}
		if (host_frame_step(time, p2_vi_retrace_time_ns(*nextCount), smoothLoop) == Step::Break) {
			break;
		}
	}
	// Game mode: the original rule, as in waitForTick (M5.3b: plus the barrier alignment, online only).
	void* msg;
	do {
		if (!receive_tick_msg(q, &msg, OS_MESSAGE_BLOCK)) {
			msg = 0;
		}
	} while (((intptr_t)msg - (int)*nextCount) < 0 || below_align(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(msg))));
	*nextCount = next_tick_count((uint32_t)(intptr_t)msg, *nextCount, time);
	s_nextCount      = *nextCount;
	s_nextCountValid = true;
	s_dueKnown       = true;
	s_alignPending   = false;
	s_dueCount = (uint32_t)(intptr_t)msg;
	s_smoothTick = smoothLoop;
	if (smoothLoop) {
		s_uiTick = true; // keep the in-between schedule: the tick's own picture takes a slot
	}
	s_lastTickStart = s_tickStart = now_ns();
	return 1;
}

// M5.1b tick order (p2_net.h): latched for the armed pass on its first call.
extern "C" int p2_host_update_first(void)
{
	if (!s_armed) {
		return 0;
	}
	if (s_updateFirst < 0) {
		s_updateFirst = p2_net_tick_order() == P2_TICK_ORDER_UPDATE_FIRST && !smooth_wanted(refresh_policy(now_ns()), s_tickTime) ? 1 : 0;
	}
	return s_updateFirst;
}

// Edit G2: Section::main arms the hook for its tick; p2_host_present clears it.
extern "C" void p2_host_arm_tick(void)
{
	s_armed = true;
}

extern "C" void p2_host_free_run(int on)
{
	s_freeRunReq = on != 0;
}

extern "C" float p2_host_tick_dt(void)
{
	return s_freeRunTick ? s_tickDt : 0.0f;
}

extern "C" int p2_host_sound_frames(void)
{
	return s_soundFrames;
}

extern "C" float p2_host_picture_fps(void)
{
	return s_pictureFps >= 0.0f ? s_pictureFps : aurora_get_fps();
}

// ---- M5.3b online (p2_net.h) ----

extern "C" long long p2_host_late_sample_margin_ns(void) { return late_sample_margin_us() * 1000; }

extern "C" long long p2_host_next_tick_due_ns(void)
{
	if (s_alignPending) {
		return p2_vi_retrace_time_ns(s_alignCount);
	}
	return s_dueKnown ? p2_vi_retrace_time_ns(s_nextCount) : 0;
}

extern "C" void p2_host_tick_shift(int ticks)
{
	s_tickShift.fetch_add(ticks, std::memory_order_relaxed);
}

extern "C" void p2_host_align_tick(uint32_t count)
{
	s_alignCount   = count;
	s_alignPending = true;
	// Free the retrace queue's one slot: a stale count from the hold would keep retrace `count` from
	// being posted, and the tick would start one retrace late.
	OSMessage msg;
	while (s_tickQueue != nullptr && OSReceiveMessage(s_tickQueue, &msg, OS_MESSAGE_NOBLOCK)) { }
	s_heldDue = false;
}

// The hold loop of an online session (D14): the game waits for a bundle or a barrier release. UI frames
// over the last picture at the UI rate (the wait overlay animates and takes input), events, the
// launcher; ready(arg) is polled every 0.5 ms in between. Returns 1 once ready, 0 when the window asked
// to quit. Runs between passes (inside JUTGamePad::read or at the end of p2_host_present), with the
// host frame open and nothing drawn into it, as the menu hold loop.
extern "C" int p2_host_net_hold(int (*ready)(void*), void* arg)
{
	if (ready(arg)) {
		return 1;
	}
	if (p2_net_headless()) { // never online (the flags conflict), but safe
		while (!ready(arg)) {
			pumpEvents();
			SDL_Delay(16);
		}
		return 1;
	}
	int64_t lastUi = 0;
	for (;;) {
		p2_net_game_poll();
		if (ready(arg)) {
			return 1;
		}
		if (s_quitRequestNs != 0) {
			return 0;
		}
		const int64_t now = now_ns();
		const Policy& p   = refresh_policy(now);
		const int64_t ui  = std::max<int64_t>(p.uiSleepNs, 8000000);
		// No UI frame in the last 20 ms before a known tick time (a barrier release): the read must not
		// be late behind a present.
		const int64_t due = p2_host_next_tick_due_ns();
		if (now - lastUi >= ui && !(due > 0 && due - now < 20000000 && due - now > -5000000)) {
			lastUi = now;
			if (s_frameOpen && aurora_preserve_frame()) {
				p2_host_block_begin();
				aurora_end_frame();
				s_frameOpen = false;
				beginHostFrame();
				p2_wide_update();
				p2_host_block_end();
				count_ui(now_ns());
			} else {
				pumpEvents(); // the picture is gone: keep the open frame, still take events
			}
			mppm::launcher::update();
			continue;
		}
		p2_host_block_begin();
		SDL_DelayNS(500000);
		p2_host_block_end();
	}
}

// One frame of the launcher before any game exists (no disc yet).
static bool launcherFrame()
{
	const int64_t now    = now_ns();
	const Policy& policy = refresh_policy(now);
	s_mode               = "launcher";
	if (s_frameOpen) {
		aurora_end_frame();
		s_frameOpen = false;
		count_ui(now);
	}
	s_uiPacer.wait(std::chrono::nanoseconds(policy.uiSleepNs));
	beginHostFrame();
	mppm::session::game_poll(false); // M5.3b: an online START from the standalone start menu boots the game
	mppm::launcher::update();
	return true;
}


// The GameCube has 24 MiB of MEM1. Host objects are larger (64-bit pointers, bigger SDK structs),
// so the emulated arena is scaled up; the game sizes its heaps from the arena bounds.
static constexpr uint32_t kMem1Size = 256 * 1024 * 1024;

int main(int argc, char** argv)
{
	// Keep the default SIGINT/SIGTERM behaviour: SDL would turn them into a quit event that a stuck
	// game loop never reads.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	p2_net_init(argc, argv); // online harness flags and environment (M5.1a); no-op without them
	p2_mods_init(argc, argv); // data-mod flags and environment (M5.2); no file access
	mppm::probe::init(late_sample_margin_us());

	// The launcher fills the rest of the config from the settings (window, backend, MSAA, vsync,
	// software adapter...); see launcher::preinit.
	AuroraConfig config{};
	config.appName  = "mppm";
	config.mem1Size = kMem1Size;
	config.mem2Size = ARAM_DEFAULT_SIZE;
	mppm::launcher::preinit(config, argc, argv);
	const AuroraInfo info   = aurora_initialize(argc, argv, &config);
	s_window                = info.window;
	s_startVsync            = config.vsync;
	const bool haveLauncher = mppm::launcher::init(info);

	// Boot flow (docs/launcher/README.md 3): a disc from the command line / MPPM_DISC boots directly
	// with no menu (headless tests rely on this), as does Skip Start Menu; otherwise the start menu
	// runs, either on its own until Play (no disc yet, or Live Start Menu off) or over the booting
	// game with the live scene behind it.
	std::string discPath = mppm::launcher::startup_disc();
	mppm::launcher::BootMode mode = mppm::launcher::boot_mode();
	if (mode == mppm::launcher::BootMode::StandaloneMenu) {
		const auto result = mppm::launcher::run_standalone_menu(launcherFrame);
		if (result.disc.empty()) { // quit from the menu
			mppm::launcher::shutdown();
			aurora_shutdown();
			return 0;
		}
		discPath = result.disc;
		mode     = result.keepMenu ? mppm::launcher::BootMode::LiveMenu : mppm::launcher::BootMode::Direct;
	}
	if (discPath.empty()) {
		std::fprintf(stderr, "usage: %s <Pikmin 2 (USA) disc image: .iso/.gcm/.rvz>\n", argv[0]);
		mppm::launcher::shutdown();
		aurora_shutdown();
		return 1;
	}
	if (!aurora_dvd_open(discPath.c_str())) {
		std::fprintf(stderr, "mppm: could not open disc image '%s'\n", discPath.c_str());
		mppm::launcher::shutdown();
		aurora_shutdown();
		return 1;
	}
	// Data-mod overlay (M5.2): loose files and archive overrides of the enabled mods, before anything
	// reads the disc. Nothing happens without an enabled mod.
	p2_mods_mount();
	if (haveLauncher && mode == mppm::launcher::BootMode::LiveMenu) {
		mppm::launcher::show_prelaunch();
	}

	// PADInit is idempotent; calling it first lets port 1 have a keyboard before the game's first
	// pad read (the game's own PADInit call then does nothing).
	PADInit();
	mppm::launcher::apply_default_keyboard(0, false); // port 1 keys unless the player saved their own

	OSInit();
	p2_os_thread_init();
	if (s_frameOpen) { // left open by the launcher loop
		aurora_end_frame();
		s_frameOpen = false;
	}
	beginHostFrame();
	p2_wide_update();
	const int ret = p2_game_main();
	aurora_dvd_close();
	aurora_shutdown();
	return ret;
}
