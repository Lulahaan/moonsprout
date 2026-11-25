// The take-off that runs when Play is pressed (docs/launcher/SCENE.md 11).
//
// About seventeen and a half seconds in eight shots. Olimar nods to the player and whistles; the
// Pikmin cheer (a hop, a somersault), run to their Onions behind the camera (one trips and
// scrambles up) and climb in; he runs to the ship, turns and waves goodbye, and the tractor beam
// lifts him aboard; the three Onions fold their legs and rise; the ship rocks on its legs and blasts
// off with Nintendo's own lift-off from the day-end movie (s02_ufo.bck, its jets, smoke and leaves
// on the ship's joints). An aerial shot rides up with the convoy as the forest falls away below,
// and a wide shot watches the four craft streak up on their trails into a sky darkening toward
// space, stars coming out. Every cue reaches the launcher in its SCENE.md 11 order; a skip is
// honoured at once in every phase.
//
// The script is two tables: beats (kBeats, on the script clock, which pauses at a gate until the
// cast is ready) and shots (kShots, the camera). After ignition the lift-off clip's frame is the
// clock of everything that flies.
#pragma once

#include "types.h"
#include "Vector3.h"
#include "Matrixf.h"
#include "SysShape/AnimInfo.h"
#include "SceneCamera.h"

struct JAISound;

namespace LauncherScene {

struct SceneWorld;
struct SceneEffects;
struct Actor;
struct OnionActor;

struct Takeoff {
	Takeoff();

	// Main thread, once the scene is built: where the particles and the lift-off clip come from.
	// Recolours the dusk-dark lift-off particles for the daylight scene.
	void init(SceneEffects* effects);

	// variant: P2SceneConfig.takeoff (0 off: fade only; anything else the full take-off).
	void start(SceneWorld& world, int variant);
	// P2_SCENE_CMD_SKIP: at once, whatever the phase, straight to a 0.3 s fade.
	void skip(SceneWorld& world);
	// Per frame while running; returns true once the scene has fully faded (P2_SCENE_DONE is the
	// section's to report, when it ends).
	bool update(SceneWorld& world, f32 dt);

	bool isRunning() const { return mRunning; }
	f32 fadeAlpha() const { return mFade; }  // 0..1 of the end fade, drawn by the section
	f32 scriptTime() const { return mTime; } // the script clock, seconds since Play (MPPM_SCENE_HOLD)
	// The last shot is on: the camera looks up from far off into open sky and the section leaves
	// the map out (its backdrop cylinder would stand in the lower picture).
	bool skyShot() const { return mRunning && mShot == SHOT_Away; }
	bool aerialShot() const { return mRunning && (mShot == SHOT_Aerial || mShot == SHOT_Away); }

	// Shots (SCENE.md 11.3; S2b, the close climb, was added when tuning on screen).
	enum ShotId { SHOT_None = -1, SHOT_TimeToGo, SHOT_ToOnions, SHOT_Climb, SHOT_Beam, SHOT_OnionsLift, SHOT_Ignition, SHOT_Aerial, SHOT_Away, SHOT_Count };

private:
	typedef void (Takeoff::*BeatFn)(SceneWorld&);
	typedef bool (Takeoff::*GateFn)(SceneWorld&) const;
	struct Beat {
		f32 mT;        // script seconds
		BeatFn mFn;
		GateFn mGate;  // null: none. Otherwise the clock waits here until it holds...
		f32 mMaxWait;  // ...or this long
	};
	static const Beat kBeats[];

	// Beats, in script order.
	void beatNod(SceneWorld& world);
	void beatWhistle(SceneWorld& world);
	void beatWhistleEnd(SceneWorld& world);
	void beatHop(SceneWorld& world);
	void beatPikminRun(SceneWorld& world);
	void beatLookLens(SceneWorld& world);
	void beatTurnToShip(SceneWorld& world);
	void beatOlimarRun(SceneWorld& world);
	void beatCutToOnions(SceneWorld& world);
	void beatCutToClimb(SceneWorld& world);
	void beatReady(SceneWorld& world);
	bool gateReady(SceneWorld& world) const;
	void beatCutToBeam(SceneWorld& world);
	void beatWave(SceneWorld& world);
	void beatBoard(SceneWorld& world);
	void beatAboard(SceneWorld& world);
	void beatSpinUp(SceneWorld& world);
	void beatBurp2(SceneWorld& world);
	void beatCutToLift(SceneWorld& world);
	void beatRiseBlue(SceneWorld& world);
	void beatRiseYellow(SceneWorld& world);
	void beatMusicFade(SceneWorld& world);
	void beatCutToIgnition(SceneWorld& world);
	void beatIgnite(SceneWorld& world);
	void flightBeats(SceneWorld& world); // keyed on the clip frame

	void cue(int cue);
	void updateSky(SceneWorld& world);
	f32 convoyHeight(SceneWorld& world, f32& ship, f32& onions) const; // stage-frame heights
	JAISound* sound(u32 id, f32 volume = 1.0f);
	void cut(SceneWorld& world, int shot);
	void beginFade(f32 duration);
	void riseOnion(SceneWorld& world, int colour);
	f32 clipSpeed(f32 frame) const;

	void updateOlimar(SceneWorld& world, f32 dt);
	void updatePikmin(SceneWorld& world, f32 dt);
	void updateOnions(SceneWorld& world, f32 dt);
	void updateShip(SceneWorld& world, f32 dt);
	void updateCamera(SceneWorld& world, f32 dt);
	void shotPose(SceneWorld& world, int shot, f32 t, bool calm, CameraPose& pose, Vector3f& goal, f32& spring);

	bool mRunning;
	int mVariant;
	f32 mTime;     // script clock: seconds since Play, paused at gates
	f32 mElapsed;  // seconds since Play, never paused
	int mBeat;     // next entry of kBeats
	f32 mGateWait; // seconds waited at the current gate
	unsigned mCues; // bit per P2_SCENE_CUE_* already sent
	unsigned mFlightDone; // bit per clip-frame beat already run
	bool mFading;
	f32 mFadeStart, mFadeLength, mFade;
	bool mMusicFaded;
	f32 mAmbientKeep; // 1, or 0 once the S2 cut has removed an area's lens-filling air

	SceneEffects* mEffects;
	SysShape::AnimInfo mLiftoff; // s02_ufo.bck on the ship model (attached at the first take-off)
	bool mLiftoffReady;
	bool mAuthored;  // the ship runs s02_ufo.bck (false: the procedural climb, SCENE.md R4)
	f32 mClipFrame;  // s02_ufo.bck frame: -1 before ignition
	int mBeamFx[3];     // suck a, suck b, particles
	int mLiftoffFx[7];  // jets a-d, smoke in, smoke out, leaves
	int mOnionFx[3][3]; // per Onion: sparkle on the propeller, leaves blown off the ground, trail
	bool mSkySaved;     // the sky colours below hold the area's own (the last shot darkens them)
	u8 mSkyTop[3], mSkyZenith[3];

	// Olimar.
	enum { OL_Stand, OL_Getup, OL_Run, OL_Wait, OL_Ride, OL_Aboard };
	int mOlimarState;
	Vector3f mBoardFrom, mBoardCtrl;
	f32 mNodFrom;     // script time the nod started (< 0: no nod)
	bool mLookLens;   // looking at the player
	JAISound** mWhistle; // the whistle's pool slot: JAudio clears it if the sound ends first

	// Pikmin: to the nearest foot of their own Onion, up the leg, in. One trips on the way.
	enum { PK_Stand, PK_Run, PK_Trip, PK_Getup, PK_Climb, PK_Aboard };
	struct Piki {
		int mState;
		int mLeg;
		int mAfterGetup; // clip to play once CLIP_Getup ends (-1 none)
		Vector3f mVia;   // first leg of the run (== target when no detour)
		bool mHasVia;
		f32 mTimer;
		f32 mNoticeAt;   // script time it notices Olimar (< 0: done)
		f32 mHopAt;      // script time of its hop (< 0: done)
		int mHopClip;    // the cheer: a hop or a somersault
		Vector3f mRunFrom;
		bool mTrips;     // trips once it has covered kTripAt of its run
	};
	Piki mPiki[3];
	int mVoices; // cheers played at the hop (at most 2)

	// Onions: rise start (script time, < 0 not yet), spin-up start.
	f32 mRiseAt[3];
	f32 mSpinUpAt;
	f32 mPistonAt; // script time the pistons start to pump faster (< 0: not yet)

	// Camera.
	int mShot;
	f32 mShotTime;   // seconds since the cut (real)
	f32 mShotFrame;  // clip frame at the cut (flight shots)
	f32 mShotStartHeight; // the convoy's middle height at the cut (stage frame)
	CameraPose mShotStart; // the shot's calm pose (Reduce Motion holds it for the whole shot)
	Vector3f mCamTarget, mCamTargetVel; // look-at spring, stage frame
	Vector3f mIdleEye, mIdleTarget;     // the idle pose at Play
	f32 mIdleFov;
	f32 mShake; // amplitude now
};

} // namespace LauncherScene
