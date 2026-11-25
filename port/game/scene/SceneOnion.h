// The three Onions of the start-menu scene (docs/launcher/SCENE.md 11.2). They stand round the
// landing site the whole time, out of the idle picture, and lift off with the ship: the Pikmin
// climb their legs and are swallowed, the propeller spins up, the legs fold and they rise along the
// day-end movie's own flight heights (s02_onyonr/y/b.bck).
//
// Gameplay's recipe (ItemOnyon::Mgr::birth, Onyon::changeMaterial, ItemMgr entry): one model data
// for all three, each Onion an unlocked SysShape::Model whose colour animation is set to its last
// frame just before its entry(). The flight clips are only sampled for their root height: bound to
// the root they would also carry the movie's X/Z and turn the Onion twice. Propeller and legs are
// driven by joint callbacks instead.
#pragma once

#include "types.h"
#include "Vector3.h"
#include "Matrixf.h"
#include "SysShape/Animator.h"
#include "SysShape/MotionListener.h"
#include "SysShape/KeyEvent.h"
#include "Sys/MatBaseAnimator.h"
#include "JSystem/J3D/J3DTypes.h"

struct J3DModelData;
struct J3DJoint;
struct J3DAnmTransform;

namespace SysShape {
struct AnimMgr;
struct Joint;
struct Model;
} // namespace SysShape

namespace LauncherScene {

struct OnionActor : public SysShape::MotionListener {
	enum { LEG_Count = 3 };

	OnionActor();

	// `colour` as PikminColour; `flightBck` the colour's s02_onyon*.bck (null: no flight heights).
	void init(int colour, J3DModelData* data, SysShape::AnimMgr* animMgr, Sys::MatBaseAnimation* colourAnim, void* flightBck);
	void update(f32 dt); // clip, callbacks' statics, matrices
	void entry();        // colour, then the draw entry (the colour is captured per model here)
	void viewCalc();

	void gulp(); // in.bck once (a Pikmin went in), then back to flowwait

	// A point on leg `leg`: u 0 is the foot tip on the ground, 1 the hip (where the leg meets the
	// body). World space, from this frame's matrices.
	Vector3f legPoint(int leg, f32 u) const;
	int nearestLeg(const Vector3f& from) const;
	// The flight clip's root height at clip frame `frame`, less the rest height of the body joint:
	// how far above the ground the movie has the Onion. 0 without a clip.
	f32 clipY(f32 frame) const;
	bool hasFlight() const { return mFlight != nullptr; }
	Vector3f bodyPos() const;          // body_1, world
	const Matrixf* flowMatrix() const; // the propeller joint's world matrix (emitters follow it)

	virtual void onKeyEvent(const SysShape::KeyEvent& event); // SysShape::MotionListener

	int mColour;
	SysShape::Model* mModel;
	SysShape::Animator mRoot;
	Sys::MatLoopAnimator mColourAnim;
	J3DAnmTransform* mFlight;

	Vector3f mPos;   // ground position, world
	f32 mFaceDir;    // radians
	f32 mLift;       // added to the height
	f32 mFold;       // legs: 0 standing .. 1 folded for flight
	f32 mSpinSpeed;  // propeller, degrees per second
	f32 mSpinAngle;  // degrees
	f32 mFlowScale;  // propeller's spread (1 standing, 1.8 in flight)
	bool mVisible;
	bool mGulpEnd;   // in.bck reached its loop end: back to flowwait at the next update

	SysShape::Joint* mFlowJoint;
	SysShape::Joint* mBodyJoint;
	SysShape::Joint* mHipJoint[LEG_Count];
	SysShape::Joint* mFootJoint[LEG_Count];

	// Leg joints 2..10: standing pose (flowwait frame 0) and folded pose (the flight clip frame 0).
	J3DTransformInfo mStand[9];
	J3DTransformInfo mFolded[9];
	bool mCanFold;

private:
	static bool flowCallback(J3DJoint* joint, int calcStage);
	static bool legCallback(J3DJoint* joint, int calcStage);
};

} // namespace LauncherScene
