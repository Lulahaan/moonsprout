// The take-off (docs/launcher/SCENE.md 11). "T" below is the script clock, seconds from Play; it
// pauses at gates. The ship's lift-off, its particles and their timing are the day-end movie's
// (disc:user/Mukki/movie/s02_dayend_result: s02_ufo.bck, s02_onyon*.bck, efx.jpc, demo.stb), the
// beam and the burp the sunset boarding's (s01_dayend). The STB starts every s02 emitter and the
// 0x300E roar on clip frame 0 and the leaf rustle 0x86F on frame 41; the s01 burps follow the beam
// by 14 and 46 frames. Everything runs on scene seconds, so it plays at the same speed at any
// frame rate.
#include "SceneTakeoff.h"
#include "SceneWorld.h"
#include "SceneAudio.h"
#include "SceneEffects.h"
#include "SceneOnion.h"
#include "SceneAssets.h"
#include "SceneLight.h"

#include "Dolphin/rand.h"
#include "PSSystem/PSSystemIF.h"
#include "JSystem/JAudio/JAI/JAISound.h"
#include "SoundID.h"
#include "SysShape/Joint.h"
#include "SysShape/Model.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DJoint.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "JSystem/JParticle/JPAEmitter.h"

#include <math.h>

namespace LauncherScene {
namespace {

const f32 kPi  = 3.14159265f;
const f32 kTau = 6.2831853f;
const f32 kDeg = kPi / 180.0f;

// The timeline (SCENE.md 11.1). Times of beats live in kBeats; these are the ones other code
// reads too.
const f32 T_Nod      = 0.35f;
const f32 T_Whistle  = 1.15f;
const f32 T_Cheer    = 1.95f; // the whistle ends, the Pikmin cheer
const f32 T_PikiRun  = 2.75f;
const f32 T_OlimarRun = 3.30f;
const f32 T_ToOnions = 3.60f;
const f32 T_Climb    = 4.70f;
const f32 T_Ready    = 5.90f;
const f32 T_Beam     = 6.00f;
const f32 kWhistleTurn = 0.70f; // radians (40 deg): how far Olimar turns from the lens to whistle
const f32 T_Wave     = 6.15f; // he turns to the lens and waves goodbye
const f32 kWaveTime  = 1.25f;
const f32 T_Board    = 7.50f;
const f32 T_Carry    = 7.70f; // the hop's own rise, then the beam takes him
const f32 T_Aboard   = 9.00f;
const f32 T_SpinUp   = 9.10f;
const f32 T_Burp2    = 9.35f;
const f32 kRiseRed    = 9.80f;
const f32 kRiseBlue   = 9.95f;
const f32 kRiseYellow = 10.10f;
const f32 T_MusicFade = 11.05f;
const f32 T_Anticip  = 11.10f; // the ignition shot: the ship trembles and rocks on its legs
const f32 T_Ignite   = 12.45f; // s02_ufo.bck frame 0

// Clip frames of the flight (s02_ufo.bck; the Onions' clips run in step). The ship climbs about 24
// units a frame from frame 12; the Onions start 250 units up, fall behind and catch up by 175.
const f32 F_Liftoff   = 8.0f;   // the body leaves the ground
const f32 F_Aerial    = 20.0f;  // the ship has left the top of the ignition shot
const f32 F_Leaf      = 41.0f;  // PSSE_PL_S02_SCATTER_LEAF in the STB
const f32 F_SmokeEnd  = 48.0f;  // the launch cloud and the leaves stop growing once the ship is away
const f32 F_Away      = 70.0f;
const f32 F_Exit      = 120.0f; // EXIT_FRAME
const f32 F_End       = 150.0f; // about where the fade ends
const f32 F_WobbleEnd = 40.0f;  // the rocking has died down

const f32 kFadeLength = 0.75f;
const f32 kOffFade    = 0.40f;
const f32 kSkipFade   = 0.30f;
const f32 kMusicFade  = 2.0f;

const int kClipClimb   = 20;     // HNOBORU: climbing (FakePiki.h)
const f32 kPikiRun     = 120.0f; // RUN2 at its authored speed
const f32 kClimbTime   = 1.0f;
const f32 kClimbShrink = 0.6f;
const f32 kOlimarRunMin = 110.0f;
const f32 kOlimarRunMax = 160.0f;
const f32 kRideTime    = 1.3f;
const f32 kRideShrink  = 0.3f;
const f32 kRideTurns   = 1.5f;
const f32 kVoiceVolume = 0.3f; // Pikmin voices as system sounds have no falloff
const int kClipRollJump = 35;    // the somersault of a thrown Pikmin
const f32 kTripAt      = 0.55f;  // share of its run after which the tripping Pikmin falls
const int kTripper     = 1;      // yellow: its run ends in the middle of the onion shot
const f32 kGetupSpeed  = 38.0f;  // GETUP a little brisk: it scrambles up
const f32 kTripTime    = 0.55f;  // longest it lies there
const f32 kGetupTime   = 0.35f;

// The ship before and at lift-off: it trembles (Y, 9 Hz) and rocks on its legs (a lean about the
// ground point on two incommensurate rates), most as the body leaves the ground, dying down as it
// climbs.
const f32 kTrembleAmp  = 0.4f;
const f32 kRockDegPre  = 0.7f;  // peak lean while it builds up
const f32 kRockDegLift = 2.2f;  // at lift-off
const f32 kRockHzX     = 2.6f;
const f32 kRockHzZ     = 3.3f;

// Onions in flight (SCENE.md 11.2): propeller speeds in degrees per second, spread, fold.
const f32 kSpinIdle   = 800.0f;
const f32 kSpinFlight = 1250.0f;
const f32 kFlowSpread = 1.8f;
const f32 kFoldDelay  = 0.2f;
const f32 kFoldTime   = 0.9f;

// The procedural climb used when s02_ufo.bck is missing (SCENE.md R4 fallback).
const f32 kClimbAccel = 380.0f;
const f32 kSpinClimb  = 3.0f;

// The day-end STBs' particle objects: emitter id, draw group, ship joint; how they are placed. The
// plumes and the leaves stay where the ship stood, as the launch cloud and the leaves torn off the
// ground, drawn smaller; the jets ride the ship, the exhaust trail (dusk-dark) drawn thinner.
struct DemoEmitter {
	u16 mId;
	u8 mGroup;
	const char* mJoint;
	f32 mScale;
	bool mGrounded; // stays where the joint was at the start instead of following it
	u8 mAlpha;      // 255: as authored
};
const DemoEmitter kLiftoffFx[] = {
	{ 1, 6, "jet_eff", 0.55f, false, 110 }, // ufojet_a: the exhaust trail, thinner
	{ 2, 7, "jet_eff", 1.0f, false, 255 },  // ufojet_b
	{ 3, 8, "jet_eff", 1.0f, false, 255 },  // ufojet_c
	{ 4, 7, "jet_eff", 1.0f, false, 255 },  // ufojet_d
	{ 5, 6, "in1", 0.6f, true, 120 },       // ufosmoke_in
	{ 6, 6, "out", 0.6f, true, 120 },       // ufosmake_out
	{ 7, 7, "body_s", 0.5f, true, 255 },    // ufo_leaf
};
const u8 kSmokeEarlyAlpha = 60; // the plumes before ignition (the ship trembles)
const u8 kAwayJetAlpha    = 110; // the jets' glow in the last shot
const DemoEmitter kBeamSuckA = { 0, 7, "start1", 1.0f, true, 255 };
const DemoEmitter kBeamSuckB = { 1, 7, "start1", 1.0f, true, 255 };
const DemoEmitter kBeamPtcl  = { 2, 7, "pmotion3", 1.0f, false, 255 };
const DemoEmitter kBurpA     = { 3, 7, "pmotion3", 1.0f, false, 255 };
const DemoEmitter kBurpB     = { 4, 7, "pmotion3", 1.0f, false, 255 };
// The Onions' own emitters of the s02 set: the sparkle on the propeller, the leaves under it.
const u16 kOnionSparkle      = 0;
const u8 kOnionSparkleGroup  = 8;
const u16 kOnionLeaves       = 8;
const u8 kOnionLeavesGroup   = 7;
// The Onions' vapour trail: the ship's own exhaust trail (ufojet_a) on each Onion's body from
// ignition, so all four craft draw a line up the sky.
const DemoEmitter kOnionTrail = { 1, 6, nullptr, 0.42f, false, 120 };

// Colours of the s02 set (emitter id, primary, environment): the movie's sunset silhouettes
// recoloured for daylight (sunlit olive leaves, tan dust, light grey exhaust).
struct DemoTint {
	u16 mId;
	u32 mPrm;
	u32 mEnv;
};
const DemoTint kLiftoffTint[] = {
	{ 1, 0xC9C6C0, 0xE4E1DA }, // ufojet_a: the exhaust trail
	{ 5, 0xB4A78E, 0xD6CCB6 }, // ufosmoke_in
	{ 6, 0xB4A78E, 0xD6CCB6 }, // ufosmake_out
	{ 7, 0x5F7A2E, 0x5F7A2E }, // ufo_leaf
	{ 8, 0x5F7A2E, 0x5F7A2E }, // its twin, under the Onions
};

const Vector3f kStart1(0.0f, 0.0f, 137.0f); // ship joint start1, stage frame (the joint wins when present)
const Vector3f kCockpit(0.0f, 62.0f, 17.0f);

// ---- Shots (SCENE.md 11.3), stage frame --------------------------------------------------------
//
// Ground shots keep the top edge of the picture at or below kShotSkyEdge: the maps' backdrop is a
// cylinder made for gameplay's downward cameras and ends some 46 degrees above this eye's
// horizon. The aerial shot looks down on the map from above the convoy; the last shot looks up
// from far off into open sky with the map left out.
// Per ground shot, the highest the top edge may go (degrees above the horizon). Facing +Z, away
// from the idle view, the backdrop's rim is low: at 4:3 (Vert+, a taller lens) it showed above S2.
const f32 kShotSkyEdge[] = { 40.0f, 20.0f, 20.0f, 40.0f, 36.0f, 40.0f };
const f32 kMinAspect    = 1.6f;  // below it ground shots keep the 16:10 horizontal view

struct ShotDef {
	Vector3f mEye;
	Vector3f mTarget;
	f32 mFov;
	f32 mCalmFov;
};
// Tuned from screenshots with MPPM_SCENE_HOLD and MPPM_SCENE_CAM (SCENE.md 11.3). Looking toward
// +Z (away from the idle view) the backdrop's rim sits low, 15-20 degrees up: the shots that face
// that way (S2, S2b) stay level or look down; S4 turns round and looks back past the Onions at the
// ship, where the forest wall stands tall. The aerial shot's eye and target heights are relative
// to the convoy (convoyHeight); the last shot's target is the convoy's middle.
const ShotDef kShots[Takeoff::SHOT_Count] = {
	{ Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 0.0f), 0.0f, 0.0f },              // S1: from the idle pose
	{ Vector3f(60.0f, 40.0f, 40.0f), Vector3f(-40.0f, 30.0f, 430.0f), 50.0f, 50.0f },    // S2 to the Onions
	{ Vector3f(100.0f, 45.0f, 180.0f), Vector3f(-70.0f, 46.0f, 470.0f), 44.0f, 44.0f }, // S2b up the legs
	{ Vector3f(-38.0f, 16.0f, 200.0f), Vector3f(-6.0f, 17.0f, 120.0f), 40.0f, 40.0f },   // S3 the beam
	{ Vector3f(70.0f, 40.0f, 800.0f), Vector3f(0.0f, 100.0f, 250.0f), 48.0f, 48.0f },   // S4 Onions lift (all three apart)
	{ Vector3f(-110.0f, 4.0f, 240.0f), Vector3f(0.0f, 75.0f, 0.0f), 44.0f, 44.0f },      // S5 ignition
	{ Vector3f(-170.0f, 600.0f, -300.0f), Vector3f(40.0f, 0.0f, 300.0f), 52.0f, 52.0f }, // S6 aerial
	{ Vector3f(-800.0f, 450.0f, -200.0f), Vector3f(0.0f, 90.0f, 250.0f), 40.0f, 40.0f },  // S7 away
};
const f32 kClimbPush     = 0.06f; // S2b creeps this share of the way toward its target (both Onions stay in)
const f32 kClimbPushTime = 1.2f;
const Vector3f kS1EyeMove(-4.0f, 1.0f, -14.0f);
const f32 kS1MoveTime   = 2.4f;
const f32 kS1FovEnd     = 33.0f / 36.0f; // of the idle lens
const f32 kS1Lean       = 0.35f;         // toward Olimar's head
const f32 kS2Drift      = -10.0f;
const f32 kS2DriftTime  = 2.1f;
const f32 kS3Lean       = 0.75f;         // toward Olimar during the ride
const f32 kS3RideFov    = 50.0f;         // the lens opens as he rises to the cockpit
const f32 kS4RiseAim    = 0.5f;          // of the Onions' mean lift
// S6: the eye rides up with the convoy a little slower than it climbs (the ship rises toward the
// lens, the ground falls away), circling a few degrees round it.
const f32 kS6Follow     = 0.82f;
const f32 kS6OrbitDeg   = 14.0f;
const f32 kS6FovEnd     = 46.0f;
// S7: the eye climbs too, slowly; the lens closes in a little; the sky darkens toward space.
const f32 kS7Climb      = 0.35f;
const f32 kS7FovEnd     = 30.0f;
const f32 kS7Follow     = 0.7f;  // share of the convoy's climb the aim follows
const u8 kSpaceZenith[3] = { 8, 14, 44 };
const u8 kSpaceTop[3]    = { 52, 86, 160 };

// Sounds still playing at the end fade with the picture instead of being cut when the section
// deletes its sound scene. JAudio clears a handle when its sound ends, so the handles live in
// static storage, not in the section's heap.
const int kSoundPool = 16;
JAISound* sSounds[kSoundPool];

f32 clamp01(f32 x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

f32 smoothstep(f32 x)
{
	x = clamp01(x);
	return x * x * (3.0f - 2.0f * x);
}

Vector3f lerpV(const Vector3f& a, const Vector3f& b, f32 t)
{
	return Vector3f(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

Vector3f bezier(const Vector3f& a, const Vector3f& m, const Vector3f& b, f32 t)
{
	return lerpV(lerpV(a, m, t), lerpV(m, b, t), t);
}

f32 flatDist(const Vector3f& a, const Vector3f& b)
{
	f32 dx = a.x - b.x, dz = a.z - b.z;
	return sqrtf(dx * dx + dz * dz);
}

SysShape::Joint* shipJoint(ShipActor* ship, const char* name)
{
	return ship != nullptr && ship->mModel != nullptr ? ship->mModel->getJoint((char*)name) : nullptr;
}

const Matrixf* shipJointMtx(ShipActor* ship, const char* name)
{
	SysShape::Joint* joint = shipJoint(ship, name);
	return joint != nullptr ? joint->getWorldMatrix() : nullptr;
}

int startFx(SceneEffects* fx, int set, const DemoEmitter& e, ShipActor* ship, f32 seconds = 0.0f)
{
	const Matrixf* mtx = shipJointMtx(ship, e.mJoint);
	if (fx == nullptr || mtx == nullptr) {
		return -1;
	}
	int h = e.mGrounded ? fx->startDemo(set, e.mId, e.mGroup, nullptr, mtx, seconds, e.mScale)
	                    : fx->startDemo(set, e.mId, e.mGroup, mtx, nullptr, seconds, e.mScale);
	if (e.mAlpha != 255) {
		fx->setAlpha(h, e.mAlpha);
	}
	return h;
}

// Vertical lens that keeps the horizontal view of kMinAspect on narrower pictures (Vert+).
f32 groundFov(f32 fov, f32 aspect)
{
	if (aspect >= kMinAspect) {
		return fov;
	}
	return 2.0f * atanf(tanf(fov * 0.5f * kDeg) * kMinAspect / aspect) / kDeg;
}

} // namespace

// The script (SCENE.md 11.1). Times are script seconds; a gate holds the clock at its beat until
// it is satisfied or its wait runs out.
const Takeoff::Beat Takeoff::kBeats[] = {
	{ T_Nod, &Takeoff::beatNod, nullptr, 0.0f },
	{ T_Whistle, &Takeoff::beatWhistle, nullptr, 0.0f },
	{ T_Cheer, &Takeoff::beatWhistleEnd, nullptr, 0.0f },
	{ T_Cheer, &Takeoff::beatHop, nullptr, 0.0f },
	{ T_PikiRun, &Takeoff::beatPikminRun, nullptr, 0.0f },
	{ 2.80f, &Takeoff::beatLookLens, nullptr, 0.0f },
	{ 3.15f, &Takeoff::beatTurnToShip, nullptr, 0.0f },
	{ T_OlimarRun, &Takeoff::beatOlimarRun, nullptr, 0.0f },
	{ T_ToOnions, &Takeoff::beatCutToOnions, nullptr, 0.0f },
	{ T_Climb, &Takeoff::beatCutToClimb, nullptr, 0.0f },
	{ T_Ready, &Takeoff::beatReady, &Takeoff::gateReady, 1.0f },
	{ T_Beam, &Takeoff::beatCutToBeam, nullptr, 0.0f },
	{ T_Wave, &Takeoff::beatWave, nullptr, 0.0f },
	{ T_Board, &Takeoff::beatBoard, nullptr, 0.0f },
	{ T_Aboard, &Takeoff::beatAboard, nullptr, 0.0f },
	{ T_SpinUp, &Takeoff::beatSpinUp, nullptr, 0.0f },
	{ T_Burp2, &Takeoff::beatBurp2, nullptr, 0.0f },
	{ kRiseRed, &Takeoff::beatCutToLift, nullptr, 0.0f },
	{ kRiseBlue, &Takeoff::beatRiseBlue, nullptr, 0.0f },
	{ kRiseYellow, &Takeoff::beatRiseYellow, nullptr, 0.0f },
	{ T_MusicFade, &Takeoff::beatMusicFade, nullptr, 0.0f },
	{ T_Anticip, &Takeoff::beatCutToIgnition, nullptr, 0.0f },
	{ T_Ignite, &Takeoff::beatIgnite, nullptr, 0.0f },
};

Takeoff::Takeoff()
    : mRunning(false)
    , mVariant(2)
    , mTime(0.0f)
    , mElapsed(0.0f)
    , mBeat(0)
    , mGateWait(0.0f)
    , mCues(0)
    , mFlightDone(0)
    , mFading(false)
    , mFadeStart(0.0f)
    , mFadeLength(kFadeLength)
    , mFade(0.0f)
    , mMusicFaded(false)
    , mAmbientKeep(1.0f)
    , mEffects(nullptr)
    , mLiftoffReady(false)
    , mAuthored(false)
    , mClipFrame(-1.0f)
    , mOlimarState(OL_Stand)
    , mBoardFrom(0.0f, 0.0f, 0.0f)
    , mBoardCtrl(0.0f, 0.0f, 0.0f)
    , mNodFrom(-1.0f)
    , mLookLens(false)
    , mWhistle(nullptr)
    , mVoices(0)
    , mSpinUpAt(-1.0f)
    , mPistonAt(-1.0f)
    , mShot(SHOT_None)
    , mShotTime(0.0f)
    , mShotFrame(0.0f)
    , mShotStartHeight(0.0f)
    , mCamTarget(0.0f, 0.0f, 0.0f)
    , mCamTargetVel(0.0f, 0.0f, 0.0f)
    , mIdleEye(0.0f, 0.0f, 0.0f)
    , mIdleTarget(0.0f, 0.0f, 0.0f)
    , mIdleFov(36.0f)
    , mShake(0.0f)
{
	mSkySaved = false;
	for (int i = 0; i < 3; i++) {
		mBeamFx[i]     = -1;
		mOnionFx[i][0] = -1;
		mOnionFx[i][1] = -1;
		mOnionFx[i][2] = -1;
		mRiseAt[i]     = -1.0f;
		mSkyTop[i] = mSkyZenith[i] = 0;
	}
	for (int i = 0; i < 7; i++) {
		mLiftoffFx[i] = -1;
	}
}

void Takeoff::init(SceneEffects* effects)
{
	mEffects  = effects;
	mSkySaved = false; // a new section (another area) has its own sky colours
	if (mEffects != nullptr) {
		for (u32 i = 0; i < sizeof(kLiftoffTint) / sizeof(kLiftoffTint[0]); i++) {
			mEffects->tint(DEMO_Liftoff, kLiftoffTint[i].mId, kLiftoffTint[i].mPrm, kLiftoffTint[i].mEnv);
		}
	}
}

void Takeoff::cue(int c)
{
	unsigned bit = 1u << c;
	if (!(mCues & bit)) {
		mCues |= bit;
		p2_launcher_scene_cue(c);
	}
}

// Without a position, as the system sounds of the menus (SCENE.md 9, R7). Every sound goes into
// the pool the end fade stops; none starts under the fade. A sound whose wave the title's sound
// scene did not load stays silent.
JAISound* Takeoff::sound(u32 id, f32 volume)
{
	if (PSSystem::spSysIF == nullptr || mFading) {
		return nullptr;
	}
	JAISound** slot = nullptr;
	for (int i = 0; i < kSoundPool; i++) {
		if (sSounds[i] == nullptr) {
			slot = &sSounds[i];
			break;
		}
	}
	if (slot == nullptr) {
		PSSystem::spSysIF->playSystemSe(id, 0);
		return nullptr;
	}
	PSSystem::spSysIF->playSystemSe(id, slot, 0);
	if (*slot != nullptr && volume != 1.0f) {
		(*slot)->setVolume(volume, 0, SOUNDPARAM_Direct);
	}
	return *slot;
}

void Takeoff::beginFade(f32 duration)
{
	if (mFading) {
		return;
	}
	cue(P2_SCENE_CUE_EXIT_FRAME);
	mCues       = ~0u; // the launcher hears nothing after EXIT_FRAME
	mFading     = true;
	mFadeStart  = mElapsed;
	mFadeLength = duration;
	p2_launcher_scene_state(P2_SCENE_FADING);
	u32 frames = (u32)(duration * 60.0f);
	for (int i = 0; i < kSoundPool; i++) {
		if (sSounds[i] != nullptr) {
			sSounds[i]->stop(frames);
		}
	}
}

// 20 frames a second until the body is off the ground, ramping to the authored 30 by frame 28: the
// blast-off gets a quarter second more weight and every keyed effect stays in step with it.
f32 Takeoff::clipSpeed(f32 frame) const
{
	if (frame < 10.0f) {
		return 20.0f;
	}
	if (frame < 28.0f) {
		return 20.0f + 10.0f * (frame - 10.0f) / 18.0f;
	}
	return 30.0f;
}

void Takeoff::start(SceneWorld& world, int variant)
{
	// The launcher may have changed these since the scene loaded (SCENE.md 12).
	P2SceneConfig config = world.mConfig;
	p2_launcher_scene_config(&config);
	world.mConfig.takeoff      = config.takeoff;
	world.mConfig.reduceMotion = config.reduceMotion;
	world.mConfig.fadeColor    = config.fadeColor;
	if (variant != 0) {
		variant = config.takeoff != 0 ? 2 : 0; // Short is gone: anything on is the full take-off
	}

	mRunning    = true;
	mTime       = 0.0f;
	mElapsed    = 0.0f;
	mBeat       = 0;
	mGateWait   = 0.0f;
	mCues       = 0;
	mFlightDone = 0;
	mFading     = false;
	mFade       = 0.0f;
	mMusicFaded = false;
	mAmbientKeep = 1.0f;
	mClipFrame  = -1.0f;
	mShot       = SHOT_None;
	mShake      = 0.0f;
	mVoices     = 0;
	mSpinUpAt   = -1.0f;
	mPistonAt   = -1.0f;
	mNodFrom    = -1.0f;
	mLookLens   = true;
	mWhistle    = nullptr;
	for (int i = 0; i < 3; i++) {
		mRiseAt[i] = -1.0f;
		for (int k = 0; k < 3; k++) {
			mOnionFx[i][k] = -1;
		}
	}
	if (world.mLight != nullptr && !mSkySaved) {
		// The last shot darkens the sky toward space (updateSky); the section draws the sky from
		// these colours, set once from the area's light when the scene loaded.
		mSkySaved     = true;
		mSkyTop[0]    = world.mLight->mSkyTop.r;
		mSkyTop[1]    = world.mLight->mSkyTop.g;
		mSkyTop[2]    = world.mLight->mSkyTop.b;
		mSkyZenith[0] = world.mLight->mSkyZenith.r;
		mSkyZenith[1] = world.mLight->mSkyZenith.g;
		mSkyZenith[2] = world.mLight->mSkyZenith.b;
	}

	// The lift-off clip on the ship's own joints. The clip's header counts 26 joints and ufo.bmd
	// has 25 (SCENE.md R4): the tracks line up with the model's joints one to one and the 26th is
	// never asked for.
	ShipActor* ship = world.mShip;
	if (!mLiftoffReady && ship != nullptr && mEffects != nullptr && mEffects->liftoffAnim() != nullptr) {
		mLiftoff.attach(ship->mModel->getJ3DModel()->getModelData(), mEffects->liftoffAnim());
		mLiftoffReady = mLiftoff.mAnm != nullptr && mLiftoff.mCalc != nullptr;
	}
	mAuthored = mLiftoffReady;
	if (variant != 0) {
		bool onions = true;
		for (int i = 0; i < 3; i++) {
			onions = onions && world.mOnions[i] != nullptr && world.mOnions[i]->hasFlight();
		}
		if (!onions || !mAuthored) {
			OSReport("scene: the take-off needs the Onions and s02_ufo.bck; fading out instead\n");
			variant = 0;
		}
	}
	mVariant = variant;

	p2_launcher_scene_state(P2_SCENE_TAKEOFF);
	cue(P2_SCENE_CUE_LAUNCH_BEGIN);
	if (variant == 0) {
		SceneAudio::fadeOut(kOffFade);
		mMusicFaded = true;
		beginFade(kOffFade);
		return;
	}

	SceneCamera* cam = world.mCamera;
	if (cam != nullptr) {
		CameraPose idle = cam->idlePose();
		mIdleEye        = idle.mPos;
		mIdleTarget     = idle.mTarget;
		mIdleFov        = idle.mFovY;
	}

	// Everyone turns to the action: Olimar to the player, the Pikmin to him. Whoever sits or
	// sleeps gets up first, briskly.
	Vector3f eye = world.cameraEye();
	for (int i = 0; i < ACTOR_Count; i++) {
		Actor* a = world.mActors[i];
		if (a == nullptr) {
			continue;
		}
		a->stopMove();
		a->mAnimSpeed = 30.0f;
		a->mTurnRate  = 8.0f;
		bool down     = a->getClip() == CLIP_Suwaru || a->getClip() == CLIP_Neru || a->getClip() == CLIP_Getup;
		if (down) {
			a->mAnimSpeed = 45.0f;
			a->startClip(CLIP_Getup);
		}
		if (i == ACTOR_Olimar) {
			mOlimarState = down ? OL_Getup : OL_Stand;
			a->faceToward(eye);
			if (!down) {
				a->startClip(CLIP_Wait);
			}
		} else {
			Piki& p       = mPiki[i - ACTOR_Red];
			p.mState      = PK_Stand;
			p.mLeg        = 0;
			p.mHasVia     = false;
			p.mTimer      = 0.0f;
			p.mHopAt      = -1.0f;
			p.mNoticeAt   = 0.12f * (f32)(i - ACTOR_Red);
			p.mAfterGetup = down ? CLIP_Kizuku : -1;
			p.mHopClip    = i == ACTOR_Red ? kClipRollJump : CLIP_Jump;
			p.mTrips      = (i - ACTOR_Red) == kTripper;
			p.mRunFrom    = a->mPos;
			if (world.mActors[ACTOR_Olimar] != nullptr) {
				a->faceToward(world.mActors[ACTOR_Olimar]->mPos);
			}
		}
	}
	cut(world, SHOT_TimeToGo);
}

// Honoured at once, in every phase.
void Takeoff::skip(SceneWorld& world)
{
	if (!mRunning) {
		return;
	}
	beginFade(kSkipFade);
	if (mEffects != nullptr) {
		mEffects->stopSet(DEMO_Beam);
		mEffects->stopSet(DEMO_Liftoff);
	}
	if (!mMusicFaded) {
		mMusicFaded = true;
		SceneAudio::fadeOut(kSkipFade);
	}
}

bool Takeoff::update(SceneWorld& world, f32 dt)
{
	if (!mRunning) {
		return false;
	}
	mElapsed += dt;

	if (mVariant != 0) {
		// Beats on the script clock. Under the fade (normal or skip) motion goes on, but no new
		// beat, cut, sound or emitter starts.
		mTime += dt;
		const int count = (int)(sizeof(kBeats) / sizeof(kBeats[0]));
		while (!mFading && mBeat < count && mTime >= kBeats[mBeat].mT) {
			const Beat& b = kBeats[mBeat];
			if (b.mGate != nullptr && !(this->*b.mGate)(world) && mGateWait < b.mMaxWait) {
				mGateWait += dt;
				mTime = b.mT; // the clock waits here
				break;
			}
			mGateWait = 0.0f;
			(this->*b.mFn)(world);
			mBeat++;
		}
		if (mClipFrame >= 0.0f) {
			mClipFrame += clipSpeed(mClipFrame) * dt;
			flightBeats(world);
		}

		// The area's air (petals, snow) thins out while the ship trembles: looking up from below,
		// the sky shots would look straight into the cloud of it the emitter releases overhead.
		if (mEffects != nullptr && mEffects->mAmbient != nullptr) {
			f32 a = 1.0f - smoothstep((mTime - T_Anticip) / (T_Ignite - T_Anticip));
			mEffects->mAmbient->setGlobalAlpha((u8)(255.0f * a * mAmbientKeep));
		}

		updateOlimar(world, dt);
		updatePikmin(world, dt);
		updateOnions(world, dt);
		updateShip(world, dt);
		updateCamera(world, dt);
		updateSky(world);
	}

	if (mFading) {
		f32 u = mFadeLength > 0.0f ? (mElapsed - mFadeStart) / mFadeLength : 1.0f;
		mFade = u > 1.0f ? 1.0f : u;
		if (u >= 1.0f) {
			mRunning = false;
			if (mSkySaved && world.mLight != nullptr) {
				// Give the area its own sky back (updateSky lent it the darkening one), and take it
				// afresh from the light at the next start.
				world.mLight->mSkyTop.r    = mSkyTop[0];
				world.mLight->mSkyTop.g    = mSkyTop[1];
				world.mLight->mSkyTop.b    = mSkyTop[2];
				world.mLight->mSkyZenith.r = mSkyZenith[0];
				world.mLight->mSkyZenith.g = mSkyZenith[1];
				world.mLight->mSkyZenith.b = mSkyZenith[2];
				mSkySaved                  = false;
			}
			return true;
		}
	}
	return false;
}

// ---- Beats --------------------------------------------------------------------------------------

void Takeoff::beatNod(SceneWorld& world)
{
	mNodFrom = mTime;
}

void Takeoff::beatWhistle(SceneWorld& world)
{
	Actor* olimar = world.mActors[ACTOR_Olimar];
	mLookLens     = false;
	if (olimar == nullptr) {
		return;
	}
	// To the Pikmin: a whistle, time to go.
	Vector3f mid(0.0f, 0.0f, 0.0f);
	int n = 0;
	for (int i = ACTOR_Red; i < ACTOR_Count; i++) {
		if (Actor* p = world.mActors[i]) {
			mid = Vector3f(mid.x + p->mPos.x, mid.y + p->mPos.y, mid.z + p->mPos.z);
			n++;
		}
	}
	if (n > 0) {
		mid = Vector3f(mid.x / n, mid.y / n, mid.z / n);
		// The Pikmin stand round and behind him from the lens: turning square to them showed the
		// player his back. His body turns from the lens toward them by at most kWhistleTurn; his
		// head makes up the rest, so the whistle is seen from the front or three-quarters.
		Vector3f eye = world.cameraEye();
		f32 lens     = atan2f(eye.x - olimar->mPos.x, eye.z - olimar->mPos.z);
		f32 toPiki   = atan2f(mid.x - olimar->mPos.x, mid.z - olimar->mPos.z);
		f32 turn     = toPiki - lens;
		while (turn > kPi) {
			turn -= 2.0f * kPi;
		}
		while (turn < -kPi) {
			turn += 2.0f * kPi;
		}
		turn = turn > kWhistleTurn ? kWhistleTurn : (turn < -kWhistleTurn ? -kWhistleTurn : turn);
		olimar->faceTo(lens + turn);
		Vector3f head(mid.x, mid.y + 12.0f, mid.z);
		olimar->lookAt(&head);
	}
	olimar->mAnimSpeed = 30.0f;
	olimar->startClip(CLIP_Fue);
	mWhistle = nullptr;
	if (JAISound* h = sound(PSSE_PL_SHUGO)) {
		for (int i = 0; i < kSoundPool; i++) {
			if (sSounds[i] == h) {
				mWhistle = &sSounds[i];
				break;
			}
		}
	}
}

void Takeoff::beatWhistleEnd(SceneWorld& world)
{
	if (mWhistle != nullptr && *mWhistle != nullptr) {
		(*mWhistle)->stop(6);
	}
	mWhistle = nullptr;
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		// Square to the lens again for the cheer, his head still on the Pikmin: the player sees
		// him enjoy it, and the nod to the lens at 2.80 follows from there.
		olimar->startClip(CLIP_Wait);
		olimar->faceToward(world.cameraEye());
	}
}

void Takeoff::beatHop(SceneWorld& world)
{
	// Red somersaults, yellow and blue hop: a ragged, eager cheer.
	for (int i = 0; i < 3; i++) {
		mPiki[i].mHopAt = mTime + 0.15f * (f32)i;
	}
}

// Off to their Onions: each to the nearest foot of its own, round Olimar if he is in the way.
void Takeoff::beatPikminRun(SceneWorld& world)
{
	Actor* olimar = world.mActors[ACTOR_Olimar];
	Vector3f eye  = world.cameraEye();
	for (int i = 0; i < 3; i++) {
		Actor* a      = world.mActors[ACTOR_Red + i];
		OnionActor* o = world.mOnions[i];
		Piki& p       = mPiki[i];
		if (a == nullptr || o == nullptr) {
			continue;
		}
		p.mLeg       = o->nearestLeg(a->mPos);
		Vector3f tip = o->legPoint(p.mLeg, 0.0f);
		tip.y        = a->mPos.y;
		p.mHasVia    = false;
		if (olimar != nullptr) {
			// Closest point of the run to Olimar.
			f32 dx = tip.x - a->mPos.x, dz = tip.z - a->mPos.z;
			f32 len2 = dx * dx + dz * dz;
			f32 t    = len2 > 0.0f ? ((olimar->mPos.x - a->mPos.x) * dx + (olimar->mPos.z - a->mPos.z) * dz) / len2 : 0.0f;
			t        = clamp01(t);
			Vector3f c(a->mPos.x + dx * t, a->mPos.y, a->mPos.z + dz * t);
			if (flatDist(c, olimar->mPos) < 10.0f) {
				f32 lx = eye.x - olimar->mPos.x, lz = eye.z - olimar->mPos.z;
				f32 l  = sqrtf(lx * lx + lz * lz);
				l      = l > 0.0f ? l : 1.0f;
				p.mVia    = Vector3f(olimar->mPos.x + lx / l * 12.0f, a->mPos.y, olimar->mPos.z + lz / l * 12.0f);
				p.mHasVia = true;
			}
		}
		p.mState     = PK_Run;
		p.mAfterGetup = -1;
		p.mRunFrom   = a->mPos;
		a->lookAt(nullptr);
		a->mAnimSpeed = 30.0f * kPikiRun / 120.0f;
		a->mTurnRate  = 14.0f;
		a->startClip(CLIP_Run2);
		a->moveTo(p.mHasVia ? p.mVia : tip, kPikiRun);
	}
}

// A last look at the player and a quick nod before he goes: the beat before the dash.
void Takeoff::beatLookLens(SceneWorld& world)
{
	mLookLens = true;
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		olimar->faceToward(world.cameraEye());
		olimar->nod();
	}
}

void Takeoff::beatTurnToShip(SceneWorld& world)
{
	mLookLens = false;
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		olimar->lookAt(nullptr);
		olimar->faceToward(world.mShip != nullptr ? world.mShip->mPos : world.mFrame.mOrigin);
	}
}

void Takeoff::beatOlimarRun(SceneWorld& world)
{
	cue(P2_SCENE_CUE_HOP);
	Actor* olimar   = world.mActors[ACTOR_Olimar];
	ShipActor* ship = world.mShip;
	if (olimar == nullptr) {
		return;
	}
	Vector3f start1 = shipJoint(ship, "start1") != nullptr ? ship->getJointPos("start1") : world.stagePos(kStart1);
	start1.y        = olimar->mPos.y;
	f32 d           = flatDist(start1, olimar->mPos);
	f32 speed       = d / 1.3f;
	speed           = speed < kOlimarRunMin ? kOlimarRunMin : (speed > kOlimarRunMax ? kOlimarRunMax : speed);
	olimar->startClip(CLIP_Run2);
	olimar->mAnimSpeed = 30.0f * speed / 120.0f;
	olimar->mTurnRate  = 14.0f;
	olimar->moveTo(start1, speed);
	mOlimarState = OL_Run;
}

void Takeoff::beatCutToOnions(SceneWorld& world)
{
	cut(world, SHOT_ToOnions);
	if (world.mStage != nullptr && world.mStage->mAmbientNearLens) {
		mAmbientKeep = 0.0f; // the cut hides the change
	}
}

void Takeoff::beatCutToClimb(SceneWorld& world)
{
	cut(world, SHOT_Climb);
}

void Takeoff::beatReady(SceneWorld& world)
{
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		olimar->lookAt(nullptr);
		olimar->faceToward(world.mShip != nullptr ? world.mShip->mPos : world.mFrame.mOrigin);
	}
}

// All Pikmin aboard their Onions and Olimar at the pick-up point.
bool Takeoff::gateReady(SceneWorld& world) const
{
	for (int i = 0; i < 3; i++) {
		if (world.mActors[ACTOR_Red + i] != nullptr && world.mOnions[i] != nullptr && mPiki[i].mState != PK_Aboard) {
			return false;
		}
	}
	return world.mActors[ACTOR_Olimar] == nullptr || mOlimarState == OL_Wait;
}

void Takeoff::beatCutToBeam(SceneWorld& world)
{
	cut(world, SHOT_Beam);
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		// He turns round to the lens: the goodbye.
		olimar->mTurnRate = 7.0f;
		olimar->faceToward(world.stagePos(kShots[SHOT_Beam].mEye));
		Vector3f eye = world.stagePos(kShots[SHOT_Beam].mEye);
		olimar->lookAt(&eye);
	}
}

// Goodbye: one arm high, waving, looking at the lens; then the hop into the beam.
void Takeoff::beatWave(SceneWorld& world)
{
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		olimar->wave(kWaveTime, 1);
	}
}

// BOARD: he hops into the tractor beam.
void Takeoff::beatBoard(SceneWorld& world)
{
	cue(P2_SCENE_CUE_BOARD);
	Actor* olimar   = world.mActors[ACTOR_Olimar];
	ShipActor* ship = world.mShip;
	if (olimar != nullptr) {
		mBoardFrom = olimar->mPos;
		olimar->stopMove();
		olimar->lookAt(nullptr);
		olimar->mAnimSpeed = 30.0f;
		olimar->startClip(CLIP_Jump);
		Vector3f cockpit = ship != nullptr ? ship->getJointPos("orima") : world.stagePos(kCockpit);
		f32 reach        = flatDist(cockpit, mBoardFrom);
		// The curve rises steeply out of the hop, then swings in over the cockpit.
		mBoardCtrl   = Vector3f(mBoardFrom.x + (cockpit.x - mBoardFrom.x) * 0.3f, cockpit.y + 0.35f * reach,
		                        mBoardFrom.z + (cockpit.z - mBoardFrom.z) * 0.3f);
		mOlimarState = OL_Ride;
	}
	if (ship != nullptr) {
		ship->mRootAnimator.startAnim(0, nullptr); // suction
		ship->mRootSpeed = 30.0f;
	}
	mBeamFx[0] = startFx(mEffects, DEMO_Beam, kBeamSuckA, ship);
	mBeamFx[1] = startFx(mEffects, DEMO_Beam, kBeamSuckB, ship);
	mBeamFx[2] = startFx(mEffects, DEMO_Beam, kBeamPtcl, ship);
	sound(PSSE_EV_S01_VACUUM_PLAYER);
}

// Aboard: the beam lets go, the ship swallows him with a burp.
void Takeoff::beatAboard(SceneWorld& world)
{
	if (Actor* olimar = world.mActors[ACTOR_Olimar]) {
		olimar->mVisible = false;
	}
	mOlimarState = OL_Aboard;
	if (mEffects != nullptr) {
		for (int i = 0; i < 3; i++) {
			mEffects->stop(mBeamFx[i]);
			mBeamFx[i] = -1;
		}
	}
	startFx(mEffects, DEMO_Beam, kBurpA, world.mShip, 50.0f / 30.0f);
	sound(PSSE_EV_VACUUM_PLAYER_END);
	mPistonAt = mTime;
}

void Takeoff::beatSpinUp(SceneWorld& world)
{
	mSpinUpAt = mTime;
}

void Takeoff::beatBurp2(SceneWorld& world)
{
	startFx(mEffects, DEMO_Beam, kBurpB, world.mShip, 18.0f / 30.0f);
}

void Takeoff::riseOnion(SceneWorld& world, int colour)
{
	OnionActor* o = world.mOnions[colour];
	mRiseAt[colour] = mTime;
	if (o == nullptr || mEffects == nullptr) {
		return;
	}
	mOnionFx[colour][0] = mEffects->startDemo(DEMO_Liftoff, kOnionSparkle, kOnionSparkleGroup, o->flowMatrix());
	if (colour != PIKI_Yellow) {
		// The leaves blown off the ground under it (the movie gives them to red and blue).
		Matrixf at;
		Vector3f one(1.0f, 1.0f, 1.0f), zero(0.0f, 0.0f, 0.0f);
		at.makeSRT(one, zero, o->mPos);
		mOnionFx[colour][1] = mEffects->startDemo(DEMO_Liftoff, kOnionLeaves, kOnionLeavesGroup, nullptr, &at, 0.0f, 0.5f);
	}
}

void Takeoff::beatCutToLift(SceneWorld& world)
{
	cut(world, SHOT_OnionsLift);
	riseOnion(world, PIKI_Red);
	sound(PSSE_EV_ONYON_FLY_AWAY);
}

void Takeoff::beatRiseBlue(SceneWorld& world)
{
	riseOnion(world, PIKI_Blue);
}

void Takeoff::beatRiseYellow(SceneWorld& world)
{
	riseOnion(world, PIKI_Yellow);
}

void Takeoff::beatMusicFade(SceneWorld& world)
{
	if (!mMusicFaded) {
		mMusicFaded = true;
		SceneAudio::fadeOut(kMusicFade);
	}
}

// The ship trembles; the plumes start, thin.
void Takeoff::beatCutToIgnition(SceneWorld& world)
{
	cut(world, SHOT_Ignition);
	for (int i = 4; i <= 5; i++) {
		DemoEmitter e = kLiftoffFx[i];
		e.mAlpha      = kSmokeEarlyAlpha;
		mLiftoffFx[i] = startFx(mEffects, DEMO_Liftoff, e, world.mShip);
	}
}

// IGNITE: Nintendo's lift-off on the ship's root, with its jets, smoke and leaves on the joints the
// movie attaches them to. The part motions stop driving their joints so the clip moves pistons and
// radar as authored.
void Takeoff::beatIgnite(SceneWorld& world)
{
	cue(P2_SCENE_CUE_IGNITE);
	mClipFrame      = 0.0f;
	ShipActor* ship = world.mShip;
	if (ship != nullptr && mAuthored) {
		ship->mOffset = Vector3f(0.0f, 0.0f, 0.0f);
		ship->mRootAnimator.startExAnim(&mLiftoff);
		ship->setRootFrame(0.0f);
		// The clip turns the root joint (`model`) to the angle of the movie's ship actor, 135
		// degrees; undo that turn so the ship lifts off facing the way it stands.
		J3DTransformInfo root;
		mLiftoff.mAnm->setFrame(0.0f);
		mLiftoff.mAnm->getTransform(0, &root);
		ship->mSpin        = -(f32)root.mRotation.y * kPi / 32768.0f;
		J3DModelData* data = ship->mModel->getJ3DModel()->getModelData();
		for (int i = 0; i < 3; i++) {
			if (ship->mPMotionJoint[i] >= 0) {
				data->getJointNodePointer(ship->mPMotionJoint[i])->setMtxCalc(nullptr);
				ship->mPMotionJoint[i] = -1;
			}
		}
	}
	for (u32 i = 0; i < sizeof(kLiftoffFx) / sizeof(kLiftoffFx[0]); i++) {
		if (mLiftoffFx[i] >= 0) {
			mEffects->setAlpha(mLiftoffFx[i], kLiftoffFx[i].mAlpha); // the plumes thicken
			continue;
		}
		mLiftoffFx[i] = startFx(mEffects, DEMO_Liftoff, kLiftoffFx[i], ship);
	}
	// Vapour trails on the Onions, so all four craft draw their line up the sky.
	for (int i = 0; i < 3; i++) {
		OnionActor* o = world.mOnions[i];
		if (o == nullptr || mEffects == nullptr || o->mBodyJoint == nullptr) {
			continue;
		}
		const DemoEmitter& e = kOnionTrail;
		mOnionFx[i][2] = mEffects->startDemo(DEMO_Liftoff, e.mId, e.mGroup, o->mBodyJoint->getWorldMatrix(), nullptr, 0.0f, e.mScale);
		mEffects->setAlpha(mOnionFx[i][2], e.mAlpha);
	}
	sound(PSSE_EV_ROCKET_FLY_AWAY);
}

// After ignition everything that flies runs on the clip frame.
void Takeoff::flightBeats(SceneWorld& world)
{
	enum { FB_Liftoff = 1, FB_Aerial = 2, FB_Leaf = 4, FB_Smoke = 8, FB_Away = 16, FB_Exit = 32 };
	if (mClipFrame >= F_Liftoff && !(mFlightDone & FB_Liftoff)) {
		mFlightDone |= FB_Liftoff;
		cue(P2_SCENE_CUE_LIFTOFF);
	}
	if (mFading) {
		return;
	}
	if (mClipFrame >= F_Aerial && !(mFlightDone & FB_Aerial)) {
		mFlightDone |= FB_Aerial;
		cut(world, SHOT_Aerial);
		p2_launcher_scene_ship_screen(0.5f, 0.0f, 0); // the bloom goes with the ignition shot
	}
	if (mClipFrame >= F_Leaf && !(mFlightDone & FB_Leaf)) {
		mFlightDone |= FB_Leaf;
		sound(PSSE_PL_S02_SCATTER_LEAF);
	}
	if (mClipFrame >= F_SmokeEnd && !(mFlightDone & FB_Smoke) && mEffects != nullptr) {
		// The launch cloud stops growing and drifts; the last leaves settle.
		mFlightDone |= FB_Smoke;
		for (int i = 4; i <= 6; i++) {
			mEffects->stop(mLiftoffFx[i]);
		}
		for (int i = 0; i < 3; i++) {
			mEffects->stop(mOnionFx[i][1]);
		}
	}
	if (mClipFrame >= F_Away && !(mFlightDone & FB_Away)) {
		mFlightDone |= FB_Away;
		cut(world, SHOT_Away);
		// Seen from the side against the sky the jets' glow would swallow the ship: dimmer, so its
		// silhouette and the trail read.
		if (mEffects != nullptr) {
			for (int i = 1; i <= 3; i++) {
				mEffects->setAlpha(mLiftoffFx[i], kAwayJetAlpha);
			}
		}
	}
	if (mClipFrame >= F_Exit && !(mFlightDone & FB_Exit)) {
		mFlightDone |= FB_Exit;
		beginFade(kFadeLength);
	}
}

// ---- Actors -------------------------------------------------------------------------------------

void Takeoff::updateOlimar(SceneWorld& world, f32 dt)
{
	Actor* olimar   = world.mActors[ACTOR_Olimar];
	ShipActor* ship = world.mShip;
	if (olimar == nullptr) {
		return;
	}
	const f32 T = mTime;

	if (mOlimarState == OL_Getup && olimar->isClipDone()) {
		olimar->mAnimSpeed = 30.0f;
		olimar->startClip(CLIP_Wait);
		mOlimarState = OL_Stand;
	}
	if (mLookLens) {
		// Looking at the player; the nod is two quick dips of the head.
		Vector3f eye  = world.cameraEye();
		Vector3f head = olimar->getHeadPos();
		f32 d         = flatDist(eye, head);
		f32 dip       = 0.0f;
		if (mNodFrom >= 0.0f) {
			f32 t = T - mNodFrom;
			if ((t >= 0.0f && t < 0.15f) || (t >= 0.30f && t < 0.45f)) {
				dip = -0.7f * d;
			}
		}
		Vector3f at(eye.x, eye.y + dip, eye.z);
		olimar->lookAt(&at);
	}
	if (mOlimarState == OL_Run && !olimar->isMoving()) {
		// At the pick-up point: he turns and watches the Pikmin climb in.
		mOlimarState       = OL_Wait;
		olimar->mAnimSpeed = 30.0f;
		olimar->startClip(CLIP_Wait);
		Vector3f mid(0.0f, 0.0f, 0.0f);
		int n = 0;
		for (int i = 0; i < 3; i++) {
			if (OnionActor* o = world.mOnions[i]) {
				mid = Vector3f(mid.x + o->mPos.x, mid.y + o->mPos.y, mid.z + o->mPos.z);
				n++;
			}
		}
		if (n > 0) {
			mid = Vector3f(mid.x / n, mid.y / n + 60.0f, mid.z / n);
			olimar->faceToward(mid);
			olimar->lookAt(&mid);
		}
	}
	if (mOlimarState == OL_Ride && T >= T_Carry) {
		// Up the beam: a curve into the cockpit, shrinking and turning.
		f32 u            = smoothstep((T - T_Carry) / kRideTime);
		Vector3f cockpit = ship != nullptr ? ship->getJointPos("orima") : world.stagePos(kCockpit);
		olimar->mPos     = bezier(mBoardFrom, mBoardCtrl, cockpit, u);
		olimar->mScale   = 1.0f - (1.0f - kRideShrink) * u * u;
		f32 du           = smoothstep((T + dt - T_Carry) / kRideTime) - u;
		olimar->snapFace(olimar->mFaceDir + kTau * kRideTurns * (du > 0.0f ? du : 0.0f));
	}
}

void Takeoff::updatePikmin(SceneWorld& world, f32 dt)
{
	Actor* olimar = world.mActors[ACTOR_Olimar];
	const f32 T   = mTime;
	for (int i = 0; i < 3; i++) {
		Actor* a      = world.mActors[ACTOR_Red + i];
		OnionActor* o = world.mOnions[i];
		Piki& p       = mPiki[i];
		if (a == nullptr) {
			continue;
		}
		switch (p.mState) {
		case PK_Stand:
			if (p.mAfterGetup >= 0) {
				if (a->isClipDone()) {
					a->mAnimSpeed = 30.0f;
					a->startClip(p.mAfterGetup);
					p.mAfterGetup = -1;
					p.mNoticeAt   = -1.0f;
				}
				break;
			}
			if (p.mNoticeAt >= 0.0f && T >= p.mNoticeAt) {
				p.mNoticeAt   = -1.0f;
				a->mAnimSpeed = 30.0f;
				a->startClip(CLIP_Kizuku);
			}
			if (p.mHopAt >= 0.0f && T >= p.mHopAt) {
				p.mHopAt      = -1.0f;
				a->mAnimSpeed = 30.0f;
				a->startClip(p.mHopClip);
				if (mVoices < 2) {
					mVoices++;
					sound(PSSE_PK_VC_YATTA, kVoiceVolume);
				}
			}
			if (olimar != nullptr && olimar->mVisible) {
				Vector3f head = olimar->getHeadPos();
				a->lookAt(&head);
				a->faceToward(olimar->mPos);
			}
			if (a->isClipDone()) {
				a->startClip(CLIP_Wait);
			}
			break;
		case PK_Run:
			if (p.mTrips && p.mHasVia == false && a->isMoving() && o != nullptr) {
				// Tripping on the way, in the onion shot: it falls flat, scrambles up and dashes on.
				Vector3f tip = o->legPoint(p.mLeg, 0.0f);
				f32 total    = flatDist(p.mRunFrom, tip);
				if (total > 1.0f && flatDist(p.mRunFrom, a->mPos) > kTripAt * total) {
					p.mTrips = false;
					p.mState = PK_Trip;
					p.mTimer = 0.0f;
					a->stopMove();
					a->mAnimSpeed = 30.0f;
					a->startClip(CLIP_Korobu);
					if (mVoices < 3) {
						mVoices++;
						sound(PSSE_PK_VC_LAND, kVoiceVolume);
					}
					break;
				}
			}
			if (!a->isMoving()) {
				if (p.mHasVia && o != nullptr) {
					p.mHasVia    = false;
					Vector3f tip = o->legPoint(p.mLeg, 0.0f);
					tip.y        = a->mPos.y;
					a->moveTo(tip, kPikiRun);
				} else {
					p.mState      = PK_Climb;
					p.mTimer      = 0.0f;
					a->mAnimSpeed = 30.0f;
					a->startClip(kClipClimb);
					if (o != nullptr) {
						a->faceToward(o->mPos);
					}
					sound(PSSE_PK_VC_ONY_CLIMB, kVoiceVolume);
				}
			}
			break;
		case PK_Trip:
			// KOROBU may hold its last frame without ending: a time limit as well.
			p.mTimer += dt;
			if (a->isClipDone() || p.mTimer >= kTripTime) {
				p.mState      = PK_Getup;
				p.mTimer      = 0.0f;
				a->mAnimSpeed = kGetupSpeed;
				a->startClip(CLIP_Getup);
			}
			break;
		case PK_Getup:
			p.mTimer += dt;
			if ((a->isClipDone() || p.mTimer >= kGetupTime) && o != nullptr) {
				// Back to the run, a little faster to catch up.
				Vector3f tip = o->legPoint(p.mLeg, 0.0f);
				tip.y        = a->mPos.y;
				p.mState      = PK_Run;
				a->mAnimSpeed = 30.0f * kPikiRun * 1.15f / 120.0f;
				a->startClip(CLIP_Run2);
				a->moveTo(tip, kPikiRun * 1.15f);
			}
			break;
		case PK_Climb: {
			// Up the leg to the body, shrinking into it over the last third.
			p.mTimer += dt;
			f32 u = p.mTimer / kClimbTime;
			if (o != nullptr) {
				a->mPos = o->legPoint(p.mLeg, u > 1.0f ? 1.0f : u);
				a->faceToward(o->mPos);
			}
			f32 s    = clamp01((u - 2.0f / 3.0f) * 3.0f);
			a->mScale = 1.0f - (1.0f - kClimbShrink) * s;
			if (u >= 1.0f) {
				p.mState   = PK_Aboard;
				a->mVisible = false;
				if (o != nullptr) {
					o->gulp();
				}
				sound(PSSE_PK_SE_ONY_ENTER);
			}
			break;
		}
		default:
			break;
		}
	}
}

// Propeller spin-up, the legs folding, and the rise: a Hermite curve from the rise start to the
// flight clip's height and climb rate at ignition, then the clip's own height.
void Takeoff::updateOnions(SceneWorld& world, f32 dt)
{
	const f32 T = mTime;
	for (int i = 0; i < 3; i++) {
		OnionActor* o = world.mOnions[i];
		if (o == nullptr) {
			continue;
		}
		f32 t0 = mRiseAt[i];
		if (t0 >= 0.0f) {
			f32 span     = T_Ignite - t0;
			f32 v        = clamp01((T - t0) / span);
			o->mSpinSpeed = kSpinIdle + (kSpinFlight - kSpinIdle) * smoothstep(v);
			o->mFlowScale = 1.0f + (kFlowSpread - 1.0f) * smoothstep((T - t0) / 0.8f);
			o->mFold      = smoothstep((T - t0 - kFoldDelay) / kFoldTime);
			if (mClipFrame >= 0.0f) {
				o->mLift = o->clipY(mClipFrame);
			} else {
				f32 h0  = o->clipY(0.0f);
				f32 v1  = (o->clipY(1.0f) - h0) * clipSpeed(0.0f);
				f32 u   = v;
				o->mLift = h0 * (3.0f * u * u - 2.0f * u * u * u) + v1 * span * (u * u * u - u * u);
			}
		} else if (mSpinUpAt >= 0.0f) {
			o->mSpinSpeed = kSpinIdle * smoothstep((T - mSpinUpAt) / 1.0f);
		}
	}
}

void Takeoff::updateShip(SceneWorld& world, f32 dt)
{
	ShipActor* ship = world.mShip;
	if (ship == nullptr) {
		return;
	}
	const f32 T       = mTime;
	bool reduceMotion = world.mConfig.reduceMotion != 0;

	// The pistons pump faster once he is aboard.
	if (mPistonAt >= 0.0f && mClipFrame < 0.0f) {
		ship->mPMotionSpeed[0] = 30.0f + 60.0f * smoothstep((T - mPistonAt) / 0.8f);
	}
	// It rocks on its legs: building while it trembles, strongest as the body leaves the ground,
	// dying down as it climbs.
	f32 rock = 0.0f;
	if (!reduceMotion && T >= T_Anticip) {
		if (mClipFrame < 0.0f) {
			rock = kRockDegPre * smoothstep((T - T_Anticip) / (T_Ignite - T_Anticip));
		} else if (mClipFrame < F_Liftoff) {
			rock = kRockDegPre + (kRockDegLift - kRockDegPre) * smoothstep(mClipFrame / F_Liftoff);
		} else {
			rock = kRockDegLift * (1.0f - smoothstep((mClipFrame - F_Liftoff) / (F_WobbleEnd - F_Liftoff)));
		}
	}
	ship->mTilt = Vector3f(rock * kDeg * sinf(kTau * kRockHzX * mElapsed), 0.0f, rock * kDeg * 0.8f * sinf(kTau * kRockHzZ * mElapsed + 1.3f));
	if (mClipFrame < 0.0f) {
		// The ignition shot opens on a trembling ship.
		ship->mOffset = Vector3f(0.0f, (T >= T_Anticip && !reduceMotion) ? kTrembleAmp * sinf(kTau * 9.0f * T) : 0.0f, 0.0f);
		return;
	}
	if (mAuthored) {
		ship->setRootFrame(mClipFrame);
	} else {
		// Without the clip: rumble, then an eased climb with a slow spin.
		f32 t = (mClipFrame - F_Liftoff) / 30.0f;
		if (t < 0.0f) {
			f32 r         = reduceMotion ? 0.0f : 0.6f;
			ship->mOffset = Vector3f((randFloat() - 0.5f) * r, (randFloat() - 0.5f) * r, (randFloat() - 0.5f) * r);
		} else {
			ship->mOffset = Vector3f(0.0f, 0.5f * kClimbAccel * t * t + 6.0f * t, 0.0f);
			f32 u         = t / ((F_End - F_Liftoff) / 30.0f);
			ship->mSpin   = kTau * kSpinClimb * u * u;
		}
	}

	// The launcher's bloom follows the ship in the ignition shot (DESIGN.md 7.2).
	if (mShot == SHOT_Ignition && world.mCamera != nullptr) {
		f32 x = 0.5f, y = 0.5f;
		bool on = world.mCamera->project(ship->getJointPos("body_s"), x, y);
		on      = on && x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
		p2_launcher_scene_ship_screen(x, y, on ? 1 : 0);
	}
}

// ---- Camera -------------------------------------------------------------------------------------

// Stage-frame heights of the ship's body, the Onions' mean body and the convoy's middle (the
// flight shots aim at it).
f32 Takeoff::convoyHeight(SceneWorld& world, f32& ship, f32& onions) const
{
	ship   = world.mShip != nullptr ? world.mFrame.toStage(world.mShip->getJointPos("body_s")).y : 60.0f;
	onions = 0.0f;
	int n  = 0;
	for (int i = 0; i < 3; i++) {
		if (OnionActor* o = world.mOnions[i]) {
			onions += world.mFrame.toStage(o->bodyPos()).y;
			n++;
		}
	}
	onions = n > 0 ? onions / n : ship;
	return 0.5f * (ship + onions);
}

// The last shot: the sky darkens toward space as the convoy climbs, and the stars come out
// (SceneEffects::setStars). The section draws its sky from the light manager's colours; the take-off
// lends them its own while the shot runs (the section ends with the take-off).
void Takeoff::updateSky(SceneWorld& world)
{
	f32 u = 0.0f;
	if (mShot == SHOT_Away && mClipFrame >= 0.0f) {
		u = smoothstep((mClipFrame - F_Away) / (F_Exit - F_Away));
	}
	if (mSkySaved && world.mLight != nullptr) {
		SceneLightMgr* l = world.mLight;
		l->mSkyTop.r     = (u8)(mSkyTop[0] + (kSpaceTop[0] - mSkyTop[0]) * u);
		l->mSkyTop.g     = (u8)(mSkyTop[1] + (kSpaceTop[1] - mSkyTop[1]) * u);
		l->mSkyTop.b     = (u8)(mSkyTop[2] + (kSpaceTop[2] - mSkyTop[2]) * u);
		l->mSkyZenith.r  = (u8)(mSkyZenith[0] + (kSpaceZenith[0] - mSkyZenith[0]) * u);
		l->mSkyZenith.g  = (u8)(mSkyZenith[1] + (kSpaceZenith[1] - mSkyZenith[1]) * u);
		l->mSkyZenith.b  = (u8)(mSkyZenith[2] + (kSpaceZenith[2] - mSkyZenith[2]) * u);
	}
	if (mEffects != nullptr) {
		mEffects->setStars(smoothstep(u * 1.5f - 0.35f));
	}
}

void Takeoff::cut(SceneWorld& world, int shot)
{
	mShot      = shot;
	mShotTime  = 0.0f;
	mShotFrame = mClipFrame;
	f32 hs = 0.0f, ho = 0.0f;
	mShotStartHeight = convoyHeight(world, hs, ho);
	Vector3f goal;
	f32 spring;
	shotPose(world, shot, 0.0f, true, mShotStart, goal, spring);
	mShotStart.mTarget = goal;
	mCamTarget         = goal; // the look-at spring starts on the new shot's target, at rest
	mCamTargetVel      = Vector3f(0.0f, 0.0f, 0.0f);
	if (mEffects != nullptr) {
		// The day-end movies' particles are authored at 30 steps a second; from the first cut on
		// (S1 -> S2) they run at that rate, and every cut takes one step so its first picture is
		// current. The idle petals keep the game's 60 through S1.
		mEffects->setStepHz(shot == SHOT_TimeToGo ? 60.0f : 30.0f);
		mEffects->forceStep();
	}
}

// The pose of `shot` `t` seconds after its cut. `calm` gives the Reduce Motion pose (the shot's
// start, its calm lens). `goal` is where the look-at spring heads (stage frame), `spring` its rate
// (0: no spring).
void Takeoff::shotPose(SceneWorld& world, int shot, f32 t, bool calm, CameraPose& pose, Vector3f& goal, f32& spring)
{
	const ShotDef& def = kShots[shot];
	Actor* olimar      = world.mActors[ACTOR_Olimar];
	ShipActor* ship    = world.mShip;
	pose.mUp           = Vector3f(0.0f, 1.0f, 0.0f);
	pose.mPos          = def.mEye;
	pose.mFovY         = calm ? def.mCalmFov : def.mFov;
	goal               = def.mTarget;
	spring             = 0.0f;
	f32 frame          = mClipFrame;

	switch (shot) {
	case SHOT_TimeToGo: {
		f32 u      = calm ? 0.0f : smoothstep(t / kS1MoveTime);
		pose.mPos  = Vector3f(mIdleEye.x + kS1EyeMove.x * u, mIdleEye.y + kS1EyeMove.y * u, mIdleEye.z + kS1EyeMove.z * u);
		pose.mFovY = mIdleFov * (1.0f + (kS1FovEnd - 1.0f) * u);
		goal       = mIdleTarget;
		if (!calm && olimar != nullptr && olimar->mVisible) {
			goal   = lerpV(mIdleTarget, world.mFrame.toStage(olimar->getHeadPos()), kS1Lean);
			spring = 4.0f;
		}
		break;
	}
	case SHOT_ToOnions:
		if (!calm) {
			pose.mPos.x += kS2Drift * clamp01(t / kS2DriftTime);
		}
		break;
	case SHOT_Climb:
		if (!calm) {
			pose.mPos = lerpV(def.mEye, def.mTarget, kClimbPush * smoothstep(t / kClimbPushTime));
		}
		break;
	case SHOT_Beam:
		if (!calm && olimar != nullptr && olimar->mVisible && mOlimarState == OL_Ride) {
			goal = lerpV(def.mTarget, world.mFrame.toStage(olimar->mPos), kS3Lean);
		}
		if (!calm && mTime > T_Board) {
			pose.mFovY = def.mFov + (kS3RideFov - def.mFov) * smoothstep((mTime - T_Board) / 1.0f);
		}
		spring = calm ? 0.0f : 3.0f;
		break;
	case SHOT_OnionsLift:
		if (world.mStage != nullptr && world.mStage->mShots != nullptr) {
			const StageShots& st = *world.mStage->mShots;
			pose.mPos  = st.mLiftEye;
			pose.mFovY = st.mLiftFov;
			goal       = st.mLiftTarget;
		}
		if (!calm) {
			f32 lift = 0.0f;
			int n    = 0;
			for (int i = 0; i < 3; i++) {
				if (OnionActor* o = world.mOnions[i]) {
					lift += o->mLift;
					n++;
				}
			}
			goal.y += kS4RiseAim * (n > 0 ? lift / n : 0.0f);
		}
		break;
	case SHOT_Ignition:
		if (!calm && frame >= 0.0f && ship != nullptr) {
			goal   = world.mFrame.toStage(ship->getJointPos("body_s"));
			spring = 5.5f;
		} else {
			spring = calm ? 0.0f : 5.5f;
		}
		break;
	case SHOT_Aerial: {
		// Above the convoy, looking down past the ship and the Onions at the forest. The eye rides
		// up a little slower than they climb and circles a few degrees round the landing site.
		f32 hs = 0.0f, ho = 0.0f;
		f32 mid = convoyHeight(world, hs, ho);
		f32 u   = clamp01((frame - F_Aerial) / (F_Away - F_Aerial));
		// From the convoy's height at the cut the eye follows kS6Follow of the climb.
		f32 h     = mShotStartHeight + (mid - mShotStartHeight) * kS6Follow;
		f32 a     = (calm ? 0.0f : kS6OrbitDeg * smoothstep(u)) * kDeg;
		f32 ca = cosf(a), sa = sinf(a);
		pose.mPos = Vector3f(def.mEye.x * ca + def.mEye.z * sa, h + def.mEye.y, -def.mEye.x * sa + def.mEye.z * ca);
		goal      = Vector3f(def.mTarget.x, mid + def.mTarget.y, def.mTarget.z);
		if (!calm) {
			pose.mFovY = def.mFov + (kS6FovEnd - def.mFov) * smoothstep(u);
		}
		break;
	}
	case SHOT_Away: {
		// Far off and low, looking up as the four craft climb on their trails.
		f32 hs = 0.0f, ho = 0.0f;
		f32 mid   = convoyHeight(world, hs, ho);
		f32 u     = clamp01((frame - F_Away) / (F_End - F_Away));
		// The eye climbs a little and the aim lags the climb (kS7Follow), so the four craft rise
		// through the frame on their trails instead of hanging still in it.
		pose.mPos = Vector3f(def.mEye.x, def.mEye.y + (mid - mShotStartHeight) * kS7Climb, def.mEye.z);
		goal      = Vector3f(def.mTarget.x, mShotStartHeight + (mid - mShotStartHeight) * kS7Follow + def.mTarget.y, def.mTarget.z);
		if (!calm) {
			pose.mFovY = def.mFov + (kS7FovEnd - def.mFov) * smoothstep(u);
		}
		break;
	}
	default:
		break;
	}
}

void Takeoff::updateCamera(SceneWorld& world, f32 dt)
{
	SceneCamera* cam = world.mCamera;
	if (cam == nullptr || world.mTune || mShot == SHOT_None) {
		return;
	}
	mShotTime += dt;
	bool calm = world.mConfig.reduceMotion != 0;

	CameraPose pose;
	Vector3f goal;
	f32 spring = 0.0f;
	if (calm && mShot < SHOT_Aerial) {
		pose = mShotStart;
		goal = mShotStart.mTarget;
	} else if (calm) {
		// The flight shots follow the convoy even under Reduce Motion (they would lose it at
		// once), but without orbit, lens change, spring or shake.
		shotPose(world, mShot, mShotTime, true, pose, goal, spring);
		spring = 0.0f;
	} else {
		shotPose(world, mShot, mShotTime, false, pose, goal, spring);
	}
	if (spring > 0.0f) {
		// A critically damped spring toward the goal.
		f32 w = spring;
		Vector3f acc((goal.x - mCamTarget.x) * w * w - 2.0f * w * mCamTargetVel.x,
		             (goal.y - mCamTarget.y) * w * w - 2.0f * w * mCamTargetVel.y,
		             (goal.z - mCamTarget.z) * w * w - 2.0f * w * mCamTargetVel.z);
		mCamTargetVel = Vector3f(mCamTargetVel.x + acc.x * dt, mCamTargetVel.y + acc.y * dt, mCamTargetVel.z + acc.z * dt);
		mCamTarget    = Vector3f(mCamTarget.x + mCamTargetVel.x * dt, mCamTarget.y + mCamTargetVel.y * dt, mCamTarget.z + mCamTargetVel.z * dt);
	} else {
		mCamTarget    = goal;
		mCamTargetVel = Vector3f(0.0f, 0.0f, 0.0f);
	}
	pose.mTarget = mCamTarget;

	f32 aspect = cam->mAspectRatio > 0.0f ? cam->mAspectRatio : 16.0f / 9.0f;
	if (mShot <= SHOT_Ignition) {
		// Ground shot: the 16:10 view on narrow pictures (the idle lens already has it), and the
		// top edge kept under the backdrop's rim.
		if (mShot != SHOT_TimeToGo) {
			pose.mFovY = groundFov(pose.mFovY, aspect);
		}
		Vector3f d(pose.mTarget.x - pose.mPos.x, pose.mTarget.y - pose.mPos.y, pose.mTarget.z - pose.mPos.z);
		f32 horiz    = sqrtf(d.x * d.x + d.z * d.z);
		f32 edge     = kShotSkyEdge[mShot];
		if (world.mStage != nullptr && world.mStage->mShots != nullptr) {
			const StageShots& st = *world.mStage->mShots;
			if ((mShot == SHOT_Beam || mShot == SHOT_Ignition) && st.mSkyEdgeDeg > 0.0f) {
				edge = st.mSkyEdgeDeg;
			} else if ((mShot == SHOT_ToOnions || mShot == SHOT_Climb) && st.mOnionsEdgeDeg > 0.0f) {
				edge = st.mOnionsEdgeDeg;
			}
		}
		f32 maxPitch = edge - 0.5f * pose.mFovY;
		f32 maxRise  = horiz * tanf(maxPitch * kDeg);
		if (d.y > maxRise) {
			pose.mTarget.y = pose.mPos.y + maxRise;
		}
	} else {
		pose.mFovY = groundFov(pose.mFovY, aspect);
	}

	// Shake: it builds while the ship trembles, peaks as the body leaves the ground and dies away
	// in the first sky shot.
	f32 amp = 0.0f;
	if (!calm) {
		if (mShot == SHOT_Ignition) {
			if (mClipFrame < 0.0f) {
				amp = 0.8f * smoothstep((mTime - T_Anticip) / (T_Ignite - T_Anticip));
			} else {
				amp = 0.8f + 0.4f * clamp01(mClipFrame / F_Liftoff);
			}
		} else if (mShot == SHOT_Aerial) {
			amp = 0.8f * (1.0f - clamp01(mShotTime / 0.5f));
		}
	}
	mShake = amp;
	f32 ph = kTau * 18.0f * mElapsed;

	cam->mOverride     = true;
	cam->mOverridePose = pose;
	cam->mShake        = Vector3f(amp * sinf(ph), amp * 0.7f * sinf(ph * 1.37f + 1.0f), 0.0f);
}

} // namespace LauncherScene
