// Camera of the start-menu scene (docs/launcher/SCENE.md 8.2): low and close, the group to the
// right of the menu column (DESIGN.md 6.2), with a slow drift and a little parallax from the
// mouse. The take-off script (SceneTakeoff.h) can take it over through `mOverride`.
#pragma once

#include "Camera.h"
#include "SceneStage.h"

namespace LauncherScene {

// One camera pose in the stage frame.
struct CameraPose {
	Vector3f mPos;    // eye
	Vector3f mTarget; // look-at point
	f32 mFovY;        // degrees, vertical
	Vector3f mUp = Vector3f(0.0f, 1.0f, 0.0f); // up vector (stage frame); sky shots turn it
};

struct SceneCamera : public LookAtCamera {
	SceneCamera();

	void setStage(const StageFrame* frame) { mFrame = frame; }

	// The idle framing for the current content aspect (Hor+: the aim swings a little right on
	// wide windows so the group does not drift toward the menu column).
	CameraPose idlePose() const;

	// Per frame. `dt` in seconds; `reduceMotion` turns off drift and parallax.
	void update(f32 dt, bool reduceMotion);

	// Projects a world point to 0..1 of the picture (y down). False when behind the camera.
	bool project(const Vector3f& world, f32& x, f32& y) const;

	const StageFrame* mFrame;
	f32 mTime;
	f32 mParallaxX, mParallaxY; // low-passed pointer offsets, -1..1

	// Set by a script (take-off, free camera) to replace the idle pose for this frame.
	bool mOverride;
	CameraPose mOverridePose;
	Vector3f mShake; // stage-frame offset added to the eye and the target

	// MPPM_SCENE_CAM: a fixed pose that replaces every other (no drift, parallax or shake), for
	// tuning shots together with MPPM_SCENE_HOLD.
	bool mDebugCam;
	CameraPose mDebugPose;
};

} // namespace LauncherScene
