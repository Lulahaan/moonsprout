// Host services for the start-menu scene beyond the shared bridge in p2_host.h: test and tuning
// hooks that only the scene uses (docs/launcher/SCENE.md 8.3).
//
// Plain C so both sides can include it: the game side (port/game/scene) and the launcher's bridge
// (port/launcher/scene_bridge.cpp), which implements these functions. Kept out of p2_host.h because
// nothing else in the game needs them.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Raw test switches (SETTINGS.md 1.3: they stay environment checks).
typedef struct P2SceneDebug {
	int tune;    // MPPM_SCENE_TUNE=1: clip viewer, free camera and an on-screen readout
	float speed; // MPPM_SCENE_SPEED=<k>: scales the scene's dt (1 when unset or not positive)
	float hold;  // MPPM_SCENE_HOLD=<T>: dt becomes 0 once the take-off script clock reaches T s (0 = off)
	int hasCam;  // MPPM_SCENE_CAM="ex,ey,ez,tx,ty,tz,fov[,ux,uy,uz]": replaces the camera pose
	float cam[10]; // stage frame eye, target, vertical fov in degrees, up (default 0,1,0)
	int clipLog; // MPPM_SCENE_CLIPLOG=1: log "scene: clip <actor> <id>" on every startClip
} P2SceneDebug;
void p2_launcher_scene_debug(P2SceneDebug* out);

// Keys of the tuning mode, sampled by the host once per frame from the keyboard state. The start
// menu keeps the arrows, Enter, Escape and Space; tuning uses keys it never reads.
enum {
	P2_TUNE_NEXT_CLIP  = 1 << 0,  // ]
	P2_TUNE_PREV_CLIP  = 1 << 1,  // [
	P2_TUNE_PAUSE      = 1 << 2,  // P: pause / resume the selected actor's clip
	P2_TUNE_STEP       = 1 << 3,  // .: one frame forward while paused
	P2_TUNE_STEP_BACK  = 1 << 4,  // ,: one frame back while paused
	P2_TUNE_NEXT_ACTOR = 1 << 5,  // Tab: select the next actor
	P2_TUNE_FREE_CAM   = 1 << 6,  // C: toggle the free camera
	P2_TUNE_RESET      = 1 << 7,  // Backspace: back to the stage camera and WAIT
	P2_TUNE_PRINT      = 1 << 8,  // F8: print the camera and actors to the log
	P2_TUNE_CAM_LEFT   = 1 << 9,  // J (held)
	P2_TUNE_CAM_RIGHT  = 1 << 10, // L (held)
	P2_TUNE_CAM_FWD    = 1 << 11, // I (held)
	P2_TUNE_CAM_BACK   = 1 << 12, // K (held)
	P2_TUNE_CAM_UP     = 1 << 13, // O (held)
	P2_TUNE_CAM_DOWN   = 1 << 14, // U (held)
	P2_TUNE_YAW_LEFT   = 1 << 15, // Y (held): turn the free camera
	P2_TUNE_YAW_RIGHT  = 1 << 16, // H (held)
	P2_TUNE_FOV_IN     = 1 << 17, // - (held)
	P2_TUNE_FOV_OUT    = 1 << 18, // = (held)
	P2_TUNE_PITCH_UP   = 1 << 19, // T (held): tilt the free camera up
	P2_TUNE_PITCH_DOWN = 1 << 20, // G (held)
	P2_TUNE_ARM_FIX    = 1 << 21, // B: Olimar's shoulder offset on / off (before-and-after checks)
};
typedef struct P2SceneTuneInput {
	unsigned pressed; // went down since the last call
	unsigned held;    // down now
} P2SceneTuneInput;
void p2_launcher_scene_tune_input(P2SceneTuneInput* out);

#ifdef __cplusplus
}
#endif
