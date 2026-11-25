// Behaviour of the cast while the start menu is up (docs/launcher/SCENE.md 7.4).
//
// Three layers, highest first: a script (the take-off, SceneTakeoff.h) owns everyone while it runs;
// an *event* is one interaction between two or more of the cast (a Pikmin pesters Olimar, a roll
// call, a chase that ends in a trip...); *fidgets* are what each one does on its own timer when it
// is not in an event (look around, hop, yawn, wander, sit down...). Under both sits the idle loop:
// WAIT at home, a glance now and then, and the long-idle wind-down (sit, then doze off) that any
// input undoes.
//
// Everything an event or fidget does is written as a short queue of steps per actor (play a clip,
// walk somewhere, turn, look at someone, wait); the director runs the queues and, for events, waits
// for the actors it needs between beats. The section calls nothing but init(), update() and the
// two hand-over functions.
#pragma once

#include "types.h"
#include "Vector3.h"
#include "SceneActor.h"

namespace LauncherScene {

struct SceneWorld;

// One thing an actor does; see Director::runQueue for how each kind starts and ends.
struct Step {
	enum Kind {
		Clip,  // a one-shot clip at `speed` frames/s (0: authored speed); ends with the clip
		Loop,  // a looping clip held for `time` seconds (from frame `speed` when > 0)
		Hold,  // stand (WAIT unless a loop is playing) for `time` seconds
		Walk,  // walk / run to `pos` at `speed` units/s
		Face,  // turn toward `target`; takes `time` seconds before the next step
		Look,  // head toward `target` for `time` seconds (0: until the next Look)
		Happa, // `clip` = new HappaKind (leaf, bud, flower)
	};
	u8 mKind;
	s8 mTarget; // actor index, or a Target value
	s16 mClip;
	f32 mTime;
	f32 mSpeed;
	Vector3f mPos; // world (Walk) or the point of a Target_Point
};

enum Target {
	Target_None    = -1, // Look: straight ahead
	Target_Point   = -2, // the step's mPos
	Target_Camera  = -3, // the player
	Target_Default = -4, // Face: the actor's default direction at home
	Target_Pointer = -5, // the ground under the mouse cursor
};

enum Posture { Posture_Stand, Posture_Sit, Posture_Sleep };

// What the director keeps per actor.
struct Brain {
	enum { kQueueSize = 16 };

	Step mQueue[kQueueSize];
	int mHead, mCount;
	bool mStarted; // the head step has begun
	f32 mStepTime; // seconds in the head step

	int mLook;         // Target or actor index the head follows
	Vector3f mLookPos; // for Target_Point
	f32 mLookLeft;     // seconds until the look ends (< 0: no limit)

	f32 mFidgetIn;   // seconds to the next fidget
	int mLastFidget; // no fidget twice in a row
	bool mInEvent;
	int mPosture;
};

struct Director {
	Director();

	void init(SceneWorld& world); // places the cast at home (main thread, after loading)
	void update(SceneWorld& world, f32 dt);

	// The take-off script takes the cast over (SceneTakeoff.h): stop rolling new behaviour and let
	// whatever runs end where it stands.
	void release(SceneWorld& world);
	// Debug tools hand the cast back (SceneDebug.cpp, Backspace): everyone home, WAIT.
	void reset(SceneWorld& world);

	bool mActive; // false once released

private:
	// Step queues.
	void clearQueue(int who);
	Step& push(int who, u8 kind);
	void clip(int who, int clip, f32 speed = 0.0f);
	void loop(int who, int clip, f32 seconds);
	void hold(int who, f32 seconds);
	void walk(int who, const Vector3f& to, f32 speed);
	void face(int who, int target, f32 seconds = 0.0f);
	void facePoint(int who, const Vector3f& point, f32 seconds = 0.0f);
	void look(int who, int target, f32 seconds = 0.0f);
	bool idle(int who) const { return mBrain[who].mCount == 0; }
	bool allIdle(const int* who, int count) const;

	void runQueue(SceneWorld& world, int who, f32 dt);
	void beginStep(SceneWorld& world, int who, Step& step);
	bool stepDone(SceneWorld& world, int who, Step& step);
	void updateLook(SceneWorld& world, int who, f32 dt);
	Vector3f targetPos(SceneWorld& world, int target, const Vector3f& point, bool head) const;

	// Layers.
	void rollFidget(SceneWorld& world, int who);
	void fidgetOlimar(SceneWorld& world);
	void fidgetPikmin(SceneWorld& world, int who);
	void noticeActivity(int who, f32 chance);
	bool startEvent(SceneWorld& world);
	void updateEvent(SceneWorld& world, f32 dt);
	void endEvent(SceneWorld& world);
	void updateLongIdle(SceneWorld& world, f32 dt);
	void wakeUp(SceneWorld& world);
	void separate(SceneWorld& world, f32 dt);

	// Helpers.
	bool present(SceneWorld& world, int who) const;
	int randomPikmin(SceneWorld& world, int notA = -1, int notB = -1) const;
	Vector3f nearPoint(SceneWorld& world, int who, int anchor, f32 distance) const;
	bool inSubjectZone(SceneWorld& world, const Vector3f& pos) const;
	bool pathClear(SceneWorld& world, int who, const Vector3f& to, f32 clearance) const;
	bool pointerGround(SceneWorld& world, Vector3f& out) const;
	f32 interval(f32 lo, f32 hi) const;
	bool reduced(SceneWorld& world) const;

	SceneWorld* mWorld; // set by init / update, for clearQueue
	Brain mBrain[ACTOR_Count];

	int mEvent;      // EventKind, or -1
	int mEventStep;  // beat within the event
	f32 mEventTime;  // seconds in the current beat
	f32 mEventIn;    // seconds to the next event roll
	int mLastEvent;
	int mCastA, mCastB, mCastC; // who the event picked
	int mForcedDone; // MPPM_SCENE_EVENTS entries already played

	f32 mIdleTime;      // seconds without input (long idle), from the host or the scene clock
	bool mHostIdleSeen; // the host has reported idle time at least once
	int mWindDown;      // long-idle stage reached (0: awake)
	f32 mIdleScale;     // MPPM_SCENE_IDLE_SCALE: long idle and growth come this many times sooner
	f32 mPointerX, mPointerY;
	f32 mPointerStill;  // seconds the pointer has rested in the scene
	bool mPointerUsed;
};

} // namespace LauncherScene
