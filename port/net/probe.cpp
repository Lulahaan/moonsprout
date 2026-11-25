// Software latency probe: see probe.h.
#include "probe.h"

#include <aurora/aurora.h>

#include <SDL3/SDL_timer.h>

#include "p2_net.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" unsigned p2_net_probe_pad_buttons(void); // port/game/net/probe_patch.cpp
extern "C" void p2_net_probe_draw_patch(void);      // port/game/net/probe_patch.cpp

namespace mppm::probe {

bool g_on = false;

namespace {

enum class State {
	Idle,    // no press being measured
	Pending, // a press is waiting for its pad edge
	Edge,    // this draw-first pass's read saw the press: the next pass draws the first picture of it
	Drawing, // the patch is drawn; the sample ends when a frame with it is presented
};

struct Pump {
	uint64_t start = 0;
	uint64_t end   = 0; // 0 while the pump runs
};
constexpr size_t kPumps = 8;

constexpr int kEdgePasses   = 10; // a press with no edge within this many passes is dropped
constexpr int kEdgePassesOnline = 16; // M5.3b: online the press reaches the game d ticks later (d <= 8)
constexpr int kHoldFrames   = 3;  // presented frames that show the patch per sample (photodiode rig)
constexpr uint64_t kPollNs  = 100000;
constexpr uint64_t kWaitNs  = 50000000;

int s_holdFrames    = kHoldFrames; // MPPM_PROBE_HOLD_FRAMES=N overrides (a screenshot aid)
State s_state       = State::Idle;
uint64_t s_pressNs  = 0;
uint64_t s_readNs   = 0;
int s_passes        = 0;
unsigned s_prev     = 0;
int s_holdLeft      = 0; // presented frames still to show the patch after a sample
bool s_patchDrawn   = false;
bool s_between      = false; // the frame being ended is a Smooth Motion in-between picture
bool s_efbPatch     = false; // the last ended frame had the patch (a preserved frame shows it again)
std::vector<double> s_samples;
size_t s_summarized = 0;
Pump s_pumps[kPumps];     // the last pumps, s_pumps[s_pumpHead] the newest
size_t s_pumpHead     = 0;
size_t s_pumpCount    = 0;
bool s_estimated      = false; // the current press time is the pump-interval midpoint
uint64_t s_pumpSpanNs = 0;
size_t s_estimatedCount = 0;
size_t s_betweenCount   = 0; // samples ended by an in-between picture (Smooth Motion on)
size_t s_sameCount      = 0; // samples whose pad edge came in an update-first pass
bool s_sameRead         = false; // the current sample's edge pass ran update before draw
uint64_t s_peekedUpTo   = 0;     // newest press stamp already seen by peek_presses

} // namespace

void init(long long lateSampleUs)
{
	g_on = p2_net_latency_probe() != 0 && p2_net_headless() == 0;
	if (g_on) {
		if (const char* v = std::getenv("MPPM_PROBE_HOLD_FRAMES")) {
			s_holdFrames = std::max(1, std::atoi(v));
		}
		const char* order = p2_net_tick_order() == P2_TICK_ORDER_UPDATE_FIRST ? "update-first" : "draw-first";
		if (lateSampleUs > 0) {
			std::fprintf(stderr, "mppm: probe: on (sim channel, %s, late sample %lld us)\n", order, lateSampleUs);
		} else {
			std::fprintf(stderr, "mppm: probe: on (sim channel, %s, late sample off)\n", order);
		}
	}
}

void pump_begin()
{
	s_pumpHead               = (s_pumpHead + 1) % kPumps;
	s_pumps[s_pumpHead]      = Pump{SDL_GetTicksNS(), 0};
	s_pumpCount              = std::min(s_pumpCount + 1, kPumps);
}

void pump_end()
{
	s_pumps[s_pumpHead].end = SDL_GetTicksNS();
}

namespace {

void press_event(const SDL_Event& event, bool peeked)
{
	const bool press = (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
	if (!press) {
		return;
	}
	if (!peeked && event.common.timestamp != 0 && event.common.timestamp <= s_peekedUpTo) {
		return; // already seen before the pad read
	}
	if (peeked) {
		s_peekedUpTo = std::max(s_peekedUpTo, event.common.timestamp);
	}
	if (s_state != State::Idle) {
		return;
	}
	const uint64_t stamp = event.common.timestamp != 0 ? event.common.timestamp : SDL_GetTicksNS();
	// The pump whose interval holds the stamp, newest first (the event may be dispatched one pump
	// after the late-sampling SDL_PumpEvents stamped it).
	s_estimated = false;
	for (size_t i = 0; i + 1 < s_pumpCount; ++i) {
		const Pump& pump = s_pumps[(s_pumpHead + kPumps - i) % kPumps];
		const Pump& prev = s_pumps[(s_pumpHead + kPumps - i - 1) % kPumps];
		const uint64_t end = pump.end != 0 ? pump.end : UINT64_MAX;
		if (stamp >= pump.start && stamp <= end) {
			// Stamped at dispatch (X11): the press happened after the previous pump drained the queue.
			s_estimated  = true;
			s_pumpSpanNs = pump.start - prev.start;
			s_pressNs    = prev.start + s_pumpSpanNs / 2;
			break;
		}
	}
	if (!s_estimated) {
		s_pressNs = stamp;
	}
	s_passes  = 0;
	s_state   = State::Pending;
}

} // namespace

void on_event(const SDL_Event& event)
{
	press_event(event, false);
}

void peek_presses()
{
	// The pad read that follows uses these presses before pumpEvents dispatches them.
	SDL_Event events[32];
	for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_GAMEPAD_BUTTON_DOWN}) {
		const int n = SDL_PeepEvents(events, 32, SDL_PEEKEVENT, type, type);
		for (int i = 0; i < n; ++i) {
			press_event(events[i], true);
		}
	}
}

void pass_end()
{
	const unsigned buttons = p2_net_probe_pad_buttons();
	const unsigned rising  = buttons & ~s_prev;
	s_prev                 = buttons;
	if (s_state == State::Edge) {
		s_state = State::Drawing; // this pass drew after the update that used the press
	}
	if (s_state != State::Pending) {
		return;
	}
	if (rising != 0) {
		s_readNs   = SDL_GetTicksNS();
		// Update-first: this pass's picture is drawn after the update that used the press.
		s_sameRead = p2_net_pass_ran_update_first() != 0;
		s_state    = s_sameRead ? State::Drawing : State::Edge;
	} else if (++s_passes >= (p2_net_online() ? kEdgePassesOnline : kEdgePasses)) {
		s_state = State::Idle; // not bound to a button
	}
}

void draw_patch()
{
	s_between    = false;
	s_patchDrawn = s_state == State::Drawing || s_holdLeft > 0;
	if (s_patchDrawn) {
		p2_net_probe_draw_patch();
	}
}

void draw_patch_between()
{
	// The pass that read the press has captured its state (p2_game_present_tick_end), so an
	// in-between picture drawn after it already moves toward the press's effect.
	s_between    = true;
	s_patchDrawn = s_state == State::Edge || s_state == State::Drawing || s_holdLeft > 0;
	if (s_patchDrawn) {
		p2_net_probe_draw_patch();
	}
}

void preserved_frame()
{
	s_between    = false;
	s_patchDrawn = s_efbPatch;
}

void frame_ended(bool shown)
{
	const bool drawn = s_patchDrawn;
	const bool between = s_between;
	s_patchDrawn     = false;
	s_between        = false;
	s_efbPatch       = drawn;
	if (!shown || !drawn) {
		return; // a skipped present: the next pass draws the patch again
	}
	if (s_state != State::Drawing && s_state != State::Edge) {
		s_holdLeft = std::max(0, s_holdLeft - 1);
		return;
	}
	const uint64_t start = SDL_GetTicksNS();
	while (aurora_presents_in_flight() > 0 && SDL_GetTicksNS() - start < kWaitNs) {
		SDL_DelayNS(kPollNs);
	}
	const uint64_t presentNs = SDL_GetTicksNS();
	const double total       = (presentNs - s_pressNs) / 1e6;
	const double toRead      = (s_readNs - s_pressNs) / 1e6;
	const char* picture      = between ? "in-between" : (s_sameRead ? "same-pass tick" : "tick");
	if (between) {
		++s_betweenCount;
	}
	if (s_sameRead) {
		++s_sameCount;
	}
	if (s_estimated) {
		std::fprintf(stderr,
		             "mppm: probe: sim press->present %.2f ms (press->read %.2f, read->present %.2f, %s picture; press "
		             "estimated, pump interval %.2f ms)\n",
		             total, toRead, total - toRead, picture, s_pumpSpanNs / 1e6);
		++s_estimatedCount;
	} else {
		std::fprintf(stderr, "mppm: probe: sim press->present %.2f ms (press->read %.2f, read->present %.2f, %s picture)\n",
		             total, toRead, total - toRead, picture);
	}
	s_samples.push_back(total);
	s_state    = State::Idle;
	s_holdLeft = s_holdFrames - 1;
	if (s_samples.size() % 10 == 0) {
		summary();
	}
}

void summary()
{
	if (s_samples.empty() || s_summarized == s_samples.size()) {
		return;
	}
	s_summarized = s_samples.size();
	std::vector<double> v = s_samples;
	std::sort(v.begin(), v.end());
	const size_t n      = v.size();
	const double median = n % 2 != 0 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
	std::fprintf(stderr,
	             "mppm: probe: n=%zu median %.2f ms min %.2f max %.2f (estimated press times: %zu, ended by an in-between "
	             "picture: %zu, update-first: %zu)\n",
	             n, median, v.front(), v.back(), s_estimatedCount, s_betweenCount, s_sameCount);
}

} // namespace mppm::probe
