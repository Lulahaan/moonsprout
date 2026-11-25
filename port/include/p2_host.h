// Small host-only helpers for behaviour the GameCube CPU defines and x86/arm64 does not.
#pragma once

// Integer division by zero: PowerPC divw/divwu produce a value instead of trapping (Dolphin's
// model: 0, or -1 for a negative signed dividend). Used where the game can divide by zero.
template <typename A, typename B>
inline auto p2_div(A a, B b) -> decltype(a / b)
{
	using R = decltype(a / b);
	if (b == 0) {
		return (a < 0) ? R(-1) : R(0);
	}
	return a / b;
}

// Remainder with a zero divisor: MWCC computes a - (a / b) * b, and the quotient is 0 on the
// GameCube (see p2_div), so the result is the dividend.
template <typename A, typename B>
inline auto p2_mod(A a, B b) -> decltype(a % b)
{
	using R = decltype(a % b);
	if (b == 0) {
		return R(a);
	}
	return a % b;
}

// Settings mirrored for game code (gP2Config).
#include "p2_config.h"
// Widescreen state (k, 2D remap) for the game's viewport, camera and 2D edits (ASPECT.md 5).
#include "p2_wide.h"

// Whether the JAudio system runs (port/audio). Latches gP2Config.audioEnabled (audio.enabled,
// MPPM_AUDIO=0) on first use, which is why that setting needs a restart.
extern "C" int p2_audio_enabled(void);

// Settings read off the game thread live next to their reader, as atomics with setters (the
// launcher calls these when the setting changes; see SETTINGS.md 1.4).
// port/audio/audio.cpp (mixer and AI threads):
extern "C" void p2_audio_set_master_gain(float gain);           // 0..1, multiplied into the mix
extern "C" void p2_audio_set_duck(float gain);                  // 0..1, ramped: mute in background, pause in menu
// Pause While Menu Is Open: the game's sound is ducked by p2_audio_set_duck, but the voices of the
// game's system sound effects (the menu cues, p2_game_play_ui_sound) keep `uiGain` instead.
extern "C" void p2_audio_set_menu_hold(int held, float uiGain);
extern "C" void p2_audio_set_debug(int debug, int voiceLog);    // tools.audioDebug, tools.audioVoiceLog
extern "C" int p2_audio_debug(void);
extern "C" void p2_audio_set_dump_path(const char* path);       // tools.audioDumpPath; before the first AI DMA
// port/sdk/os_thread.cpp (timer thread):
extern "C" void p2_sched_set_debug(int debug);                  // tools.schedDebug

// ---- Launcher <-> game: options owned by the launcher's config (SETTINGS.md 5.1) ----
// Host side, port/launcher/game_options.cpp (package W10). Called from the game's own option
// code once the sound system exists, and when its Options screen commits.
extern "C" void p2_host_apply_game_options(void);
extern "C" void p2_host_game_options_changed(int soundMode, int musicVol, int seVol, int rumble);
// Game side (package W10): applies the four options through the game's own setters.
extern "C" void p2_game_set_options(int soundMode, float bgm, float se, int rumble);

// ---- Launcher <- game: queries for the in-game menu (SETTINGS.md 13) ----
// Game side, port/game/menu_query.cpp. 0 while nothing unsaved can be lost (booting, the title
// screen, file select), so Start Menu and Quit skip their confirmation; 1 otherwise.
extern "C" int p2_game_progress_at_risk(void);
// Game side, port/game/menu_query.cpp: 1 while the game's own pause menu (any page) is on screen,
// so the bar's "Paused" tag stays out of its title (the menu already says the game is paused).
extern "C" int p2_game_pause_menu_open(void);

// ---- Launcher UI sounds (README 6: silent before a disc) ----
// Game side, port/game/ui_sound.cpp: plays the game's system sound effect for a launcher cue
// (mppm::ui::UiSound value). Returns 0, playing nothing, until the game's sound system exists.
// Call on the game thread (the launcher's update runs there once the game has booted).
extern "C" int p2_game_play_ui_sound(int cue);
// Game side, port/game/ui_sound.cpp. While the menu holds the game nothing runs the sound system's
// per-frame work, so a cue would never start: the host calls this at the game's frame rate
// instead (game thread). p2_game_ui_voice_mask (audio thread, from the mixer) returns the DSP
// voices (bit n = voice n) now playing a system sound effect.
extern "C" void p2_game_audio_hold_frame(void);
extern "C" unsigned long long p2_game_ui_voice_mask(void);

// ---- Soft reset (SETTINGS.md 13, Reset Game) ----
// Host side, port/launcher/integration.cpp: called by OSResetSystem at the end of the game's own
// reset sequence (the B+X+Start combo or the menu's Reset Game). Re-executes the program straight
// into the game (toMenu = 0: back to the title, as a GameCube reset reboots the disc) or into the
// start menu (toMenu = 1). Does not return.
extern "C" void p2_host_reset_system(int toMenu);
// Game side, port/game/reset.cpp: starts that reset sequence as if the reset combo were pressed
// on port 1. Call on the game thread.
extern "C" void p2_game_request_reset(void);

// ---- Live start-menu scene bridge (docs/launcher/SCENE.md 4.2) ----
// Host -> game. Implemented in port/launcher/scene_bridge.cpp.
extern "C" {
// 1 until the scene section has been created (one-shot, re-armable).
int p2_launcher_scene_pending(void);
typedef struct P2SceneConfig {
	int area;           // 0 tutorial, 1 forest, 2 yakushima, 3 last
	int timeOfDay;      // 0 morning, 1 day, 2 evening, 3 night
	int music;          // 0 off, 1 Piklopedia, 2 world map, 3 challenge menu, 4 high scores
	int takeoff;        // 0 off (fade only), 1 short, 2 full
	int reduceMotion;
	unsigned fadeColor; // 0xRRGGBB of the end fade: 0xFFF6E0 (launch flash), 0x050B1A for Off / reduce motion
	int live;           // 1 = the scene runs; 0 = parked: loaded but not drawn, no music (live_options 3.3)
} P2SceneConfig;
// The config handed to the scene. Area and live change only behind the backdrop (live_options 3.2).
void p2_launcher_scene_config(P2SceneConfig* out);
// Changes whenever the handed config changes; the scene re-reads the config when it differs.
unsigned p2_launcher_scene_config_gen(void);
enum { P2_SCENE_CMD_NONE, P2_SCENE_CMD_PLAY, P2_SCENE_CMD_SKIP };
int p2_launcher_scene_command(void);                              // polled once per frame; reading clears it
void p2_launcher_scene_pointer(float* x, float* y, int* active); // cursor in 0..1 of the window, for glances
int p2_launcher_scene_idle_seconds(void);                         // time since the last input the UI saw

// Game -> host.
enum { P2_SCENE_LOADING, P2_SCENE_READY, P2_SCENE_TAKEOFF, P2_SCENE_FADING, P2_SCENE_DONE, P2_SCENE_FAILED, P2_SCENE_PARKED };
void p2_launcher_scene_state(int state);
// From the doExit of a scene section that ends only to be created again (an area change): in place
// of P2_SCENE_DONE, re-arms p2_launcher_scene_pending() and reports Loading.
void p2_launcher_scene_reloading(void);
// Take-off cues for the UI choreography (DESIGN.md 7.1), each sent once. DESIGN's `handoff` cue is
// the state P2_SCENE_DONE.
enum {
	P2_SCENE_CUE_LAUNCH_BEGIN,
	P2_SCENE_CUE_HOP,
	P2_SCENE_CUE_BOARD,
	P2_SCENE_CUE_IGNITE,
	P2_SCENE_CUE_LIFTOFF,
	P2_SCENE_CUE_EXIT_FRAME,
};
void p2_launcher_scene_cue(int cue);
// Every frame from IGNITE on: joint body_s projected to 0..1 of the content rectangle (y down).
void p2_launcher_scene_ship_screen(float x, float y, int onScreen);

// Game side, implemented in port/game/scene/SceneSection.cpp (package W9).
struct ISection* p2_launcher_scene_create(struct JKRHeap* heap);
}

// Busy-wait loops (`while (flag) { }`) rely on the GameCube preempting the spinning thread; the
// host scheduler is cooperative, so such loops call this to let other game threads run.
extern "C" void p2_spin_yield(void);

// Zero on alloc (determinism checklist item 13, docs/online/M5_1B_BUILD.md): JKRHeap::alloc passes
// every block it hands out through this, so memory from the game heaps reads as zero whatever the
// heap held before (same contents on every peer and in every replay). Clears `size` bytes when
// `memory` is not null and returns `memory`. Implemented in port/sdk/heap_zero.cpp; a build with
// MPPM_ZERO_INIT=OFF returns the block untouched.
extern "C" void* p2_heap_zero(void* memory, unsigned long size);
// The next p2_heap_zero on this thread clears at most `bytes` (then the limit resets). JKRExpHeap
// and JKRSolidHeap::create set it to their header size before allocating the block a new heap is
// built in: the rest of that block is only handed out by the new heap's own alloc, which zeroes
// each block itself, so clearing it all (whole free arenas, on every section change) was wasted.
extern "C" void p2_heap_zero_limit(unsigned long bytes);

// ---- Frame pacing (docs: fps_design phase 1; host side port/main.cpp, game side
// port/game/present/Present.cpp). Integer types are spelled out so host and game agree. ----
#include <stdint.h>
// Edit G1 (JFWDisplay.cpp waitForTick): called before the blocking retrace wait. Returns 1 when
// the host has already made the tick due (FreeRun / HostFrames modes, setting *nextCount itself),
// 0 to take the original blocking path. Acts only when armed for this iteration of Section::main.
extern "C" int p2_host_tick_wait(void* queue, uint32_t* nextCount, uint32_t time);
// Edit G2 (section.cpp Section::main): arms p2_host_tick_wait for this tick; p2_host_present
// clears the arm. Loading loops, fades and waitBlanking never arm it.
extern "C" void p2_host_arm_tick(void);
// M5.1b: drops the next p2_host_present (a frame loop started inside an update-first update).
extern "C" void p2_host_skip_next_present(void);
// The start-menu scene asks for one tick per presented frame (1) or the original retrace pacing
// (0). Takes effect only while video.frameRate is not "original" and the game is not held.
extern "C" void p2_host_free_run(int on);
// Seconds since the previous tick while FreeRun is in effect (clamped to 1..50 ms by the host);
// 0 when it is not, so the caller falls back to its own clock.
extern "C" float p2_host_tick_dt(void);
// How many times the sound system's per-frame work should run this tick: 1 outside FreeRun,
// otherwise 0..3 from a 60 Hz accumulator (edit G3 calls p2_game_sound_main_loop).
extern "C" int p2_host_sound_frames(void);
// New pictures per second (shown ticks plus shown in-between passes), for the overlay.
extern "C" float p2_host_picture_fps(void);

// Game side, port/game/present/Present.cpp.
// Game seconds one tick advanced (sys->mDeltaTime); the host sums these for its speed check.
extern "C" float p2_game_tick_seconds(void);
// Edit G3 (system.cpp System::endRender): runs PSSystem::SysIF::mainLoop p2_host_sound_frames()
// times. `sysif` is a PSSystem::SysIF*.
extern "C" void p2_game_sound_main_loop(void* sysif);
// Smooth Motion (phase 2). Whether an in-between pass may run now (two snapshots, no movie, not
// paused...); captures the tick's state at its end; draws one pass at `alpha` (0..1 between the
// previous and the newest tick) and returns 1 when it drew. Stubs in phase 1.
extern "C" int p2_game_present_ready(void);
extern "C" void p2_game_present_tick_end(void);
extern "C" int p2_game_present(float alpha);
// Edit G4 (J3DModel::entry, phase 2): lists a model that entered the draw buffers this tick.
extern "C" void p2_present_note_model(void* model);
// Set while an in-between pass draws; edits G5/G6 skip camera updates, radar capture and
// draw-buffer resets under it.
typedef struct P2PresentState {
	int active;
} P2PresentState;
extern "C" P2PresentState gP2Present;

// ---- Online harness (docs/ONLINE_MULTIPLAYER.md, M5.1a): port/net ----
#include "p2_navi.h"
#include "p2_net.h"

#if defined(_WIN32)
// The Windows CRT's math.h defines _C2, which the game uses as a member name (HurryUp.h). Read
// math.h once here, then drop the macro; the include guard keeps later includes from bringing it back.
#include <math.h>
#undef _C2
#endif
