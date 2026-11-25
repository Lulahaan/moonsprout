#pragma once
// The live start-menu scene as the launcher UI sees it (docs/launcher/README.md 3, SCENE.md 4).
//
// The game side talks C through p2_launcher_scene_* (port/include/p2_host.h); this header is the
// C++ view for the start menu (W3) and the boot flow. Stub until package W9: there is no scene,
// state() reports None, no cue ever fires, and the boot flow runs the standalone menu.

#include <cstdint>
#include <functional>

namespace mppm::launcher::scene {

enum class State {
    None,     // no scene this run (not available, Live Start Menu off, or skipped)
    Loading,  // the scene section is loading behind the opaque backdrop
    Ready,    // first complete frame drawn: the backdrop may fade (body class `scene-ready`)
    Takeoff,  // Play was pressed; the take-off runs
    Fading,   // the scene fades to the end colour
    Done,     // DESIGN's `handoff`: the title section follows
    Failed,   // the scene could not load; the section idles empty until Play
    Parked,   // loaded behind the menu but not drawn and silent (Live Start Menu off, live_options D1)
};

// Same order as P2_SCENE_CUE_* in p2_host.h.
enum class Cue { LaunchBegin, Hop, Board, Ignite, Liftoff, ExitFrame };

enum class Command { Play, Skip };

// Whether this build can run the live scene at all. False until package W9 lands, which makes the
// boot flow treat Live Start Menu as off.
bool available();

// Arms the scene for the coming boot: p2_launcher_scene_pending() returns 1 until the game has
// created the scene section. Called by the boot flow before p2_game_main.
void arm();

State state();

// ---- Live settings (live_options_design 3.1, 3.2) ----

// Whether the config handed to the scene has it running (P2SceneConfig.live), not parked.
bool live();
// True while the start menu should keep (or bring back) its opaque backdrop because a structural
// change (area, live) is about to be handed to the scene or is being applied behind it.
bool cover_wanted();
// cover_wanted() or the scene is loading: Play waits ("Landing...") and the scene rows show
// "Applying...".
bool busy();

// Registers a callback for take-off cues (main thread, from launcher::update). Returns an id for
// remove_cue_listener.
int on_cue(std::function<void(Cue)> fn);
void remove_cue_listener(int id);

// The ship's position in 0..1 of the content rectangle (y down), from IGNITE on. False when the
// ship is off screen or no position has been reported.
bool ship_screen(float& x, float& y);

// Queues a command for the scene (read once by p2_launcher_scene_command).
void send(Command command);

// Per frame on the main thread: delivers cues the game reported since the last call.
void update();

// ---- Take-off skip (SCENE.md / README 6: any button skips) ----

// Called when Play is pressed: forgets earlier presses and holds the skip until every key, mouse
// button and gamepad button held at that moment has been released, so the Play press itself (a
// held Enter, a double click) can never count as a skip.
void arm_skip();

// True once per new press of any key, mouse button or gamepad button since the last call, after
// arm_skip's release rule. Modifier keys, F1-F24, Print Screen, volume and media keys and keys
// pressed with Alt or Super held do not count; gamepad buttons count only while the window has
// input focus or input.backgroundInput is on.
bool consume_any_press();

// False from arm_skip until everything held at the Play press has been released. Every skip path
// (consume_any_press, a confirm, a click) waits for it.
bool skip_armed();

// Milliseconds since the scene last polled p2_launcher_scene_command (once per scene frame), or
// -1 if it never did. The take-off treats a scene silent for 3 s as hung.
double heartbeat_age_ms();
// Counts the scene's polls; the take-off compares it across its own frames, so a whole-process
// pause (suspend, a debugger, a long hitch) is not mistaken for a hung scene.
uint64_t heartbeat_count();

} // namespace mppm::launcher::scene
