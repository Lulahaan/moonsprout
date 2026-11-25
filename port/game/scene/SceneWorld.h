// What the director, the take-off script and the debug tools share: the stage, the cast, the
// camera and the settings of this run. Owned by LauncherScene::Section (SceneSection.h).
#pragma once

#include "p2_host.h"
#include "SceneStage.h"
#include "SceneActor.h"
#include "SceneCamera.h"

namespace LauncherScene {

struct Assets;
struct SceneLightMgr;
struct OnionActor;

// Home positions and default facing of the cast (SCENE.md 7.1), stage frame. Tune.
struct CastEntry {
	int mKind;          // ActorKind
	f32 mHomeX, mHomeZ; // stage frame
};
extern const CastEntry kCast[ACTOR_Count];

struct SceneWorld {
	SceneWorld();

	// Stage frame helpers: every placement in the scene is authored in the stage frame.
	Vector3f stagePos(f32 x, f32 z) const { return mFrame.toWorld(Vector3f(x, 0.0f, z)); }
	Vector3f stagePos(const Vector3f& p) const { return mFrame.toWorld(p); }
	Vector3f home(int kind) const { return stagePos(kCast[kind].mHomeX, kCast[kind].mHomeZ); }
	Vector3f cameraEye() const; // world position of the idle camera eye
	f32 defaultFaceDir(int kind) const; // where an actor faces when nothing happens

	P2SceneConfig mConfig;
	const StageInfo* mStage;
	StageFrame mFrame;
	Assets* mAssets;
	Actor* mActors[ACTOR_Count]; // null when the model did not load
	ShipActor* mShip;            // null without a ship
	OnionActor* mOnions[3] = { nullptr, nullptr, nullptr }; // red, yellow, blue; null without them
	SceneCamera* mCamera;
	SceneLightMgr* mLight;
	f32 mTime;  // seconds since the scene became ready
	bool mTune; // MPPM_SCENE_TUNE: the debug tools own the actors and the camera
};

} // namespace LauncherScene
