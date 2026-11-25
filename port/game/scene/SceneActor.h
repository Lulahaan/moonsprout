// The things drawn in the start-menu scene (docs/launcher/SCENE.md 7.2, 7.5): the four characters,
// the ship and the area map. They are plain objects, not gameplay Creatures: no collision, no AI,
// no generators, nothing that needs a save file. Behaviour (who plays which clip when) belongs to
// the director (SceneDirector.h) and the take-off script (SceneTakeoff.h); an actor only knows how
// to play a clip, walk in a straight line, turn its head and draw itself.
#pragma once

#include "types.h"
#include "Vector3.h"
#include "Matrixf.h"
#include "SysShape/Animator.h"
#include "SysShape/MotionListener.h"
#include "SysShape/KeyEvent.h"
#include "Sys/MatBaseAnimator.h"

struct J3DModelData;
struct Viewport;

namespace SysShape {
struct AnimMgr;
struct Joint;
struct Model;
} // namespace SysShape

namespace LauncherScene {

struct Assets;

enum ActorKind { ACTOR_Olimar = 0, ACTOR_Red, ACTOR_Yellow, ACTOR_Blue, ACTOR_Count };

// Clips of the shared captain / Pikmin set (Game::IPikiAnims). The names the decomp gives them are
// confirmed with the clip viewer (SceneDebug.cpp, MPPM_SCENE_TUNE=1).
enum SceneClip {
	CLIP_Akubi    = 0,  // yawn
	CLIP_Asibumi  = 1,  // step in place (loop)
	CLIP_Chatting = 3,
	CLIP_Fue      = 10, // whistle (Olimar)
	CLIP_Furimuku = 11, // look over the shoulder
	CLIP_Gakkari  = 12, // sigh
	CLIP_Gattu    = 13, // nod / cheer
	CLIP_Getup    = 14,
	CLIP_Growup1  = 18,
	CLIP_Growup2  = 19,
	CLIP_Iraira   = 21, // impatient
	CLIP_Korobu   = 26, // trip
	CLIP_Run2     = 29,
	CLIP_Walk     = 30,
	CLIP_Wait     = 31, // stand (loop)
	CLIP_Kizuku   = 32, // notice
	CLIP_Jump     = 50,
	CLIP_Sagasu   = 53, // look around
	CLIP_Sagasu2  = 54,
	CLIP_Mizunomi = 55, // crouch and drink (Olimar: kneel down)
	CLIP_Suwaru   = 56, // sit (intro, then loop)
	CLIP_Neru     = 57, // sleep (intro, then loop)
};

// Olimar's drawing scale. Gameplay draws captains at 1.3 (navi.cpp:163); the scene keeps 1.0 so the
// cast reads as one group at the idle camera (SCENE.md 7.1). The take-off's beam ride and his
// shadow scale from it.
const f32 kOlimarScale = 1.0f;

// A captain or a Pikmin. One animator drives the whole skeleton (joint 0); joint 4 (the upper
// body, which gameplay blends separately) inherits it, as in FakePiki's far-LOD branch.
struct Actor : public SysShape::MotionListener {
	Actor();

	void init(int kind, SysShape::Model* model, SysShape::AnimMgr* animMgr);

	// Plays a clip from its first frame. Clips with loop markers loop until another clip starts;
	// the others hold their last frame and report isClipDone().
	//
	// Olimar shares the Pikmin's skeleton and clip set, and many of those clips swing his arms
	// through his torso or helmet (SCENE.md 7.3). He only plays the clips of kOlimarClips; the
	// others are mapped here, for every caller (director, take-off, debug tools):
	//   SAGASU -> SAGASU2; GATTU (nod) -> nod() on WAIT; GAKKARI (sigh) -> sigh() on ASIBUMI;
	//   NERU (doze) -> stays in the SUWARU loop with his head hanging (setDoze);
	//   MIZUNOMI -> the kneel: the clip's first frames (down to the crouch, head still up), held,
	//     then back up the same way; the drink itself buries his face in the ground;
	//   anything else -> WAIT, with one log line.
	// A mapped gesture reports isClipDone() when it is over, as the clip would have.
	void startClip(int clip);
	bool isClipDone() const { return mClipDone; }
	int getClip() const { return mClip; }

	// Walks in a straight line to `target` (stage-independent world position) at `speed` units/s,
	// turning toward it; arrival stops the move (isMoving() turns false).
	void moveTo(const Vector3f& target, f32 speed);
	void stopMove() { mMoveSpeed = 0.0f; }
	bool isMoving() const { return mMoveSpeed > 0.0f; }

	// Facing: eased toward the target yaw (radians, J3D convention: 0 = +Z).
	void faceTo(f32 yaw) { mTargetFaceDir = yaw; }
	void faceToward(const Vector3f& point);
	void snapFace(f32 yaw) { mFaceDir = mTargetFaceDir = yaw; }

	// Head turning toward a world point (null: back to straight ahead), clamped to +-60 deg yaw and
	// +-30 deg pitch.
	void lookAt(const Vector3f* point);

	// Head gestures on top of the look-at pitch (Olimar's replacements for clips he cannot play).
	// The bias is eased; the sum with the look-at pitch is clamped to -35..+30 degrees.
	void nod();                // two 12-degree dips of the head over 0.6 s
	void sigh();               // head down 20 degrees for 1.2 s, then back up
	void setDoze(bool on);     // head hanging 25 degrees, swaying 3 degrees at 0.25 Hz
	bool dozing() const { return mDoze; }
	// Olimar only: one arm raised high and waved for `seconds` (rising and lowering included), on
	// top of whatever clip plays. `side` +1 or -1 picks the arm (the sign of its side along the
	// torso's Z axis).
	void wave(f32 seconds, int side = 1);
	bool waving() const { return mWaveTime >= 0.0f; }

	// Per frame: movement, facing, head, clip, matrices. `animate` false holds the pose (tuning).
	void update(f32 dt, bool animate = true);
	void entry();   // queue the model into the current J3D draw buffers
	void viewCalc();

	Vector3f getJointPos(SysShape::Joint* joint) const;
	Vector3f getHeadPos() const { return getJointPos(mHeadJoint); }

	virtual void onKeyEvent(const SysShape::KeyEvent& event); // SysShape::MotionListener

	static bool neckCallback(J3DJoint* joint, int calcStage);
	// Olimar's arms: moves a swung arm's pivot out of the torso, and raises the waving arm
	// (SCENE.md 7.3, "Olimar").
	static bool shoulderCallback(J3DJoint* joint, int calcStage);
	static bool sClipLog; // MPPM_SCENE_CLIPLOG: log every startClip
	static bool sArmFix;  // the shoulder offset is applied (the clip viewer can turn it off)
	// Whether Olimar may play `clip` as authored.
	static bool olimarClipAllowed(int clip);

	int mKind;
	SysShape::Model* mModel;
	SysShape::Animator mAnimator;
	f32 mAnimSpeed; // clip frames per second (30 = authored speed)
	int mClip;
	bool mClipDone;
	bool mVisible;
	int mHappa; // HappaKind; Pikmin only

	Vector3f mPos;
	f32 mFaceDir, mTargetFaceDir, mTurnRate; // radians, radians, 1/s easing rate
	f32 mScale;
	Vector3f mMoveTarget;
	f32 mMoveSpeed;

	Vector3f mLookAt;
	bool mHasLookAt;
	f32 mNeckYaw, mNeckPitch; // radians, eased
	f32 mNeckBias;            // radians, eased toward the gesture's target (nod, sigh, doze)
	f32 mNodTime;             // seconds into nod(), < 0: none
	f32 mSighTime;            // seconds into sigh(), < 0: none
	f32 mGestureLeft;         // a mapped gesture reports isClipDone() when this runs out (< 0: off)
	f32 mDozeTime;
	bool mDoze;
	f32 mWaveTime;   // seconds into wave(), < 0: none
	f32 mKneelTime = -1.0f; // Olimar's kneel (MIZUNOMI held at its crouch), seconds in; < 0: none
	f32 mWaveLength;
	int mWaveSide;

	SysShape::Joint* mHeadJoint;
	SysShape::Joint* mLeafJoint; // happajnt3: where the leaf / antenna sits
	Matrixf mBaseMtx;
};

// Olimar's ship: the authored part motions (pistons, radar, pod) and its two colour animations,
// as gameplay runs them while it stands at the landing site (Onyon::init_pmotions,
// Onyon::do_doAnimation).
struct ShipActor {
	ShipActor();

	void init(J3DModelData* data, SysShape::AnimMgr* animMgr, Sys::MatBaseAnimation* tev0, Sys::MatBaseAnimation* tev1);
	void update(f32 dt);
	// Puts the root clip at `frame` every update instead of advancing it by mRootSpeed (the
	// take-off's clip frame after ignition); a negative frame returns to mRootSpeed.
	void setRootFrame(f32 frame);
	void entry();
	void viewCalc();
	Vector3f getJointPos(const char* name) const;
	// How far the body (joint body_s) is above its resting height, from mOffset and the authored
	// lift-off alike; the section shrinks the ship's shadow with it.
	f32 liftHeight() const;

	SysShape::Model* mModel;
	SysShape::Animator mRootAnimator; // clip 0 (suction) held at frame 0 while idle; the take-off's
	                                  // s02_ufo.bck lift-off (startExAnim) after ignition
	f32 mRootSpeed;                   // frames per second for the root clip
	f32 mRootFrame;                   // >= 0: the root clip is held at this frame (setRootFrame)
	SysShape::Animator mPMotion[3];   // on joints pmotion1-3
	f32 mPMotionSpeed[3];
	int mPMotionJoint[3];
	Sys::MatLoopAnimator mTevAnim[2];
	Vector3f mPos;
	f32 mFaceDir;
	Vector3f mOffset; // added to mPos (the stub take-off lifts the ship with it)
	f32 mSpin;        // extra yaw, radians
	Vector3f mTilt;   // x and z: a lean about the ground point, radians (the take-off's wobble)
	bool mVisible;
};

// The area model with its texture animations (ShapeMapMgr::load / do_update).
struct MapActor {
	MapActor();

	void init(J3DModelData* data, int texAnimCount, Sys::MatTexAnimation* texAnims);
	void update(f32 dt); // texture animations at 30 frames per second of `dt`
	void entry();
	void viewCalc();
	// The map's light shafts (materials named *hikari*): long additive beams that wash the aerial
	// shot out white from above. Hidden there, shown everywhere else.
	void showShafts(bool show);

	SysShape::Model* mModel;
	int mShaftCount = 0;
	u16 mShafts[8];
	bool mShaftsShown = true;
	int mTexAnimCount;
	Sys::MatLoopAnimator* mTexAnimators;
};

// Draws the leaves (or buds / flowers) of the given Pikmin at their happajnt3, the loop of
// PikiMgr::doSimpleDraw. `sunDir` (world, toward the sun) lights the leaf blades from either side.
void drawLeaves(Assets& assets, Actor** pikmin, int count, Viewport* vp, const Vector3f& sunDir);

} // namespace LauncherScene
