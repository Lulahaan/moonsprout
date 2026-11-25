// Actors of the start-menu scene (docs/launcher/SCENE.md 7.2, 7.5). Each per-frame step follows
// the gameplay code it is named after, minus everything that needs the creature system.
#include "SceneActor.h"
#include "SceneAssets.h"

#include "System.h"
#include "Viewport.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DMtxCalc.h"
#include "JSystem/J3D/J3DShape.h"
#include "JSystem/J3D/J3DSys.h"
#include "SysShape/AnimMgr.h"
#include "SysShape/AnimInfo.h"
#include "SysShape/Joint.h"
#include "SysShape/Model.h"

#include <math.h>
#include <string.h>

namespace LauncherScene {
namespace {

const f32 kPi  = 3.14159265f;
const f32 kTau = 6.2831853f;

const f32 kDegToRad = kPi / 180.0f;

// Head angles for the joint callback, set just before each actor's calc().
f32 sNeckYaw   = 0.0f;
f32 sNeckPitch = 0.0f;

// ---- Olimar's arms (SCENE.md 7.3, "Olimar") ----
// Olimar and the Pikmin share one skeleton and clip set. His shoulder pivots (lhandjnt and
// rhandjnt, leaves under the torso joint sebonjnt) sit +-1.52 units from the spine, inside his
// round torso. Hanging at his sides (Wait, Walk) the sleeves merge into the torso exactly as in
// gameplay, which draws him with the same model, clips and flags (checked against the field at
// close zoom): that pose needs no help, and moving the pivot out there opened a notch between
// sleeve and body. Only an arm swung well forward or back can push the glove into the belly or
// the backpack, so the callback moves the pivot out along the torso's side axis by up to
// kShoulderOut, weighted by how far the arm points along the torso's forward axis. Model units,
// times the actor's scale.
const f32 kShoulderOut = 0.6f;
// The weight by the arm's forward (or backward) share: 0 up to kShoulderSwingFrom (the hanging
// and walking poses stay as authored), 1 from kShoulderSwingFull.
const f32 kShoulderSwingFrom = 0.45f;
const f32 kShoulderSwingFull = 0.85f;
// Set just before Olimar's calc(): his torso joint index (-1: no fix for this model) and scale.
int sTorsoJoint     = -1;
f32 sShoulderOut    = 0.0f;
// Set just before Olimar's calc(): the waving arm's turn (radians, 0: no wave) and which arm
// (the sign of its side along the torso's Z axis).
f32 sWaveAngle = 0.0f;
int sWaveSide  = 1;
// The wave (Actor::wave): the arm rises to kWaveLift, swings kWaveSwing either way at kWaveHz.
const f32 kWaveLift  = 1.75f; // radians from hanging: just above level, the glove clear of the helmet
const f32 kWaveSwing = 0.38f;
const f32 kWaveHz    = 2.4f;
const f32 kWaveRise  = 0.28f; // seconds to raise and to lower

// Olimar's whitelist (SCENE.md 7.3): the clips checked in the clip viewer at the idle camera with
// the shoulder offset, every frame, with no glove or sleeve inside the torso or the helmet. JUMP is
// exempt: he only plays it in the take-off's beam shot, far from the lens.
const int kOlimarClips[] = {
	CLIP_Wait, CLIP_Walk, CLIP_Run2, CLIP_Asibumi, CLIP_Fue, CLIP_Furimuku, CLIP_Akubi, CLIP_Sagasu2, CLIP_Suwaru, CLIP_Mizunomi,
	CLIP_Kizuku, CLIP_Getup, CLIP_Jump,
};

// Olimar's kneel (SCENE.md 7.3): MIZUNOMI's frames 0..kKneelFrame are the bend down to a Pikmin's
// height with the face still forward; past it he plunges his face to the ground to drink. Down in
// kKneelDown, held, up in kKneelUp (frames played backwards), kKneelLength in all.
const f32 kKneelFrame  = 5.0f;
const f32 kKneelDown   = 0.35f;
const f32 kKneelUp     = 0.40f;
const f32 kKneelLength = 1.9f;

// Gestures (seconds, degrees).
const f32 kNodLength   = 0.6f;
const f32 kNodDipDeg   = 12.0f;
const f32 kSighLength  = 1.2f;
const f32 kSighDeg     = -20.0f;
const f32 kSighStepFor = 0.6f;  // the ASIBUMI shuffle under the sigh
const f32 kDozeDeg     = -25.0f;
const f32 kDozeSwayDeg = 3.0f;
const f32 kDozeSwayHz  = 0.25f;
const f32 kBiasRate    = 3.5f;  // 1/s: the bias reaches 95% of its target in about 0.85 s
const f32 kNeckMinDeg  = -35.0f;
const f32 kNeckMaxDeg  = 30.0f;

f32 wrapPi(f32 a)
{
	while (a > kPi) {
		a -= kTau;
	}
	while (a < -kPi) {
		a += kTau;
	}
	return a;
}

f32 clampf(f32 v, f32 lo, f32 hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Frame-rate independent exponential approach: the share of the remaining distance covered in dt.
f32 approach(f32 rate, f32 dt)
{
	return 1.0f - expf(-rate * dt);
}

bool clipLoops(SysShape::AnimMgr* mgr, int clip)
{
	SysShape::AnimInfo* info = mgr != nullptr ? mgr->getAnimByID(clip) : nullptr;
	return info != nullptr && info->getAnimKeyByType(KEYEVENT_LOOP_END) != nullptr;
}

} // namespace

// ---- Actor -------------------------------------------------------------------------------------

Actor::Actor()
    : mKind(ACTOR_Olimar)
    , mModel(nullptr)
    , mAnimSpeed(30.0f)
    , mClip(-1)
    , mClipDone(false)
    , mVisible(true)
    , mHappa(HAPPA_Leaf)
    , mPos(0.0f, 0.0f, 0.0f)
    , mFaceDir(0.0f)
    , mTargetFaceDir(0.0f)
    , mTurnRate(6.0f)
    , mScale(1.0f)
    , mMoveTarget(0.0f, 0.0f, 0.0f)
    , mMoveSpeed(0.0f)
    , mLookAt(0.0f, 0.0f, 0.0f)
    , mHasLookAt(false)
    , mNeckYaw(0.0f)
    , mNeckPitch(0.0f)
    , mNeckBias(0.0f)
    , mNodTime(-1.0f)
    , mSighTime(-1.0f)
    , mGestureLeft(-1.0f)
    , mDozeTime(0.0f)
    , mDoze(false)
    , mWaveTime(-1.0f)
    , mWaveLength(0.0f)
    , mWaveSide(1)
    , mHeadJoint(nullptr)
    , mLeafJoint(nullptr)
{
	PSMTXIdentity(mBaseMtx.mMatrix.mtxView);
}

void Actor::init(int kind, SysShape::Model* model, SysShape::AnimMgr* animMgr)
{
	mKind               = kind;
	mModel              = model;
	mAnimator.mAnimMgr  = animMgr;
	mHeadJoint          = model->getJoint((char*)"headjnt");
	mLeafJoint          = model->getJoint((char*)"happajnt3");
	if (mHeadJoint != nullptr) {
		mHeadJoint->mJ3d->setCallBack(neckCallback);
	}
	if (kind == ACTOR_Olimar) {
		// orima1.bmd has its own model data: the Pikmin keep their arms as authored.
		SysShape::Joint* torso = model->getJoint((char*)"sebonjnt");
		SysShape::Joint* left  = model->getJoint((char*)"lhandjnt");
		SysShape::Joint* right = model->getJoint((char*)"rhandjnt");
		if (torso != nullptr && left != nullptr && right != nullptr) {
			sTorsoJoint = torso->mJointIndex;
			left->mJ3d->setCallBack(shoulderCallback);
			right->mJ3d->setCallBack(shoulderCallback);
		} else {
			OSReport("scene: Olimar's arm joints not found; arms as authored\n");
		}
	}
	startClip(CLIP_Wait);
}

bool Actor::sClipLog = false;
bool Actor::sArmFix  = true;

bool Actor::olimarClipAllowed(int clip)
{
	for (u32 i = 0; i < sizeof(kOlimarClips) / sizeof(kOlimarClips[0]); i++) {
		if (kOlimarClips[i] == clip) {
			return true;
		}
	}
	return false;
}

void Actor::startClip(int clip)
{
	if (mAnimator.mAnimMgr == nullptr || mAnimator.mAnimMgr->getAnimByID(clip) == nullptr) {
		return;
	}
	static const char* const kNames[ACTOR_Count] = { "olimar", "red", "yellow", "blue" };
	const char* name = mKind >= 0 && mKind < ACTOR_Count ? kNames[mKind] : "?";
	f32 gesture      = -1.0f; // a mapped gesture's length (isClipDone after it)
	bool doze        = false;
	if (mKind == ACTOR_Olimar && !olimarClipAllowed(clip)) {
		int asked = clip;
		switch (clip) {
		case CLIP_Sagasu:
			clip = CLIP_Sagasu2;
			break;
		case CLIP_Gattu:
			nod();
			clip    = CLIP_Wait;
			gesture = kNodLength;
			break;
		case CLIP_Gakkari:
			sigh();
			clip    = CLIP_Asibumi;
			gesture = kSighStepFor;
			break;
		case CLIP_Neru:
			doze = true;
			clip = CLIP_Suwaru;
			if (mClip == CLIP_Suwaru) {
				// Already sitting: keep the loop running, only the head drops.
				setDoze(true);
				if (sClipLog) {
					OSReport("scene: clip %s %d (for %d)\n", name, clip, asked);
				}
				return;
			}
			break;
		default:
			OSReport("scene: Olimar does not play clip %d; WAIT instead\n", clip);
			clip = CLIP_Wait;
			break;
		}
		if (sClipLog) {
			OSReport("scene: clip %s %d (for %d)\n", name, clip, asked);
		}
	} else if (sClipLog) {
		OSReport("scene: clip %s %d\n", name, clip);
	}
	mAnimator.startAnim(clip, this);
	mKneelTime = -1.0f;
	if (mKind == ACTOR_Olimar && clip == CLIP_Mizunomi) {
		mKneelTime = 0.0f;
		gesture    = kKneelLength;
	}
	mClip        = clip;
	mClipDone    = false;
	mGestureLeft = gesture;
	setDoze(doze);
}

void Actor::onKeyEvent(const SysShape::KeyEvent& event)
{
	if (event.mType == KEYEVENT_END && mGestureLeft < 0.0f) {
		mClipDone = true;
	}
}

void Actor::nod()
{
	mNodTime = 0.0f;
}

void Actor::wave(f32 seconds, int side)
{
	mWaveTime   = 0.0f;
	mWaveLength = seconds;
	mWaveSide   = side;
}

void Actor::sigh()
{
	mSighTime = 0.0f;
}

void Actor::setDoze(bool on)
{
	if (on && !mDoze) {
		mDozeTime = 0.0f;
	}
	mDoze = on;
}

void Actor::moveTo(const Vector3f& target, f32 speed)
{
	mMoveTarget = target;
	mMoveSpeed  = speed;
}

void Actor::faceToward(const Vector3f& point)
{
	f32 dx = point.x - mPos.x;
	f32 dz = point.z - mPos.z;
	if (dx * dx + dz * dz > 0.0001f) {
		mTargetFaceDir = atan2f(dx, dz);
	}
}

void Actor::lookAt(const Vector3f* point)
{
	mHasLookAt = point != nullptr;
	if (point != nullptr) {
		mLookAt = *point;
	}
}

void Actor::update(f32 dt, bool animate)
{
	// Straight-line walking on the flat landing site, facing the way it goes.
	if (mMoveSpeed > 0.0f) {
		f32 dx   = mMoveTarget.x - mPos.x;
		f32 dz   = mMoveTarget.z - mPos.z;
		f32 dist = sqrtf(dx * dx + dz * dz);
		f32 step = mMoveSpeed * dt;
		if (dist <= step || dist < 0.01f) {
			mPos.x     = mMoveTarget.x;
			mPos.z     = mMoveTarget.z;
			mMoveSpeed = 0.0f;
		} else {
			mPos.x += dx / dist * step;
			mPos.z += dz / dist * step;
			mTargetFaceDir = atan2f(dx, dz);
		}
	}
	mFaceDir = wrapPi(mFaceDir + wrapPi(mTargetFaceDir - mFaceDir) * approach(mTurnRate, dt));

	// Head: yaw relative to the body and pitch toward the look-at point, eased and clamped.
	f32 targetYaw   = 0.0f;
	f32 targetPitch = 0.0f;
	if (mHasLookAt) {
		Vector3f head = mHeadJoint != nullptr ? getHeadPos() : mPos;
		f32 dx        = mLookAt.x - head.x;
		f32 dy        = mLookAt.y - head.y;
		f32 dz        = mLookAt.z - head.z;
		targetYaw     = clampf(wrapPi(atan2f(dx, dz) - mFaceDir), -kPi / 3.0f, kPi / 3.0f);
		targetPitch   = clampf(atan2f(dy, sqrtf(dx * dx + dz * dz)), -kPi / 6.0f, kPi / 6.0f);
	}
	mNeckYaw += (targetYaw - mNeckYaw) * approach(5.0f, dt);
	mNeckPitch += (targetPitch - mNeckPitch) * approach(5.0f, dt);

	// Head gestures: the bias eases toward the sigh or the doze; the nod rides on top of it.
	f32 biasTarget = 0.0f;
	if (mSighTime >= 0.0f) {
		mSighTime += dt;
		if (mSighTime < kSighLength) {
			biasTarget = kSighDeg * kDegToRad;
		} else {
			mSighTime = -1.0f;
		}
	}
	if (mDoze) {
		mDozeTime += dt;
		biasTarget = (kDozeDeg + kDozeSwayDeg * sinf(kTau * kDozeSwayHz * mDozeTime)) * kDegToRad;
	}
	mNeckBias += (biasTarget - mNeckBias) * approach(kBiasRate, dt);
	f32 nod = 0.0f;
	if (mNodTime >= 0.0f) {
		mNodTime += dt;
		if (mNodTime < kNodLength) {
			f32 s = sinf(kPi * mNodTime / (kNodLength * 0.5f)); // two dips: sin^2 has half the period
			nod   = -kNodDipDeg * kDegToRad * s * s;
		} else {
			mNodTime = -1.0f;
		}
	}
	if (mGestureLeft >= 0.0f) {
		mGestureLeft -= dt;
		if (mGestureLeft < 0.0f) {
			mClipDone = true; // the gesture stood in for a one-shot clip: it is over now
		}
	}

	// Clip. Animator::animate writes the frame into the shared J3DAnmTransform, so the model must
	// be calculated right after (as FakePiki::doAnimation does).
	if (mKneelTime >= 0.0f) {
		// The kneel sets its frame itself; it holds frame 0 (standing) once it is over.
		if (animate) {
			mKneelTime += dt;
		}
		f32 k = 0.0f;
		if (mKneelTime < kKneelDown) {
			k = mKneelTime / kKneelDown;
		} else if (mKneelTime < kKneelLength - kKneelUp) {
			k = 1.0f;
		} else {
			k = clampf((kKneelLength - mKneelTime) / kKneelUp, 0.0f, 1.0f);
		}
		k = k * k * (3.0f - 2.0f * k);
		mAnimator.setCurrFrame(kKneelFrame * k);
		mAnimator.animate(0.0f);
	} else {
		mAnimator.animate(animate ? mAnimSpeed * dt : 0.0f);
	}
	// No setFrame(mTimer) here, unlike ShipActor: the characters' clips are .bca (full, one pose
	// per frame), whose getTransform rounds the frame and does not interpolate, so it would only
	// turn floor into round and could show the frame past a loop end for one picture.
	mAnimator.setModelCalc(mModel, 0);
	mModel->getJ3DModel()->getModelData()->getJointNodePointer(4)->setMtxCalc(nullptr);

	Vector3f scale(mScale, mScale, mScale);
	Vector3f rotation(0.0f, mFaceDir, 0.0f);
	mBaseMtx.makeSRT(scale, rotation, mPos);
	PSMTXCopy(mBaseMtx.mMatrix.mtxView, mModel->getJ3DModel()->mPosMtx);

	sNeckYaw     = mNeckYaw;
	sNeckPitch   = clampf(mNeckPitch + mNeckBias + nod, kNeckMinDeg * kDegToRad, kNeckMaxDeg * kDegToRad);
	sShoulderOut = mKind == ACTOR_Olimar && sArmFix ? kShoulderOut * mScale : 0.0f;
	sWaveAngle   = 0.0f;
	if (mWaveTime >= 0.0f) {
		mWaveTime += dt;
		if (mWaveTime >= mWaveLength) {
			mWaveTime = -1.0f;
		} else if (mKind == ACTOR_Olimar) {
			f32 up   = clampf(mWaveTime / kWaveRise, 0.0f, 1.0f) * clampf((mWaveLength - mWaveTime) / kWaveRise, 0.0f, 1.0f);
			up       = up * up * (3.0f - 2.0f * up);
			f32 sw   = sinf(kTau * kWaveHz * clampf(mWaveTime - kWaveRise, 0.0f, mWaveLength));
			sWaveAngle = up * (kWaveLift + kWaveSwing * sw);
			sWaveSide  = mWaveSide;
		}
	}
	mModel->getJ3DModel()->calc();
	sShoulderOut = 0.0f;
	sWaveAngle   = 0.0f;
}

void Actor::entry()
{
	if (mVisible) {
		mModel->getJ3DModel()->entry();
	}
}

void Actor::viewCalc()
{
	if (mVisible) {
		mModel->getJ3DModel()->viewCalc();
	}
}

Vector3f Actor::getJointPos(SysShape::Joint* joint) const
{
	if (joint == nullptr) {
		return mPos;
	}
	Matrixf* m = joint->getWorldMatrix();
	return Vector3f(m->mMatrix.mtxView[0][3], m->mMatrix.mtxView[1][3], m->mMatrix.mtxView[2][3]);
}

// FakePiki::sNeckCallback without the OptimiseController switch: multiplies the head joint's world
// matrix by the yaw / pitch rotation. Gameplay passes its vertical angle as "phi" and its
// horizontal one as "theta".
bool Actor::neckCallback(J3DJoint* joint, int calcStage)
{
	if (calcStage != 0) {
		return false;
	}
	Matrixf* worldMat = J3DMtxCalc::mMtxBuffer->getWorldMatrix(joint->getJntNo());

	f32 sinPhi   = sinf(-sNeckPitch);
	f32 cosPhi   = cosf(-sNeckPitch);
	f32 sinTheta = sinf(sNeckYaw);
	f32 cosTheta = cosf(sNeckYaw);

	Matrixf rotMtx;
	rotMtx.mMatrix.structView.xx = cosPhi;
	rotMtx.mMatrix.structView.yx = -sinPhi;
	rotMtx.mMatrix.structView.zx = 0.0f;

	rotMtx.mMatrix.structView.xy = cosTheta * sinPhi;
	rotMtx.mMatrix.structView.yy = cosTheta * cosPhi;
	rotMtx.mMatrix.structView.zy = -sinTheta;

	rotMtx.mMatrix.structView.xz = sinTheta * sinPhi;
	rotMtx.mMatrix.structView.yz = sinTheta * cosPhi;
	rotMtx.mMatrix.structView.zz = cosTheta;

	rotMtx.mMatrix.structView.tx = 0.0f;
	rotMtx.mMatrix.structView.ty = 0.0f;
	rotMtx.mMatrix.structView.tz = 0.0f;

	Matrixf worldCopy = *worldMat;
	PSMTXConcat(worldCopy.mMatrix.mtxView, rotMtx.mMatrix.mtxView, worldMat->mMatrix.mtxView);
	PSMTXCopy(worldMat->mMatrix.mtxView, J3DSys::mCurrentMtx);
	return false;
}

// At calc stage 0 of lhandjnt / rhandjnt (after the torso and the head: both arms are leaves under
// sebonjnt that come after headjnt): the arm's world matrix M is moved along the torso's side axis,
// away from the spine, by sShoulderOut. J3DSys::mCurrentMtx follows, as in neckCallback.
bool Actor::shoulderCallback(J3DJoint* joint, int calcStage)
{
	if (calcStage != 0 || (sShoulderOut == 0.0f && sWaveAngle == 0.0f) || sTorsoJoint < 0) {
		return false;
	}
	Matrixf* arm   = J3DMtxCalc::mMtxBuffer->getWorldMatrix(joint->getJntNo());
	Matrixf* torso = J3DMtxCalc::mMtxBuffer->getWorldMatrix(sTorsoJoint);
	Mtx& m         = arm->mMatrix.mtxView;
	Mtx& t         = torso->mMatrix.mtxView;
	f32 zx = t[0][2], zy = t[1][2], zz = t[2][2];
	f32 len = sqrtf(zx * zx + zy * zy + zz * zz);
	if (len < 1.0e-5f) {
		return false;
	}
	zx /= len;
	zy /= len;
	zz /= len;
	f32 side = (m[0][3] - t[0][3]) * zx + (m[1][3] - t[1][3]) * zy + (m[2][3] - t[2][3]) * zz;
	// The arm's X axis runs from the shoulder to the hand; the torso's Y axis is its forward
	// (or back) axis, X runs up the spine and Z across.
	f32 ax = m[0][0], ay = m[1][0], az = m[2][0];
	f32 fx = t[0][1], fy = t[1][1], fz = t[2][1];
	f32 al  = sqrtf(ax * ax + ay * ay + az * az) * sqrtf(fx * fx + fy * fy + fz * fz);
	f32 fwd = al > 1.0e-5f ? fabsf(ax * fx + ay * fy + az * fz) / al : 0.0f;
	f32 w   = (fwd - kShoulderSwingFrom) / (kShoulderSwingFull - kShoulderSwingFrom);
	w       = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
	w       = w * w * (3.0f - 2.0f * w);
	f32 out  = (side < 0.0f ? -sShoulderOut : sShoulderOut) * w;
	m[0][3] += zx * out;
	m[1][3] += zy * out;
	m[2][3] += zz * out;
	if (sWaveAngle != 0.0f && (side < 0.0f) == (sWaveSide < 0)) {
		// The wave: the arm swings out sideways and up about the torso's forward axis, round its own
		// pivot.
		f32 fl = sqrtf(fx * fx + fy * fy + fz * fz);
		if (fl > 1.0e-5f) {
			Vec axis = { fx / fl, fy / fl, fz / fl };
			// A hanging arm points down the spine (-X of the torso): turn the way that carries
			// that direction out to the arm's own side.
			f32 dx = -t[0][0], dy = -t[1][0], dz = -t[2][0];
			f32 cx = axis.y * dz - axis.z * dy;
			f32 cy = axis.z * dx - axis.x * dz;
			f32 cz = axis.x * dy - axis.y * dx;
			f32 away = (cx * zx + cy * zy + cz * zz) * (side < 0.0f ? -1.0f : 1.0f);
			Mtx turn, rot;
			PSMTXRotAxisRad(turn, &axis, away < 0.0f ? -sWaveAngle : sWaveAngle);
			PSMTXCopy(m, rot);
			rot[0][3] = rot[1][3] = rot[2][3] = 0.0f;
			PSMTXConcat(turn, rot, rot);
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++) {
					m[r][c] = rot[r][c];
				}
			}
		}
	}
	PSMTXCopy(m, J3DSys::mCurrentMtx);
	return false;
}

// ---- ShipActor ---------------------------------------------------------------------------------

ShipActor::ShipActor()
    : mModel(nullptr)
    , mRootSpeed(0.0f)
    , mRootFrame(-1.0f)
    , mPos(0.0f, 0.0f, 0.0f)
    , mFaceDir(0.0f)
    , mOffset(0.0f, 0.0f, 0.0f)
    , mSpin(0.0f)
    , mTilt(0.0f, 0.0f, 0.0f)
    , mVisible(true)
{
	for (int i = 0; i < 3; i++) {
		mPMotionSpeed[i] = 0.0f;
		mPMotionJoint[i] = -1;
	}
}

// ItemOnyon::Mgr::birth (onyonMgr.cpp:2112-2128) and Onyon::init_pmotions / setupTevRegAnim.
void ShipActor::init(J3DModelData* data, SysShape::AnimMgr* animMgr, Sys::MatBaseAnimation* tev0, Sys::MatBaseAnimation* tev1)
{
	mModel = new SysShape::Model(data, J3DMODEL_ShareDL, 2);
	mModel->getJ3DModel()->newDifferedDisplayList(J3DMDF_DiffColorReg);
	mModel->getJ3DModel()->calc();
	mModel->getJ3DModel()->calcMaterial();
	mModel->getJ3DModel()->makeDL();
	mModel->getJ3DModel()->lock();

	mRootAnimator.mAnimMgr = animMgr;
	mRootAnimator.startAnim(0, nullptr);
	mRootSpeed = 0.0f;

	static const char* const kJoints[3] = { "pmotion1", "pmotion2", "pmotion3" };
	static const f32 kSpeeds[3]         = { 30.0f, 0.0f, 30.0f };
	for (int i = 0; i < 3; i++) {
		mPMotion[i].mAnimMgr = animMgr;
		mPMotion[i].startAnim(i + 1, nullptr);
		mPMotionSpeed[i]       = kSpeeds[i];
		SysShape::Joint* joint = mModel->getJoint((char*)kJoints[i]);
		mPMotionJoint[i]       = joint != nullptr ? joint->mJointIndex : -1;
	}

	mTevAnim[0].start(tev0);
	mTevAnim[1].start(tev1);
}

// MatBaseAnimator::animate multiplies its rate by sys->mDeltaTime; this rate makes it advance 30
// frames per second of scene time `dt` whatever the game's own frame time is.
static f32 materialRate(f32 dt)
{
	return sys->mDeltaTime > 0.0f ? 30.0f * dt / sys->mDeltaTime : 0.0f;
}

void ShipActor::setRootFrame(f32 frame)
{
	mRootFrame = frame;
}

void ShipActor::update(f32 dt)
{
	const f32 rate = materialRate(dt);
	mTevAnim[0].animate(rate);
	mTevAnim[1].animate(rate);
	mModel->getJ3DModel()->calcMaterial();
	mModel->getJ3DModel()->diff();

	if (mRootFrame >= 0.0f && mRootAnimator.mAnimInfo != nullptr) {
		// Driven from outside (the take-off's clip frame): set, not advanced.
		f32 frame = mRootFrame;
		f32 last  = (f32)mRootAnimator.mAnimInfo->mAnm->getTotalFrameCount();
		if (last > 0.0f && frame > last) {
			frame = last;
		}
		mRootAnimator.mTimer = frame;
		mRootAnimator.mAnimInfo->mAnm->setFrame(frame);
	} else {
		mRootAnimator.animate(mRootSpeed * dt);
	}
	if (mRootFrame < 0.0f && mRootAnimator.mAnimInfo != nullptr) {
		// Animator::animate sets whole frames, which judders at 60 fps (most visibly in the fast
		// lift-off); the key animation interpolates between them.
		mRootAnimator.mAnimInfo->mAnm->setFrame(mRootAnimator.mTimer);
	}
	mRootAnimator.setModelCalc(mModel, 0);
	for (int i = 0; i < 3; i++) {
		mPMotion[i].animate(mPMotionSpeed[i] * dt);
		if (mPMotionJoint[i] >= 0) {
			mPMotion[i].setModelCalc(mModel, mPMotionJoint[i]);
		}
	}

	Matrixf mtx;
	Vector3f scale(1.0f, 1.0f, 1.0f);
	Vector3f rotation(mTilt.x, mFaceDir + mSpin, mTilt.z);
	Vector3f pos(mPos.x + mOffset.x, mPos.y + mOffset.y, mPos.z + mOffset.z);
	mtx.makeSRT(scale, rotation, pos);
	PSMTXCopy(mtx.mMatrix.mtxView, mModel->getJ3DModel()->mPosMtx);
	mModel->getJ3DModel()->calc();
}

void ShipActor::entry()
{
	if (mVisible) {
		mModel->getJ3DModel()->entry();
	}
}

void ShipActor::viewCalc()
{
	if (mVisible) {
		mModel->getJ3DModel()->viewCalc();
	}
}

Vector3f ShipActor::getJointPos(const char* name) const
{
	SysShape::Joint* joint = mModel != nullptr ? mModel->getJoint((char*)name) : nullptr;
	if (joint == nullptr) {
		return mPos;
	}
	Matrixf* m = joint->getWorldMatrix();
	return Vector3f(m->mMatrix.mtxView[0][3], m->mMatrix.mtxView[1][3], m->mMatrix.mtxView[2][3]);
}

f32 ShipActor::liftHeight() const
{
	const f32 kBodyRestY = 61.0f; // body_s above the ship's origin when it stands (ufo.bmd: 60)
	f32 lift             = mOffset.y;
	if (mModel != nullptr) {
		f32 body = getJointPos("body_s").y - (mPos.y + mOffset.y) - kBodyRestY;
		if (body > 0.0f) {
			lift += body;
		}
	}
	return lift;
}

// ---- MapActor ----------------------------------------------------------------------------------

MapActor::MapActor()
    : mModel(nullptr)
    , mTexAnimCount(0)
    , mTexAnimators(nullptr)
{
}

// ShapeMapMgr::load (mapMgr.cpp:304-353).
void MapActor::init(J3DModelData* data, int texAnimCount, Sys::MatTexAnimation* texAnims)
{
	mModel = new SysShape::Model(data, J3DMODEL_CreateNewDL, 2);
	mModel->getJ3DModel()->newDifferedDisplayList(0x200);
	mModel->getJ3DModel()->calc();
	mModel->getJ3DModel()->calcMaterial();
	mModel->getJ3DModel()->makeDL();
	mModel->getJ3DModel()->lock();

	mModel->enableMaterialAnim(0);
	J3DModelData* modelData = mModel->getJ3DModel()->getModelData();
	for (u16 i = 0; i < modelData->getMaterialNum() && mShaftCount < 8; i++) {
		const char* name = modelData->getMaterialName()->getName(i);
		J3DShape* shape  = modelData->getMaterialNodePointer(i)->getShape();
		if (name != nullptr && strstr(name, "hikari") != nullptr && shape != nullptr) {
			mShafts[mShaftCount++] = (u16)shape->getIndex();
		}
	}
	mTexAnimCount = texAnimCount;
	if (texAnimCount > 0) {
		mTexAnimators = new Sys::MatLoopAnimator[texAnimCount];
		for (int i = 0; i < texAnimCount; i++) {
			mTexAnimators[i].start(&texAnims[i]);
		}
	}
	mModel->getJ3DModel()->calc();
}

// ShapeMapMgr::do_update (mapMgr.cpp:457-467).
void MapActor::update(f32 dt)
{
	const f32 rate = materialRate(dt);
	for (int i = 0; i < mTexAnimCount; i++) {
		mTexAnimators[i].animate(rate);
	}
	mModel->getJ3DModel()->calcMaterial();
	mModel->getJ3DModel()->diff();
}

void MapActor::entry()
{
	mModel->getJ3DModel()->entry();
}

void MapActor::viewCalc()
{
	mModel->getJ3DModel()->viewCalc();
}

void MapActor::showShafts(bool show)
{
	if (show == mShaftsShown) {
		return;
	}
	mShaftsShown = show;
	for (int i = 0; i < mShaftCount; i++) {
		J3DShapePacket* packet = mModel->getJ3DModel()->getShapePacket(mShafts[i]);
		if (show) {
			packet->offFlag(J3DShape_Hidden);
		} else {
			packet->onFlag(J3DShape_Hidden);
		}
	}
}

// ---- Leaves ------------------------------------------------------------------------------------

namespace {

// The leaf's matrix from happajnt3's world matrix. Several Pikmin clips (Kizuku above all)
// squash and stretch the stem joints by up to 2x on one axis; gameplay never shows a Pikmin that
// large or that close, but here a stretched leaf reads as a giant slab, and the scaled normal
// matrix (GX does not renormalise) swings its lighting from washed out to nearly black. The leaf
// keeps the joint's position and orientation and the actor's own uniform scale; `rot` is the
// orientation alone, for the normals.
const f32 kLeafMinUp = 0.5f; // cos of kLeafMaxTilt, 60 degrees

void leafMatrix(const Matrixf& joint, f32 scale, Mtx rot, Mtx out)
{
	const f32(*m)[4] = joint.mMatrix.mtxView;
	Vector3f y(m[0][1], m[1][1], m[2][1]);
	Vector3f z(m[0][2], m[1][2], m[2][2]);
	f32 ly = sqrtf(y.x * y.x + y.y * y.y + y.z * y.z);
	if (ly < 1.0e-5f) {
		PSMTXIdentity(rot);
		ly = 1.0f;
		y  = Vector3f(0.0f, 1.0f, 0.0f);
	}
	y = Vector3f(y.x / ly, y.y / ly, y.z / ly);
	// The same clips whip the stem over until the leaf hangs upside down in front of the face,
	// showing its unlit underside as a black-green slab. The leaf may lean, but no further than
	// kLeafMaxTilt from upright.
	if (y.y < kLeafMinUp) {
		Vector3f side(y.x, 0.0f, y.z);
		f32 ls = sqrtf(side.x * side.x + side.z * side.z);
		if (ls < 1.0e-4f) {
			side = Vector3f(0.0f, 0.0f, 1.0f);
			ls   = 1.0f;
		}
		f32 sinT = sqrtf(1.0f - kLeafMinUp * kLeafMinUp);
		y        = Vector3f(side.x / ls * sinT, kLeafMinUp, side.z / ls * sinT);
	}
	f32 d = z.x * y.x + z.y * y.y + z.z * y.z;
	z     = Vector3f(z.x - d * y.x, z.y - d * y.y, z.z - d * y.z);
	f32 lz = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z);
	if (lz < 1.0e-5f) {
		// z collapsed onto y: any perpendicular will do.
		z  = fabsf(y.x) < 0.9f ? Vector3f(0.0f, -y.z, y.y) : Vector3f(-y.z, 0.0f, y.x);
		lz = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z);
	}
	z = Vector3f(z.x / lz, z.y / lz, z.z / lz);
	Vector3f x(y.y * z.z - y.z * z.y, y.z * z.x - y.x * z.z, y.x * z.y - y.y * z.x);
	const Vector3f* axes[3] = { &x, &y, &z };
	for (int c = 0; c < 3; c++) {
		rot[0][c] = axes[c]->x;
		rot[1][c] = axes[c]->y;
		rot[2][c] = axes[c]->z;
		out[0][c] = axes[c]->x * scale;
		out[1][c] = axes[c]->y * scale;
		out[2][c] = axes[c]->z * scale;
	}
	for (int r = 0; r < 3; r++) {
		rot[r][3] = 0.0f;
		out[r][3] = m[r][3];
	}
}

// Lighting of a leaf. leaf.bmd is one flat blade whose normals all point along its local +Y, drawn
// with culling off and one-sided lighting: from the side away from the sun, and whenever the idle
// clips turn the blade on edge to the key light, it falls to the ambient term alone, a dark
// brown-green (near black against the bright forest) next to the lime green of the others. A real
// leaf lets the sun through, so the blade is lit from whichever side the sun is on (the normals
// flip with it, which is continuous: both sides are equally dim when the blade is edge-on), and no
// blade turns further than kLeafMinSun from the sun for lighting.
const f32 kLeafMinSun = 0.45f; // cos of 63 degrees

void leafNormalMatrix(const Mtx rot, const Vector3f& sun, Mtx out)
{
	Vector3f n(rot[0][1], rot[1][1], rot[2][1]); // the blade's normal, world
	f32 d = n.x * sun.x + n.y * sun.y + n.z * sun.z;
	f32 s = d < 0.0f ? -1.0f : 1.0f;
	n     = Vector3f(n.x * s, n.y * s, n.z * s);
	d *= s;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			out[r][c] = c < 3 ? rot[r][c] * s : 0.0f;
		}
	}
	if (d >= kLeafMinSun) {
		return;
	}
	Vec axis = { n.y * sun.z - n.z * sun.y, n.z * sun.x - n.x * sun.z, n.x * sun.y - n.y * sun.x };
	f32 len  = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	if (len < 1.0e-5f) {
		return;
	}
	axis.x /= len;
	axis.y /= len;
	axis.z /= len;
	Mtx turn;
	PSMTXRotAxisRad(turn, &axis, acosf(d) - acosf(kLeafMinSun)); // toward the sun
	PSMTXConcat(turn, out, out);
}

} // namespace

// PikiMgr::doSimpleDraw (pikiMgr.cpp:319-363) for our three Pikmin.
void drawLeaves(Assets& assets, Actor** pikmin, int count, Viewport* vp, const Vector3f& sunDir)
{
	Matrixf* view = vp->getMatrix(false);
	for (int i = 0; i < HAPPA_Count; i++) {
		J3DModelData* modelData = assets.mHappaData[i];
		if (modelData == nullptr) {
			continue;
		}
		J3DMaterial* mat = modelData->getJointNodePointer(0)->getMesh();

		j3dSys.setVtxPos(modelData->getVtxPosArray());
		j3dSys.setVtxNrm(modelData->getVtxNrmArray());
		j3dSys.setVtxCol(modelData->getVtxColorArray(0));
		J3DShape::resetVcdVatCache();

		for (; mat != nullptr; mat = mat->mNext) {
			mat->loadSharedDL();
			mat->mShape->loadPreDrawSetting();
			for (int j = 0; j < count; j++) {
				Actor* piki = pikmin[j];
				if (piki == nullptr || !piki->mVisible || piki->mLeafJoint == nullptr || piki->mHappa != i) {
					continue;
				}
				Mtx rot, mtx, nrm;
				leafMatrix(*piki->mLeafJoint->getWorldMatrix(), piki->mScale, rot, mtx);
				PSMTXConcat(view->mMatrix.mtxView, mtx, mtx);
				if (i == HAPPA_Leaf) {
					leafNormalMatrix(rot, sunDir, nrm);
					PSMTXConcat(view->mMatrix.mtxView, nrm, nrm);
				} else {
					PSMTXConcat(view->mMatrix.mtxView, rot, nrm);
				}
				GXLoadPosMtxImm(mtx, 0);
				GXLoadNrmMtxImm(nrm, 0);
				mat->mShape->simpleDrawCache();
			}
		}
	}
}

} // namespace LauncherScene
