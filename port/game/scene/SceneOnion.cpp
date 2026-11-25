// The Onions of the start-menu scene (docs/launcher/SCENE.md 11.2).
#include "SceneOnion.h"

#include "System.h"
#include "JSystem/J3D/J3DAnmLoader.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "JSystem/J3D/J3DJoint.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DMaterial.h"
#include "JSystem/J3D/J3DPacket.h"
#include "JSystem/J3D/J3DMtxCalc.h"
#include "JSystem/J3D/J3DSys.h"
#include "JSystem/J3D/J3DTransform.h"
#include "SysShape/AnimMgr.h"
#include "SysShape/AnimInfo.h"
#include "SysShape/Joint.h"
#include "SysShape/Model.h"

#include <math.h>

namespace LauncherScene {
namespace {

const f32 kPi = 3.14159265f;

// goal.bmd: body_1 rests 109.8 above the Onion's origin; the three legs are kasi1jnt1-3 (joints
// 2-4), kasi3jnt1-3 (5-7) and kass2jnt1 / kasi2jnt2-3 (8-10).
const f32 kBodyRestY            = 109.8f;
const int kLegFirst             = 2;
const int kLegRoot[3]           = { 2, 5, 8 };
const char* const kHipNames[3]  = { "kasi1jnt1", "kasi3jnt1", "kass2jnt1" };
const char* const kKneeNames[3] = { "kasi1jnt2", "kasi3jnt2", "kasi2jnt2" };
const char* const kFootNames[3] = { "kasi1jnt3", "kasi3jnt3", "kasi2jnt3" };
// The foot mesh runs on past its joint along the joint's X axis to the tip on the ground.
const f32 kFootLen = 50.0f;

// Set just before each Onion's calc() (the three share joints and callbacks, as the cast's neck).
OnionActor* sCalc = nullptr;

int legParent(int joint)
{
	return (joint == 2 || joint == 5 || joint == 8) ? 0 : joint - 1;
}

s16 lerpAngle(s16 a, s16 b, f32 t)
{
	s16 d = (s16)(b - a); // the short way round
	return (s16)(a + (s16)((f32)d * t));
}

Vector3f mtxPos(const Matrixf* m)
{
	return Vector3f(m->mMatrix.mtxView[0][3], m->mMatrix.mtxView[1][3], m->mMatrix.mtxView[2][3]);
}

f32 dist(const Vector3f& a, const Vector3f& b)
{
	f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

} // namespace

OnionActor::OnionActor()
    : mColour(0)
    , mModel(nullptr)
    , mFlight(nullptr)
    , mPos(0.0f, 0.0f, 0.0f)
    , mFaceDir(0.0f)
    , mLift(0.0f)
    , mFold(0.0f)
    , mSpinSpeed(0.0f)
    , mSpinAngle(0.0f)
    , mFlowScale(1.0f)
    , mVisible(true)
    , mGulpEnd(false)
    , mFlowJoint(nullptr)
    , mBodyJoint(nullptr)
    , mCanFold(false)
{
	for (int i = 0; i < LEG_Count; i++) {
		mHipJoint[i]  = nullptr;
		mFootJoint[i] = nullptr;
	}
}

void OnionActor::init(int colour, J3DModelData* data, SysShape::AnimMgr* animMgr, Sys::MatBaseAnimation* colourAnim, void* flightBck)
{
	mColour = colour;
	// The three models share materials, and a material-sorted draw buffer merges the packets of
	// one material into the first model's packet, display list and colour included: all three
	// Onions came out red. A packet flagged as changed is never merged.
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		data->getMaterialNodePointer(i)->mDiffFlag |= J3DMatPacket::J3DMP_IsChanged;
	}
	mModel = new SysShape::Model(data, 0, 2); // as ItemOnyon::Mgr::birth: unlocked
	mColourAnim.start(colourAnim);

	mRoot.mAnimMgr = animMgr;
	mRoot.startAnim(0, this); // flowwait

	mFlowJoint = mModel->getJoint((char*)"flow");
	mBodyJoint = mModel->getJoint((char*)"body_1");
	for (int i = 0; i < LEG_Count; i++) {
		mHipJoint[i]  = mModel->getJoint((char*)kHipNames[i]);
		mFootJoint[i] = mModel->getJoint((char*)kFootNames[i]);
	}
	if (mFlowJoint != nullptr) {
		mFlowJoint->mJ3d->setCallBack(flowCallback);
	}

	if (flightBck != nullptr) {
		mFlight = static_cast<J3DAnmTransform*>(J3DAnmLoaderDataBase::load(flightBck));
	}
	// Standing and folded leg poses, read from the clips rather than written down.
	SysShape::AnimInfo* wait = animMgr != nullptr ? animMgr->getAnimByID(0) : nullptr;
	if (mFlight != nullptr && wait != nullptr && wait->mAnm != nullptr && data->getJointNum() >= 11) {
		wait->mAnm->setFrame(0.0f);
		mFlight->setFrame(0.0f);
		for (int i = 0; i < 9; i++) {
			wait->mAnm->getTransform(kLegFirst + i, &mStand[i]);
			mFlight->getTransform(kLegFirst + i, &mFolded[i]);
			data->getJointNodePointer(kLegFirst + i)->setCallBack(legCallback);
		}
		mCanFold = true;
	}
	update(0.0f);
}

void OnionActor::gulp()
{
	mRoot.startAnim(2, this); // in.bck
	mGulpEnd = false;
}

void OnionActor::onKeyEvent(const SysShape::KeyEvent& event)
{
	if (event.mType == KEYEVENT_LOOP_END && mRoot.getAnimIndex() == 2) {
		mGulpEnd = true;
	}
}

void OnionActor::update(f32 dt)
{
	if (mGulpEnd) {
		mGulpEnd = false;
		mRoot.startAnim(0, this);
	}
	mSpinAngle += mSpinSpeed * dt;
	if (mSpinAngle > 360.0f) {
		mSpinAngle = fmodf(mSpinAngle, 360.0f);
	}

	// The clips share one J3DAnmTransform per clip: animate, bind and calc back to back.
	mRoot.animate(30.0f * dt);
	if (mRoot.mAnimInfo != nullptr) {
		mRoot.mAnimInfo->mAnm->setFrame(mRoot.mTimer); // smooth keys, as ShipActor
	}
	mRoot.setModelCalc(mModel, 0);

	Matrixf mtx;
	Vector3f scale(1.0f, 1.0f, 1.0f);
	Vector3f rotation(0.0f, mFaceDir, 0.0f);
	Vector3f pos(mPos.x, mPos.y + mLift, mPos.z);
	mtx.makeSRT(scale, rotation, pos);
	PSMTXCopy(mtx.mMatrix.mtxView, mModel->getJ3DModel()->mPosMtx);

	sCalc = this;
	mModel->getJ3DModel()->calc();
	sCalc = nullptr;
}

// Onyon::changeMaterial for a booted Onion, then ItemMgr's entry: the unlocked model takes its
// material, colour included, at entry, so each Onion sets its own colour right before.
void OnionActor::entry()
{
	if (!mVisible) {
		return;
	}
	if (mColourAnim.mAnimation != nullptr) {
		mColourAnim.setCurrentFrame(mColourAnim.mAnimation->getFrameMax());
		mColourAnim.animate(0.0f);
	}
	mModel->getJ3DModel()->entry();
}

void OnionActor::viewCalc()
{
	if (mVisible) {
		mModel->getJ3DModel()->viewCalc();
	}
}

Vector3f OnionActor::bodyPos() const
{
	return mBodyJoint != nullptr ? mtxPos(mBodyJoint->getWorldMatrix()) : Vector3f(mPos.x, mPos.y + mLift + kBodyRestY, mPos.z);
}

const Matrixf* OnionActor::flowMatrix() const
{
	return mFlowJoint != nullptr ? mFlowJoint->getWorldMatrix() : nullptr;
}

f32 OnionActor::clipY(f32 frame) const
{
	if (mFlight == nullptr) {
		return 0.0f;
	}
	f32 last = (f32)mFlight->getTotalFrameCount() - 1.0f;
	mFlight->setFrame(frame < 0.0f ? 0.0f : (frame > last ? last : frame));
	J3DTransformInfo info;
	mFlight->getTransform(0, &info);
	return info.mTranslation.y - kBodyRestY;
}

// Along the leg's bones: foot tip, ankle joint, knee joint, hip joint.
Vector3f OnionActor::legPoint(int leg, f32 u) const
{
	if (leg < 0 || leg >= LEG_Count || mFootJoint[leg] == nullptr || mHipJoint[leg] == nullptr) {
		return mPos;
	}
	const Matrixf* foot = mFootJoint[leg]->getWorldMatrix();
	SysShape::Joint* kneeJoint = mModel->getJoint((char*)kKneeNames[leg]);
	Vector3f p[4];
	Vector3f ax(foot->mMatrix.mtxView[0][0], foot->mMatrix.mtxView[1][0], foot->mMatrix.mtxView[2][0]);
	p[1] = mtxPos(foot);
	p[0] = Vector3f(p[1].x + ax.x * kFootLen, p[1].y + ax.y * kFootLen, p[1].z + ax.z * kFootLen);
	p[2] = kneeJoint != nullptr ? mtxPos(kneeJoint->getWorldMatrix()) : p[1];
	p[3] = mtxPos(mHipJoint[leg]->getWorldMatrix());
	f32 len[3], total = 0.0f;
	for (int i = 0; i < 3; i++) {
		len[i] = dist(p[i], p[i + 1]);
		total += len[i];
	}
	f32 s = (u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u)) * total;
	for (int i = 0; i < 3; i++) {
		if (s <= len[i] || i == 2) {
			f32 t = len[i] > 0.0f ? s / len[i] : 0.0f;
			t     = t > 1.0f ? 1.0f : t;
			return Vector3f(p[i].x + (p[i + 1].x - p[i].x) * t, p[i].y + (p[i + 1].y - p[i].y) * t, p[i].z + (p[i + 1].z - p[i].z) * t);
		}
		s -= len[i];
	}
	return p[0];
}

int OnionActor::nearestLeg(const Vector3f& from) const
{
	int best   = 0;
	f32 bestD2 = 1.0e30f;
	for (int i = 0; i < LEG_Count; i++) {
		Vector3f tip = legPoint(i, 0.0f);
		f32 dx = tip.x - from.x, dz = tip.z - from.z;
		if (dx * dx + dz * dz < bestD2) {
			bestD2 = dx * dx + dz * dz;
			best   = i;
		}
	}
	return best;
}

// The propeller: spun about its own axis and spread, as the flight clips have it (1250 deg/s,
// scale 1.8 across).
bool OnionActor::flowCallback(J3DJoint* joint, int calcStage)
{
	if (calcStage != 0 || sCalc == nullptr || (sCalc->mSpinAngle == 0.0f && sCalc->mFlowScale == 1.0f)) {
		return false;
	}
	Matrixf* world = J3DMtxCalc::mMtxBuffer->getWorldMatrix(joint->getJntNo());
	Mtx rot, scl, local;
	PSMTXRotRad(rot, 'y', sCalc->mSpinAngle * kPi / 180.0f);
	PSMTXScale(scl, sCalc->mFlowScale, 1.0f, sCalc->mFlowScale);
	PSMTXConcat(rot, scl, local);
	Mtx copy;
	PSMTXCopy(world->mMatrix.mtxView, copy);
	PSMTXConcat(copy, local, world->mMatrix.mtxView);
	PSMTXCopy(world->mMatrix.mtxView, J3DSys::mCurrentMtx);
	return false;
}

// The legs: each joint's local rotation blended from standing to folded, under its parent's
// (already final) world matrix.
bool OnionActor::legCallback(J3DJoint* joint, int calcStage)
{
	if (calcStage != 0 || sCalc == nullptr || !sCalc->mCanFold || sCalc->mFold <= 0.0f) {
		return false;
	}
	int no = joint->getJntNo();
	int i  = no - kLegFirst;
	if (i < 0 || i >= 9) {
		return false;
	}
	f32 t                     = sCalc->mFold > 1.0f ? 1.0f : sCalc->mFold;
	const J3DTransformInfo& a = sCalc->mStand[i];
	const J3DTransformInfo& b = sCalc->mFolded[i];
	Mtx local;
	J3DGetTranslateRotateMtx(lerpAngle(a.mRotation.x, b.mRotation.x, t), lerpAngle(a.mRotation.y, b.mRotation.y, t),
	                         lerpAngle(a.mRotation.z, b.mRotation.z, t), a.mTranslation.x, a.mTranslation.y, a.mTranslation.z, local);
	Matrixf* parent = J3DMtxCalc::mMtxBuffer->getWorldMatrix(legParent(no));
	Matrixf* world  = J3DMtxCalc::mMtxBuffer->getWorldMatrix(no);
	PSMTXConcat(parent->mMatrix.mtxView, local, world->mMatrix.mtxView);
	PSMTXCopy(world->mMatrix.mtxView, J3DSys::mCurrentMtx);
	return false;
}

} // namespace LauncherScene
