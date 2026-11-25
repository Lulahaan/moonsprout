// Tuning tools of the start-menu scene (docs/launcher/SCENE.md 8.3). Only active with
// MPPM_SCENE_TUNE=1; nothing here runs otherwise.
#include "SceneDebug.h"
#include "SceneDirector.h"
#include "SceneHost.h"
#include "SceneWorld.h"

#include "Graphics.h"
#include "JSystem/J2D/J2DPrint.h"
#include "JSystem/JFramework/JFWSystem.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "SysShape/AnimInfo.h"
#include "SysShape/AnimMgr.h"
#include "p2_wide.h"

#include <math.h>

namespace LauncherScene {
namespace {

const char* const kActorNames[ACTOR_Count] = { "Olimar", "Red", "Yellow", "Blue" };
const f32 kMoveSpeed = 60.0f; // units/s
const f32 kTurnSpeed = 1.2f;  // radians/s

const f32 kPitchSpeed = 0.6f; // radians/s
const f32 kMaxPitch   = 1.45f; // about 83 degrees: the sky shots look almost straight up

// Next valid clip id in `dir` from `clip`, wrapping around the set. Olimar steps through the clips
// he may play (Actor::olimarClipAllowed), the ones the viewer checks for arm clipping.
int stepClip(SysShape::AnimMgr* mgr, int clip, int dir, bool olimar)
{
	int count = mgr->mCount;
	for (int i = 0; i < count; i++) {
		clip = (clip + dir + count) % count;
		if (mgr->getAnimByID(clip) != nullptr && (!olimar || Actor::olimarClipAllowed(clip))) {
			return clip;
		}
	}
	return clip;
}

} // namespace

SceneDebug::SceneDebug()
    : mActor(ACTOR_Olimar)
    , mClip(-1)
    , mPaused(false)
    , mFreeCam(false)
    , mFreeYaw(0.0f)
    , mFreePitch(0.0f)
{
}

void SceneDebug::update(SceneWorld& world, Director& director, f32 dt)
{
	P2SceneTuneInput in;
	p2_launcher_scene_tune_input(&in);
	Actor* actor = world.mActors[mActor];

	if (in.pressed & P2_TUNE_NEXT_ACTOR) {
		for (int i = 1; i <= ACTOR_Count; i++) {
			int next = (mActor + i) % ACTOR_Count;
			if (world.mActors[next] != nullptr) {
				mActor = next;
				break;
			}
		}
		mClip   = -1;
		mPaused = false;
		actor   = world.mActors[mActor];
	}
	if (actor != nullptr && (in.pressed & (P2_TUNE_NEXT_CLIP | P2_TUNE_PREV_CLIP))) {
		int from = mClip < 0 ? actor->getClip() : mClip;
		mClip    = stepClip(actor->mAnimator.mAnimMgr, from, (in.pressed & P2_TUNE_NEXT_CLIP) ? 1 : -1, mActor == ACTOR_Olimar);
		actor->startClip(mClip); // while paused it opens paused on its first frame, ready to step
		if (SysShape::AnimInfo* info = actor->mAnimator.mAnimInfo) {
			OSReport("scene tune: %s clip %d %s, %d frames\n", kActorNames[mActor], mClip, info->mName ? info->mName : "?",
			         (int)info->mAnm->getTotalFrameCount());
		}
	}
	if (in.pressed & P2_TUNE_PAUSE) {
		mPaused = !mPaused;
	}
	if (in.pressed & P2_TUNE_ARM_FIX) {
		Actor::sArmFix = !Actor::sArmFix;
		OSReport("scene tune: Olimar's shoulder offset %s\n", Actor::sArmFix ? "on" : "off");
	}
	if (actor != nullptr && mPaused && (in.pressed & (P2_TUNE_STEP | P2_TUNE_STEP_BACK))) {
		SysShape::Animator& anim = actor->mAnimator;
		if (anim.mAnimInfo != nullptr) {
			f32 total = (f32)anim.mAnimInfo->mAnm->getTotalFrameCount();
			f32 frame = anim.getTimer() + ((in.pressed & P2_TUNE_STEP) ? 1.0f : -1.0f);
			frame     = frame < 0.0f ? total - 1.0f : (frame >= total ? 0.0f : frame);
			anim.setCurrFrame(frame);
			anim.mAnimInfo->mAnm->setFrame(frame);
		}
	}
	// A clip picked in the viewer replays when it ends instead of falling back to WAIT.
	if (actor != nullptr && mClip >= 0 && actor->isClipDone() && !mPaused) {
		actor->startClip(mClip);
	}

	if (in.pressed & P2_TUNE_FREE_CAM) {
		mFreeCam = !mFreeCam;
		if (mFreeCam && world.mCamera != nullptr) {
			mFree      = world.mCamera->idlePose();
			f32 dx     = mFree.mTarget.x - mFree.mPos.x;
			f32 dz     = mFree.mTarget.z - mFree.mPos.z;
			mFreeYaw   = atan2f(dx, dz);
			mFreePitch = atan2f(mFree.mTarget.y - mFree.mPos.y, sqrtf(dx * dx + dz * dz));
		}
	}
	if (in.pressed & P2_TUNE_RESET) {
		mFreeCam = false;
		mPaused  = false;
		mClip    = -1;
		Actor::sArmFix = true;
		director.reset(world);
	}

	if (mFreeCam && world.mCamera != nullptr) {
		f32 fwd    = ((in.held & P2_TUNE_CAM_FWD) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_CAM_BACK) ? 1.0f : 0.0f);
		f32 side   = ((in.held & P2_TUNE_CAM_RIGHT) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_CAM_LEFT) ? 1.0f : 0.0f);
		f32 up     = ((in.held & P2_TUNE_CAM_UP) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_CAM_DOWN) ? 1.0f : 0.0f);
		f32 turn   = ((in.held & P2_TUNE_YAW_LEFT) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_YAW_RIGHT) ? 1.0f : 0.0f);
		f32 zoom   = ((in.held & P2_TUNE_FOV_OUT) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_FOV_IN) ? 1.0f : 0.0f);
		f32 tilt   = ((in.held & P2_TUNE_PITCH_UP) ? 1.0f : 0.0f) - ((in.held & P2_TUNE_PITCH_DOWN) ? 1.0f : 0.0f);
		mFreeYaw += turn * kTurnSpeed * dt;
		mFreePitch += tilt * kPitchSpeed * dt;
		mFreePitch = mFreePitch < -kMaxPitch ? -kMaxPitch : (mFreePitch > kMaxPitch ? kMaxPitch : mFreePitch);
		f32 s = sinf(mFreeYaw), c = cosf(mFreeYaw);
		// Forward along the view, right = forward turned -90 degrees (J3D yaw).
		mFree.mPos.x += (s * fwd - c * side) * kMoveSpeed * dt;
		mFree.mPos.z += (c * fwd + s * side) * kMoveSpeed * dt;
		mFree.mPos.y += up * kMoveSpeed * dt;
		mFree.mFovY += zoom * 20.0f * dt;
		mFree.mFovY = mFree.mFovY < 10.0f ? 10.0f : (mFree.mFovY > 90.0f ? 90.0f : mFree.mFovY);
		f32 cp      = 200.0f * cosf(mFreePitch);
		mFree.mTarget = Vector3f(mFree.mPos.x + cp * s, mFree.mPos.y + 200.0f * sinf(mFreePitch), mFree.mPos.z + cp * c);
		world.mCamera->mOverride     = true;
		world.mCamera->mOverridePose = mFree;
	} else if (world.mCamera != nullptr) {
		world.mCamera->mOverride = false;
	}

	if (in.pressed & P2_TUNE_PRINT) {
		CameraPose p = mFreeCam ? mFree : (world.mCamera != nullptr ? world.mCamera->idlePose() : mFree);
		OSReport("scene tune: camera eye (%.1f, %.1f, %.1f) target (%.1f, %.1f, %.1f) fov %.1f\n", p.mPos.x, p.mPos.y, p.mPos.z, p.mTarget.x,
		         p.mTarget.y, p.mTarget.z, p.mFovY);
		for (int i = 0; i < ACTOR_Count; i++) {
			if (Actor* a = world.mActors[i]) {
				Vector3f s = world.mFrame.toStage(a->mPos);
				OSReport("scene tune: %-6s at (%.1f, %.1f) clip %d frame %.0f\n", kActorNames[i], s.x, s.z, a->getClip(), a->mAnimator.getTimer());
			}
		}
	}
}

void SceneDebug::draw(Graphics& gfx, SceneWorld& world)
{
	if (JFWSystem::systemFont == nullptr) {
		return;
	}
	gfx.mOrthoGraph.setPort();
	J2DPrint print(JFWSystem::systemFont, 0.0f);
	print.initiate();
	print.mGlyphWidth  = 12.0f;
	print.mGlyphHeight = 12.0f;
	f32 x = p2_wide_min_x() + 16.0f;
	f32 y = 24.0f; // top left: clear of the menu column

	Actor* a = world.mActors[mActor];
	if (a != nullptr && a->mAnimator.mAnimInfo != nullptr) {
		SysShape::AnimInfo* info = a->mAnimator.mAnimInfo;
		print.print(x, y, "%s  clip %d %s  %3.0f/%d%s%s", kActorNames[mActor], a->getClip(), info->mName ? info->mName : "?",
		            a->mAnimator.getTimer(), (int)info->mAnm->getTotalFrameCount(), mPaused ? "  [paused]" : "",
		            mActor == ACTOR_Olimar && !Actor::sArmFix ? "  [arms as authored]" : "");
		y += 14.0f;
	}
	if (world.mCamera != nullptr) {
		CameraPose p = mFreeCam ? mFree : world.mCamera->idlePose();
		print.print(x, y, "cam (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) fov %.0f%s", p.mPos.x, p.mPos.y, p.mPos.z, p.mTarget.x, p.mTarget.y,
		            p.mTarget.z, p.mFovY, mFreeCam ? "  [free]" : "");
		y += 14.0f;
	}
	print.print(x, y, "[ ] clip  P pause  , . step  Tab actor  B arms  C free cam IJKL UO YH TG -=  F8 log  Bksp reset");
}

} // namespace LauncherScene
