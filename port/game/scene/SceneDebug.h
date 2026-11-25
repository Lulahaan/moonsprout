// Tuning tools of the start-menu scene behind MPPM_SCENE_TUNE=1 (docs/launcher/SCENE.md 8.3):
// a clip viewer on any actor (next / previous clip, pause, frame step), a free camera, and a
// readout of the camera and the cast in the stage frame. Keys: SceneHost.h.
#pragma once

#include "types.h"
#include "SceneCamera.h"

struct Graphics;

namespace LauncherScene {

struct SceneWorld;
struct Director;

struct SceneDebug {
	SceneDebug();

	void update(SceneWorld& world, Director& director, f32 dt);
	void draw(Graphics& gfx, SceneWorld& world);

	// Whether this actor's clip should advance this frame (false while paused).
	bool animates(int actor) const { return !(mPaused && actor == mActor); }

	int mActor;
	int mClip; // clip shown on the selected actor, -1 = the director's
	bool mPaused;
	bool mFreeCam;
	CameraPose mFree; // stage frame
	f32 mFreeYaw;     // radians, J3D convention
	f32 mFreePitch;   // radians, up
};

} // namespace LauncherScene
