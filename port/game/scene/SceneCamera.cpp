// Camera of the start-menu scene (docs/launcher/SCENE.md 8.2). Tuned at 16:9 against the
// composition contract of DESIGN.md 6.2 and checked at 19:14, 16:10 and 21:9.
#include "SceneCamera.h"

#include "p2_host.h"
#include "p2_wide.h"

#include <math.h>

namespace LauncherScene {
namespace {

const f32 kDegToRad = 3.14159265f / 180.0f;

// Idle framing, stage frame (tune). The eye stands a little to the right of the ship's axis and
// barely above Olimar's knees, so the characters stand tall against the sky and the ship rises
// behind them; the aim swings left of the ship, which pushes the whole group into the right part
// of the picture and leaves the menu column to ground and sky.
const Vector3f kEye(20.0f, 11.0f, 354.0f);
const f32 kAimYawDeg      = 12.0f; // left of the line to the ship, at the original 19:14 aspect
const f32 kAimYawPerK     = 8.0f;  // more per unit of extra width (Hor+), so the group stays right
const f32 kAimYawMaxDeg   = 20.0f;
const f32 kAimPitchDeg    = 3.0f;  // up
const f32 kAimDistance    = 200.0f;
const f32 kFovY           = 36.0f;
const f32 kNear           = 5.0f;
const f32 kFar            = 12000.0f;
const f32 kParallaxYawDeg = 1.5f;
// Narrow windows (DESIGN.md 6.2 "at 4:3 the same fractions hold"): the menu column keeps its width
// in dp, so below 16:10 it takes a larger share of the picture while the group would take the same
// share. Below kMinAspect the lens keeps the horizontal view of kMinAspect (Vert+: a little more
// sky and ground) and the aim swings further, up to kNarrowYawDeg at 4:3, so the group shrinks
// and moves right, clear of the column.
const f32 kMinAspect    = 1.6f;
const f32 kNarrowYawDeg = 4.0f;
const f32 kParallaxPitch  = 0.8f;

f32 lowPass(f32 value, f32 target, f32 timeConstant, f32 dt)
{
	return value + (target - value) * (1.0f - expf(-dt / timeConstant));
}

} // namespace

SceneCamera::SceneCamera()
    : mFrame(nullptr)
    , mTime(0.0f)
    , mParallaxX(0.0f)
    , mParallaxY(0.0f)
    , mOverride(false)
    , mShake(0.0f, 0.0f, 0.0f)
    , mDebugCam(false)
{
	mViewAngle      = kFovY;
	mLookAtRotation = Vector3f(0.0f, 1.0f, 0.0f);
	setFixNearFar(true, kNear, kFar);
	setProjectionNearFar(kNear, kFar);
	mOverridePose.mPos    = kEye;
	mOverridePose.mTarget = Vector3f(0.0f, 30.0f, 0.0f);
	mOverridePose.mFovY   = kFovY;
	mDebugPose            = mOverridePose;
}

CameraPose SceneCamera::idlePose() const
{
	f32 k   = p2_wide_k();
	f32 yaw = kAimYawDeg + kAimYawPerK * (k - 1.0f);
	if (yaw > kAimYawMaxDeg) {
		yaw = kAimYawMaxDeg;
	}
	f32 fovY   = kFovY;
	f32 aspect = mAspectRatio > 0.0f ? mAspectRatio : 16.0f / 9.0f;
	if (aspect < kMinAspect) {
		f32 u = (kMinAspect - aspect) / (kMinAspect - 4.0f / 3.0f);
		yaw += kNarrowYawDeg * (u > 1.0f ? 1.0f : u);
		fovY = 2.0f * atanf(tanf(kFovY * 0.5f * kDegToRad) * kMinAspect / aspect) / kDegToRad;
	}
	yaw *= kDegToRad;
	f32 pitch = kAimPitchDeg * kDegToRad;

	// Base direction: from the eye toward the ship (stage origin), level.
	f32 baseYaw = atan2f(-kEye.x, -kEye.z); // J3D yaw convention: 0 = +Z
	f32 dirYaw  = baseYaw + yaw;            // turning left = toward -X when looking down -Z
	CameraPose pose;
	pose.mPos    = kEye;
	pose.mTarget = Vector3f(kEye.x + kAimDistance * sinf(dirYaw) * cosf(pitch), kEye.y + kAimDistance * sinf(pitch),
	                        kEye.z + kAimDistance * cosf(dirYaw) * cosf(pitch));
	pose.mFovY   = fovY;
	return pose;
}

void SceneCamera::update(f32 dt, bool reduceMotion)
{
	mTime += dt;

	CameraPose pose = mDebugCam ? mDebugPose : (mOverride ? mOverridePose : idlePose());
	if (!mOverride && !mDebugCam && !reduceMotion) {
		// Drift: a slow figure on two incommensurate periods, about a unit each way.
		pose.mPos.x += 1.2f * sinf(6.2831853f * mTime / 17.0f);
		pose.mPos.y += 0.5f * sinf(6.2831853f * mTime / 23.0f);

		// Parallax: the view leans a little toward the mouse while it is in use.
		float px = 0.5f, py = 0.5f;
		int active = 0;
		p2_launcher_scene_pointer(&px, &py, &active);
		f32 tx = active ? (px - 0.5f) * 2.0f : 0.0f;
		f32 ty = active ? (py - 0.5f) * 2.0f : 0.0f;
		mParallaxX = lowPass(mParallaxX, tx < -1.0f ? -1.0f : (tx > 1.0f ? 1.0f : tx), 0.35f, dt);
		mParallaxY = lowPass(mParallaxY, ty < -1.0f ? -1.0f : (ty > 1.0f ? 1.0f : ty), 0.35f, dt);

		// Rotate the target about the eye: yaw right with the pointer right, pitch down with it down.
		Vector3f d(pose.mTarget.x - pose.mPos.x, pose.mTarget.y - pose.mPos.y, pose.mTarget.z - pose.mPos.z);
		f32 dyaw   = -mParallaxX * kParallaxYawDeg * kDegToRad;
		f32 dpitch = -mParallaxY * kParallaxPitch * kDegToRad;
		f32 s = sinf(dyaw), c = cosf(dyaw);
		Vector3f r(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
		r.y += dpitch * sqrtf(r.x * r.x + r.z * r.z);
		pose.mTarget = Vector3f(pose.mPos.x + r.x, pose.mPos.y + r.y, pose.mPos.z + r.z);
	}
	if (!mDebugCam) {
		pose.mPos.x += mShake.x;
		pose.mPos.y += mShake.y;
		pose.mPos.z += mShake.z;
		pose.mTarget.x += mShake.x;
		pose.mTarget.y += mShake.y;
		pose.mTarget.z += mShake.z;
	}

	if (mFrame != nullptr) {
		mPosition       = mFrame->toWorld(pose.mPos);
		mLookAtPosition = mFrame->toWorld(pose.mTarget);
	} else {
		mPosition       = pose.mPos;
		mLookAtPosition = pose.mTarget;
	}
	mViewAngle      = pose.mFovY;
	mLookAtRotation = mFrame != nullptr ? mFrame->dirToWorld(pose.mUp) : pose.mUp;
	Camera::update(); // view matrix, sound camera, frustum planes
}

bool SceneCamera::project(const Vector3f& world, f32& x, f32& y) const
{
	const Mtx& m = mLookMatrix.mMatrix.mtxView;
	f32 vx       = m[0][0] * world.x + m[0][1] * world.y + m[0][2] * world.z + m[0][3];
	f32 vy       = m[1][0] * world.x + m[1][1] * world.y + m[1][2] * world.z + m[1][3];
	f32 vz       = m[2][0] * world.x + m[2][1] * world.y + m[2][2] * world.z + m[2][3];
	if (vz > -0.001f) {
		return false; // behind the eye (the camera looks down -Z in view space)
	}
	f32 t = tanf(mViewAngle * 0.5f * kDegToRad);
	f32 a = mAspectRatio > 0.0f ? mAspectRatio : 1.0f;
	f32 nx = (vx / -vz) / (t * a);
	f32 ny = (vy / -vz) / t;
	x      = 0.5f + 0.5f * nx;
	y      = 0.5f - 0.5f * ny;
	return true;
}

} // namespace LauncherScene
