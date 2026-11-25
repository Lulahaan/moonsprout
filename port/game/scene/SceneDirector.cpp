// Behaviour of the cast (docs/launcher/SCENE.md 7.4): idle loop, fidgets, events, glances and the
// long-idle wind-down. See the header for the layers.
#include "SceneDirector.h"
#include "SceneWorld.h"
#include "SceneAssets.h"

#include "SysShape/AnimMgr.h"
#include "SysShape/AnimInfo.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "Dolphin/rand.h"
#include "Dolphin/os.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace LauncherScene {

// Cast homes, stage frame (SCENE.md 7.1, tuned on screen against DESIGN.md 6.2). The group
// stands in front of the ship, close to the camera: red nearest, at Olimar's right; yellow just
// behind his left side; blue further back, between him and the ship.
const CastEntry kCast[ACTOR_Count] = {
	{ ACTOR_Olimar, 9.1f, 293.0f },
	{ ACTOR_Red, 21.5f, 290.0f },
	{ ACTOR_Yellow, -3.2f, 284.0f },
	{ ACTOR_Blue, 13.4f, 264.3f },
};

SceneWorld::SceneWorld()
    : mStage(nullptr)
    , mAssets(nullptr)
    , mShip(nullptr)
    , mCamera(nullptr)
    , mLight(nullptr)
    , mTime(0.0f)
    , mTune(false)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		mActors[i] = nullptr;
	}
	mConfig.area         = 1;
	mConfig.timeOfDay    = 0;
	mConfig.music        = 1;
	mConfig.takeoff      = 2;
	mConfig.reduceMotion = 0;
	mConfig.fadeColor    = 0xFFF6E0;
	mConfig.live         = 1;
}

Vector3f SceneWorld::cameraEye() const
{
	return mCamera != nullptr ? mFrame.toWorld(mCamera->idlePose().mPos) : home(ACTOR_Olimar);
}

// Olimar faces the camera turned 20 degrees to his left. The Pikmin face a point between him and
// the camera, so they turn to their captain without showing the viewer their backs.
f32 SceneWorld::defaultFaceDir(int kind) const
{
	Vector3f from = home(kind);
	Vector3f to;
	f32 turn = 0.0f;
	if (kind == ACTOR_Olimar) {
		to   = cameraEye();
		turn = 20.0f * 3.14159265f / 180.0f;
	} else {
		Vector3f olimar = home(ACTOR_Olimar);
		Vector3f eye    = cameraEye();
		to              = Vector3f(olimar.x + (eye.x - olimar.x) * 0.4f, 0.0f, olimar.z + (eye.z - olimar.z) * 0.4f);
	}
	return atan2f(to.x - from.x, to.z - from.z) + turn;
}

namespace {

const f32 kPi = 3.14159265f;

// Clips of the shared set that SceneActor.h does not name (confirmed in the clip viewer).
const int CLIP_RollJump = 35; // the somersault of a thrown Pikmin
// CLIP_Mizunomi (55) is named in SceneActor.h.
// Not a clip: "hop" picks the big JUMP when its arc stays inside the picture, else KIZUKU's small
// startled hop (JUMP lifts a Pikmin more than its own height, out of frame for the near ones).
const int CLIP_Hop = -2;
const f32 kJumpRise = 30.0f; // units above the head at the top of JUMP

// Speeds, units per second. The clips are authored for these (no foot sliding at 30 frames/s).
const f32 kWalkSpeed  = 24.0f;
// Centre to centre, Olimar to the Pikmin he visits. His kneel (MIZUNOMI) throws his head and
// helmet far forward of his feet: at 12.5 units the helmet swallowed the Pikmin's body and his
// antenna crossed its face. At 18 the helmet stops just short of the Pikmin's nose.
const f32 kVisitDistance = 18.0f;
const f32 kRunSpeed   = 58.0f;
const f32 kWalkAnimAt = 24.0f; // WALK plays at authored speed at this pace
const f32 kRunAnimAt  = 58.0f;

// Long idle (SCENE.md 7.4), seconds without input.
const f32 kOlimarSits  = 120.0f;
const f32 kPikiSits[3] = { 150.0f, 165.0f, 180.0f };
const f32 kPikiDozes[3] = { 200.0f, 214.0f, 228.0f };
const f32 kOlimarDozes = 245.0f;

// Growth: the Pikmin bloom one by one as the menu stays up (scene seconds).
const f32 kFirstBloom = 80.0f;

enum EventKind {
	EV_Pester,   // a Pikmin runs up to Olimar and hops for attention; he nods
	EV_Chat,     // two Pikmin have a chat; Olimar looks over
	EV_RollCall, // Olimar whistles, everyone runs to him, he nods, they go back
	EV_Chase,    // two Pikmin chase each other round the third; the chaser trips
	EV_YawnChain,
	EV_HopWave,  // the Pikmin hop one after another; Olimar cheers
	EV_Visit,    // Olimar walks over to a Pikmin and kneels to it
	EV_Watch,    // everyone looks at the ground under the mouse
	EV_Bloom,    // a Pikmin drinks in the light and grows a bud / flower
	EV_Count,
};

const char* const kEventNames[EV_Count] = { "pester", "chat", "roll-call", "chase", "yawn-chain", "hop-wave", "visit", "watch", "bloom" };
const int kEventWeight[EV_Count]        = { 22, 16, 12, 12, 8, 10, 12, 0, 0 };

const int kPikmin[3] = { ACTOR_Red, ACTOR_Yellow, ACTOR_Blue };

f32 randRange(f32 lo, f32 hi)
{
	return lo + randFloat() * (hi - lo);
}

int pickWeighted(const int* weights, int count, int avoid)
{
	int total = 0;
	for (int i = 0; i < count; i++) {
		total += i == avoid ? 0 : weights[i];
	}
	if (total <= 0) {
		return -1;
	}
	int roll = (int)(randFloat() * (f32)total);
	for (int i = 0; i < count; i++) {
		int w = i == avoid ? 0 : weights[i];
		if (roll < w) {
			return i;
		}
		roll -= w;
	}
	return -1;
}

f32 dist2D(const Vector3f& a, const Vector3f& b)
{
	f32 dx = a.x - b.x;
	f32 dz = a.z - b.z;
	return sqrtf(dx * dx + dz * dz);
}

f32 clipSeconds(Actor* actor, int clip, f32 speed)
{
	SysShape::AnimInfo* info = actor->mAnimator.mAnimMgr != nullptr ? actor->mAnimator.mAnimMgr->getAnimByID(clip) : nullptr;
	if (info == nullptr || info->mAnm == nullptr) {
		return 0.0f;
	}
	return (f32)info->mAnm->getTotalFrameCount() / (speed > 0.0f ? speed : 30.0f);
}

} // namespace

Director::Director()
    : mActive(true)
    , mWorld(nullptr)
    , mEvent(-1)
    , mEventStep(0)
    , mEventTime(0.0f)
    , mEventIn(0.0f)
    , mLastEvent(-1)
    , mCastA(-1)
    , mCastB(-1)
    , mCastC(-1)
    , mForcedDone(0)
    , mIdleTime(0.0f)
    , mHostIdleSeen(false)
    , mWindDown(0)
    , mIdleScale(1.0f)
    , mPointerX(0.5f)
    , mPointerY(0.5f)
    , mPointerStill(-1.0f)
    , mPointerUsed(false)
{
}

void Director::init(SceneWorld& world)
{
	mActive = true;
	mWorld  = &world;
	// Test switch (SETTINGS.md 1.3): MPPM_SCENE_IDLE_SCALE=10 brings long idle and blooming ten
	// times sooner, for headless checks.
	const char* scale = getenv("MPPM_SCENE_IDLE_SCALE");
	mIdleScale        = scale != nullptr && atof(scale) > 0.0 ? (f32)atof(scale) : 1.0f;
	reset(world);
}

void Director::reset(SceneWorld& world)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		Brain& b      = mBrain[i];
		b.mHead       = 0;
		b.mCount      = 0;
		b.mStarted    = false;
		b.mStepTime   = 0.0f;
		b.mLook       = Target_None;
		b.mLookLeft   = -1.0f;
		b.mLastFidget = -1;
		b.mInEvent    = false;
		b.mPosture    = Posture_Stand;
		// Olimar starts within a few seconds; the Pikmin spread out so they never start in step.
		b.mFidgetIn = i == ACTOR_Olimar ? randRange(3.0f, 6.0f) : randRange(1.5f, 7.0f);

		Actor* actor = world.mActors[i];
		if (actor == nullptr) {
			continue;
		}
		actor->mPos = world.home(i);
		actor->snapFace(world.defaultFaceDir(i));
		actor->stopMove();
		actor->lookAt(nullptr);
		actor->mVisible   = true;
		actor->mScale     = i == ACTOR_Olimar ? kOlimarScale : 1.0f;
		actor->mAnimSpeed = 30.0f;
		actor->startClip(CLIP_Wait);
		// Start each loop at its own frame so the four do not breathe in step.
		SysShape::AnimInfo* info = actor->mAnimator.mAnimInfo;
		if (info != nullptr && info->mAnm != nullptr) {
			actor->mAnimator.setCurrFrame(randFloat() * ((f32)info->mAnm->getTotalFrameCount() - 1.0f));
		}
	}
	mEvent     = -1;
	mEventIn   = randRange(5.0f, 8.0f);
	mLastEvent = -1;
	mIdleTime  = 0.0f;
	mWindDown  = 0;
}

void Director::release(SceneWorld& world)
{
	mActive = false;
	for (int i = 0; i < ACTOR_Count; i++) {
		clearQueue(i);
		mBrain[i].mInEvent = false;
		mBrain[i].mPosture = Posture_Stand;
		if (Actor* a = world.mActors[i]) {
			a->mAnimSpeed = 30.0f;
		}
	}
	mEvent = -1;
}

// ---- Step queues ---------------------------------------------------------------------------------

void Director::clearQueue(int who)
{
	mBrain[who].mHead    = 0;
	mBrain[who].mCount   = 0;
	mBrain[who].mStarted = false;
	if (mWorld != nullptr && mWorld->mActors[who] != nullptr) {
		mWorld->mActors[who]->stopMove();
	}
}

Step& Director::push(int who, u8 kind)
{
	Brain& b = mBrain[who];
	if (b.mCount >= Brain::kQueueSize) {
		b.mCount = Brain::kQueueSize - 1; // drop the newest rather than overrun (never expected)
	}
	Step& s  = b.mQueue[(b.mHead + b.mCount) % Brain::kQueueSize];
	b.mCount++;
	s.mKind   = kind;
	s.mTarget = Target_None;
	s.mClip   = -1;
	s.mTime   = 0.0f;
	s.mSpeed  = 0.0f;
	s.mPos    = Vector3f(0.0f, 0.0f, 0.0f);
	return s;
}

void Director::clip(int who, int clipId, f32 speed)
{
	Step& s  = push(who, Step::Clip);
	s.mClip  = clipId;
	s.mSpeed = speed;
}

void Director::loop(int who, int clipId, f32 seconds)
{
	Step& s = push(who, Step::Loop);
	s.mClip = clipId;
	s.mTime = seconds;
}

void Director::hold(int who, f32 seconds)
{
	Step& s = push(who, Step::Hold);
	s.mTime = seconds;
}

void Director::walk(int who, const Vector3f& to, f32 speed)
{
	Step& s  = push(who, Step::Walk);
	s.mPos   = to;
	s.mSpeed = speed;
}

void Director::face(int who, int target, f32 seconds)
{
	Step& s   = push(who, Step::Face);
	s.mTarget = (s8)target;
	s.mTime   = seconds;
}

void Director::facePoint(int who, const Vector3f& point, f32 seconds)
{
	Step& s   = push(who, Step::Face);
	s.mTarget = Target_Point;
	s.mPos    = point;
	s.mTime   = seconds;
}

void Director::look(int who, int target, f32 seconds)
{
	Step& s   = push(who, Step::Look);
	s.mTarget = (s8)target;
	s.mTime   = seconds;
}

bool Director::allIdle(const int* who, int count) const
{
	for (int i = 0; i < count; i++) {
		if (who[i] >= 0 && !idle(who[i])) {
			return false;
		}
	}
	return true;
}

Vector3f Director::targetPos(SceneWorld& world, int target, const Vector3f& point, bool head) const
{
	if (target >= 0 && target < ACTOR_Count && world.mActors[target] != nullptr) {
		Actor* a = world.mActors[target];
		return head ? a->getHeadPos() : a->mPos;
	}
	if (target == Target_Camera) {
		return world.cameraEye();
	}
	if (target == Target_Pointer) {
		Vector3f ground;
		if (pointerGround(world, ground)) {
			return ground;
		}
		return world.cameraEye();
	}
	return point;
}

void Director::beginStep(SceneWorld& world, int who, Step& step)
{
	Actor* a        = world.mActors[who];
	Brain& b        = mBrain[who];
	b.mStepTime     = 0.0f;
	switch (step.mKind) {
	case Step::Clip:
		if (step.mClip == CLIP_Hop) {
			step.mClip = CLIP_Kizuku;
			f32 x, y;
			Vector3f top = a->getHeadPos();
			top.y += kJumpRise;
			if (who != ACTOR_Olimar && world.mCamera != nullptr && world.mCamera->project(top, x, y) && y > 0.07f) {
				step.mClip = CLIP_Jump;
			}
		}
		a->mAnimSpeed = step.mSpeed > 0.0f ? step.mSpeed : 30.0f;
		a->startClip(step.mClip);
		break;
	case Step::Loop:
		a->mAnimSpeed = 30.0f;
		if (a->getClip() != step.mClip) {
			a->startClip(step.mClip);
			// Olimar may play another clip in its place (Actor::startClip): its frames differ.
			if (step.mSpeed > 0.0f && a->getClip() == step.mClip) {
				a->mAnimator.setCurrFrame(step.mSpeed); // Loop: mSpeed = first frame
			}
		}
		break;
	case Step::Hold:
		if (a->isClipDone() || a->isMoving()) {
			a->mAnimSpeed = 30.0f;
			a->startClip(CLIP_Wait);
		}
		break;
	case Step::Walk: {
		f32 speed = step.mSpeed;
		if (reduced(world) && speed > kWalkSpeed) {
			speed = kWalkSpeed;
		}
		bool run      = speed > 40.0f;
		a->mAnimSpeed = 30.0f * speed / (run ? kRunAnimAt : kWalkAnimAt);
		if (a->getClip() != (run ? CLIP_Run2 : CLIP_Walk)) {
			a->startClip(run ? CLIP_Run2 : CLIP_Walk);
		}
		a->moveTo(step.mPos, speed);
		break;
	}
	case Step::Face:
		if (step.mTarget == Target_Default) {
			a->faceTo(world.defaultFaceDir(who));
		} else {
			a->faceToward(targetPos(world, step.mTarget, step.mPos, false));
		}
		break;
	case Step::Look:
		b.mLook     = step.mTarget;
		b.mLookPos  = step.mPos;
		b.mLookLeft = step.mTime > 0.0f ? step.mTime : -1.0f;
		break;
	case Step::Happa:
		a->mHappa = step.mClip;
		break;
	}
}

bool Director::stepDone(SceneWorld& world, int who, Step& step)
{
	Actor* a = world.mActors[who];
	Brain& b = mBrain[who];
	switch (step.mKind) {
	case Step::Clip: {
		// The end key event, with a time-out in case a clip has none.
		f32 limit = clipSeconds(a, step.mClip, a->mAnimSpeed) + 0.5f;
		return a->isClipDone() || b.mStepTime > limit;
	}
	case Step::Loop:
	case Step::Hold:
	case Step::Face:
		return b.mStepTime >= step.mTime;
	case Step::Walk:
		if (!a->isMoving()) {
			a->mAnimSpeed = 30.0f;
			a->startClip(CLIP_Wait);
			return true;
		}
		return false;
	default:
		return true;
	}
}

void Director::runQueue(SceneWorld& world, int who, f32 dt)
{
	Brain& b = mBrain[who];
	Actor* a = world.mActors[who];
	b.mStepTime += dt;
	// Instant steps (look, face without wait, happa) chain within one frame.
	for (int guard = 0; guard < Brain::kQueueSize && b.mCount > 0; guard++) {
		Step& s = b.mQueue[b.mHead];
		if (!b.mStarted) {
			beginStep(world, who, s);
			b.mStarted = true;
		}
		if (!stepDone(world, who, s)) {
			return;
		}
		b.mHead    = (b.mHead + 1) % Brain::kQueueSize;
		b.mCount--;
		b.mStarted = false;
		b.mStepTime = 0.0f;
	}
	// Nothing queued: stand, unless sitting or asleep (their loops keep running).
	if (b.mCount == 0 && b.mPosture == Posture_Stand && a->isClipDone() && !a->isMoving()) {
		a->mAnimSpeed = 30.0f;
		a->startClip(CLIP_Wait);
	}
}

void Director::updateLook(SceneWorld& world, int who, f32 dt)
{
	Brain& b = mBrain[who];
	Actor* a = world.mActors[who];
	if (b.mLookLeft >= 0.0f) {
		b.mLookLeft -= dt;
		if (b.mLookLeft < 0.0f) {
			b.mLook = Target_None;
		}
	}
	if (b.mLook == Target_None || b.mLook == who) {
		a->lookAt(nullptr);
		return;
	}
	Vector3f p = targetPos(world, b.mLook, b.mLookPos, true);
	a->lookAt(&p);
}

// ---- Helpers -------------------------------------------------------------------------------------

bool Director::present(SceneWorld& world, int who) const
{
	return who >= 0 && who < ACTOR_Count && world.mActors[who] != nullptr;
}

int Director::randomPikmin(SceneWorld& world, int notA, int notB) const
{
	int pool[3];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		int p = kPikmin[i];
		if (p != notA && p != notB && present(world, p) && !mBrain[p].mInEvent) {
			pool[n++] = p;
		}
	}
	return n > 0 ? pool[(int)(randFloat() * (f32)n) % n] : -1;
}

// A point `distance` from `anchor`'s current position, on the side toward `who`.
Vector3f Director::nearPoint(SceneWorld& world, int who, int anchor, f32 distance) const
{
	Vector3f from = world.mActors[who]->mPos;
	Vector3f to   = world.mActors[anchor]->mPos;
	f32 dx        = from.x - to.x;
	f32 dz        = from.z - to.z;
	f32 len       = sqrtf(dx * dx + dz * dz);
	if (len < 0.01f) {
		return from;
	}
	return Vector3f(to.x + dx / len * distance, to.y, to.z + dz / len * distance);
}

// True when a character standing at `pos` (world, on the ground) is fully inside the subject zone
// of the idle framing: x 0.40-0.93 of the picture, head clear of the top.
bool Director::inSubjectZone(SceneWorld& world, const Vector3f& pos) const
{
	if (world.mCamera == nullptr) {
		return true;
	}
	f32 x, y;
	Vector3f top(pos.x, pos.y + 24.0f, pos.z);
	if (!world.mCamera->project(pos, x, y) || x < 0.40f || x > 0.93f || y > 0.92f) {
		return false;
	}
	if (!world.mCamera->project(top, x, y) || x < 0.40f || x > 0.93f || y < 0.08f) {
		return false;
	}
	return true;
}

// True when walking straight from `who` to `to` passes no one else closer than `clearance`.
bool Director::pathClear(SceneWorld& world, int who, const Vector3f& to, f32 clearance) const
{
	Vector3f from = world.mActors[who]->mPos;
	f32 dx = to.x - from.x, dz = to.z - from.z;
	f32 len2 = dx * dx + dz * dz;
	for (int i = 0; i < ACTOR_Count; i++) {
		if (i == who || !present(world, i)) {
			continue;
		}
		Vector3f p = world.mActors[i]->mPos;
		f32 t = len2 > 0.0001f ? ((p.x - from.x) * dx + (p.z - from.z) * dz) / len2 : 0.0f;
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		Vector3f q(from.x + dx * t, p.y, from.z + dz * t);
		if (dist2D(p, q) < clearance) {
			return false;
		}
	}
	return true;
}

// The ground point under the mouse cursor (the landing site is flat at the ship's height).
bool Director::pointerGround(SceneWorld& world, Vector3f& out) const
{
	if (world.mCamera == nullptr) {
		return false;
	}
	SceneCamera* cam = world.mCamera;
	Vector3f eye     = cam->mPosition;
	Vector3f fwd(cam->mLookAtPosition.x - eye.x, cam->mLookAtPosition.y - eye.y, cam->mLookAtPosition.z - eye.z);
	f32 len = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
	if (len < 0.001f) {
		return false;
	}
	fwd = Vector3f(fwd.x / len, fwd.y / len, fwd.z / len);
	// right = fwd x up, up' = right x fwd
	Vector3f right(-fwd.z, 0.0f, fwd.x);
	f32 rl = sqrtf(right.x * right.x + right.z * right.z);
	if (rl < 0.001f) {
		return false;
	}
	right = Vector3f(right.x / rl, 0.0f, right.z / rl);
	Vector3f up(right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x);
	f32 t  = tanf(cam->mViewAngle * 0.5f * kPi / 180.0f);
	f32 a  = cam->mAspectRatio > 0.0f ? cam->mAspectRatio : 1.0f;
	f32 nx = (mPointerX - 0.5f) * 2.0f * t * a;
	f32 ny = (0.5f - mPointerY) * 2.0f * t;
	Vector3f dir(fwd.x + right.x * nx + up.x * ny, fwd.y + right.y * nx + up.y * ny, fwd.z + right.z * nx + up.z * ny);
	f32 ground = world.home(ACTOR_Olimar).y;
	if (dir.y > -0.01f) {
		return false; // at or above the horizon
	}
	f32 k = (ground - eye.y) / dir.y;
	if (k > 400.0f) {
		k = 400.0f; // far ground: still a direction to look in
	}
	out = Vector3f(eye.x + dir.x * k, ground, eye.z + dir.z * k);
	return true;
}

f32 Director::interval(f32 lo, f32 hi) const
{
	return randRange(lo, hi);
}

bool Director::reduced(SceneWorld& world) const
{
	return world.mConfig.reduceMotion != 0;
}

// The others glance at whoever just did something (a hop, a flip, a trip), if they are free.
void Director::noticeActivity(int who, f32 chance)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		Brain& b = mBrain[i];
		if (i == who || b.mInEvent || b.mPosture != Posture_Stand || b.mLook != Target_None) {
			continue;
		}
		if (randFloat() < chance) {
			b.mLook     = who;
			b.mLookLeft = randRange(1.4f, 2.6f);
		}
	}
}

// ---- Fidgets -------------------------------------------------------------------------------------

void Director::rollFidget(SceneWorld& world, int who)
{
	if (who == ACTOR_Olimar) {
		fidgetOlimar(world);
	} else {
		fidgetPikmin(world, who);
	}
	f32 scale = reduced(world) ? 2.0f : 1.0f;
	mBrain[who].mFidgetIn = scale * (who == ACTOR_Olimar ? interval(3.5f, 8.0f) : interval(2.5f, 6.5f));
}

// Olimar's idle loop: mostly WAIT with his eyes wandering over the group, the player and the
// ship; now and then a bigger gesture.
void Director::fidgetOlimar(SceneWorld& world)
{
	enum { F_LookAround, F_LookPlayer, F_LookShip, F_LookPikmin, F_Yawn, F_Sigh, F_Nod, F_Pointer, F_Count };
	int weights[F_Count] = { 22, 14, 10, 30, 6, 4, 6, 0 };
	if (mPointerUsed) {
		weights[F_Pointer] = 14;
	}
	const int o = ACTOR_Olimar;
	int f       = pickWeighted(weights, F_Count, mBrain[o].mLastFidget);
	mBrain[o].mLastFidget = f;
	switch (f) {
	case F_LookAround:
		clip(o, randFloat() < 0.5f ? CLIP_Sagasu : CLIP_Sagasu2);
		break;
	case F_LookPlayer:
		look(o, Target_Camera, randRange(2.0f, 3.2f));
		hold(o, 2.0f);
		break;
	case F_LookShip:
		look(o, Target_None);
		clip(o, CLIP_Furimuku);
		break;
	case F_LookPikmin: {
		int p = randomPikmin(world);
		if (p >= 0) {
			look(o, p, randRange(1.8f, 3.0f));
			hold(o, 1.5f);
		}
		break;
	}
	case F_Yawn:
		look(o, Target_None);
		clip(o, CLIP_Akubi);
		break;
	case F_Sigh:
		clip(o, CLIP_Gakkari);
		break;
	case F_Nod:
		look(o, Target_Camera, 2.4f);
		hold(o, 0.8f);
		clip(o, CLIP_Gattu);
		break;
	case F_Pointer:
		look(o, Target_Pointer, 2.5f);
		hold(o, 2.0f);
		break;
	}
}

// Each Pikmin's own little life.
void Director::fidgetPikmin(SceneWorld& world, int p)
{
	enum {
		F_LookAround,
		F_Hop,
		F_Wander,
		F_Step,
		F_LookOlimar,
		F_LookPlayer,
		F_Yawn,
		F_Sit,
		F_Doze,
		F_Flip,
		F_Drink,
		F_Pointer,
		F_Count
	};
	int weights[F_Count] = { 24, 14, 18, 7, 14, 10, 7, 6, 4, 4, 5, 0 };
	if (reduced(world)) {
		weights[F_Wander] = 0;
		weights[F_Flip]   = 0;
	}
	if (mPointerUsed) {
		weights[F_Pointer] = 8;
	}
	Brain& b = mBrain[p];
	int f    = pickWeighted(weights, F_Count, b.mLastFidget);
	b.mLastFidget = f;
	Vector3f home = world.home(p);
	switch (f) {
	case F_LookAround:
		clip(p, randFloat() < 0.5f ? CLIP_Sagasu : CLIP_Sagasu2);
		break;
	case F_Hop:
		clip(p, CLIP_Hop);
		if (randFloat() < 0.4f) {
			clip(p, CLIP_Hop);
		}
		noticeActivity(p, 0.5f);
		break;
	case F_Wander: {
		// A few steps to a spot near home that keeps clear of the others, a look round, back.
		Vector3f to = home;
		for (int tries = 0; tries < 8; tries++) {
			f32 ang = randFloat() * 2.0f * kPi;
			f32 r   = randRange(6.0f, 14.0f);
			Vector3f cand(home.x + r * sinf(ang), home.y, home.z + r * cosf(ang));
			bool clear = true;
			for (int i = 0; i < ACTOR_Count; i++) {
				if (i != p && present(world, i) && dist2D(cand, world.mActors[i]->mPos) < 9.0f) {
					clear = false;
				}
			}
			// Keep out of the near foreground and inside the subject zone of the picture
			// (DESIGN.md 6.2), whatever the window's shape.
			Vector3f st = world.mFrame.toStage(cand);
			if (st.z > 300.0f || !inSubjectZone(world, cand)) {
				clear = false;
			}
			if (clear) {
				to = cand;
				break;
			}
		}
		walk(p, to, kWalkSpeed);
		if (randFloat() < 0.5f) {
			clip(p, CLIP_Sagasu2);
		} else {
			hold(p, randRange(0.8f, 1.8f));
		}
		walk(p, home, kWalkSpeed);
		face(p, Target_Default, 0.3f);
		break;
	}
	case F_Step:
		loop(p, CLIP_Asibumi, randRange(1.2f, 2.0f));
		break;
	case F_LookOlimar:
		look(p, ACTOR_Olimar, randRange(1.8f, 3.0f));
		hold(p, 1.5f);
		break;
	case F_LookPlayer:
		// Notices you, stares a moment, and sometimes hops hello.
		look(p, Target_Camera, randRange(2.0f, 3.0f));
		hold(p, 1.4f);
		if (randFloat() < 0.5f) {
			clip(p, CLIP_Hop);
		}
		break;
	case F_Yawn:
		clip(p, CLIP_Akubi);
		break;
	case F_Sit:
		look(p, Target_None);
		b.mPosture = Posture_Sit;
		loop(p, CLIP_Suwaru, randRange(5.0f, 9.0f));
		clip(p, CLIP_Getup);
		break;
	case F_Doze:
		// Sits, nods off, wakes with a start.
		look(p, Target_None);
		b.mPosture = Posture_Sit;
		loop(p, CLIP_Suwaru, randRange(2.5f, 4.0f));
		loop(p, CLIP_Neru, randRange(4.0f, 7.0f));
		clip(p, CLIP_Kizuku);
		look(p, Target_Camera, 1.5f);
		break;
	case F_Flip:
		clip(p, CLIP_RollJump);
		noticeActivity(p, 0.7f);
		break;
	case F_Drink:
		clip(p, CLIP_Mizunomi);
		break;
	case F_Pointer:
		look(p, Target_Pointer, 2.2f);
		hold(p, 1.8f);
		break;
	}
}

// ---- Events --------------------------------------------------------------------------------------

bool Director::startEvent(SceneWorld& world)
{
	int weights[EV_Count];
	for (int i = 0; i < EV_Count; i++) {
		weights[i] = kEventWeight[i];
	}
	if (reduced(world)) {
		weights[EV_Chase]    = 0;
		weights[EV_RollCall] = 0;
	}
	if (mPointerUsed && mPointerStill > 0.6f && mPointerX > 0.4f && mPointerY > 0.3f) {
		weights[EV_Watch] = 14;
	}
	// Growth: from kFirstBloom on, a Pikmin that has not flowered yet may bloom.
	bool canBloom = false;
	for (int i = 0; i < 3; i++) {
		if (present(world, kPikmin[i]) && world.mActors[kPikmin[i]]->mHappa < HAPPA_Flower) {
			canBloom = true;
		}
	}
	if (canBloom && world.mTime * mIdleScale >= kFirstBloom) {
		weights[EV_Bloom] = 14;
	}

	int ev = pickWeighted(weights, EV_Count, mLastEvent);
	// Test switch: MPPM_SCENE_EVENTS=bloom,chase,... plays those first, in order (headless checks).
	bool forced = false;
	if (const char* list = getenv("MPPM_SCENE_EVENTS")) {
		int index = 0;
		for (const char* p = list; *p != '\0' && !forced;) {
			const char* end = p;
			while (*end != '\0' && *end != ',') {
				end++;
			}
			if (index == mForcedDone) {
				for (int i = 0; i < EV_Count; i++) {
					const char* name = kEventNames[i];
					int len          = (int)strlen(name);
					if (len == (int)(end - p) && strncmp(name, p, len) == 0) {
						ev     = i;
						forced = true;
					}
				}
				if (!forced) {
					mForcedDone++; // unknown name: skip it
				}
			}
			index++;
			p = *end == ',' ? end + 1 : end;
		}
	}
	if (ev < 0) {
		return false;
	}
	// Everyone the event needs must be free and standing; otherwise try again shortly.
	const int o = ACTOR_Olimar;
	if (!present(world, o)) {
		return false;
	}
	for (int i = 0; i < ACTOR_Count; i++) {
		if (present(world, i) && (!idle(i) || mBrain[i].mPosture != Posture_Stand)) {
			return false;
		}
	}
	mCastA = randomPikmin(world);
	mCastB = randomPikmin(world, mCastA);
	mCastC = randomPikmin(world, mCastA, mCastB);
	if (ev == EV_Bloom) {
		// The least grown goes first.
		int best = -1;
		for (int i = 0; i < 3; i++) {
			int p = kPikmin[(i + (int)(randFloat() * 3.0f)) % 3];
			if (present(world, p) && (best < 0 || world.mActors[p]->mHappa < world.mActors[best]->mHappa)) {
				best = p;
			}
		}
		mCastA = best;
		mCastB = randomPikmin(world, mCastA);
		mCastC = randomPikmin(world, mCastA, mCastB);
	}
	if (mCastA < 0 || ((ev == EV_Chat || ev == EV_Chase) && mCastB < 0)) {
		return false;
	}
	if (ev == EV_Chat) {
		// The visitor must not walk through anyone on the way (red and yellow have Olimar between
		// them); swap roles or partner until a clear pair turns up.
		bool found = false;
		for (int tries = 0; tries < 6 && !found; tries++) {
			Vector3f spot = nearPoint(world, mCastA, mCastB, 8.0f);
			if (pathClear(world, mCastA, spot, 6.0f)) {
				found = true;
				break;
			}
			int t  = mCastA;
			mCastA = mCastB;
			mCastB = tries % 2 == 0 ? t : randomPikmin(world, mCastA);
			if (mCastB < 0) {
				break;
			}
		}
		if (!found) {
			return false;
		}
		mCastC = randomPikmin(world, mCastA, mCastB);
	}
	if (ev == EV_Visit) {
		// Only a Pikmin at his side or in front: walking off toward the ship would show the player
		// his back for the whole visit.
		Vector3f op  = world.mActors[o]->mPos;
		Vector3f eye = world.cameraEye();
		Vector3f pp  = world.mActors[mCastA]->mPos;
		f32 cx = eye.x - op.x, cz = eye.z - op.z, px = pp.x - op.x, pz = pp.z - op.z;
		f32 cl = sqrtf(cx * cx + cz * cz), pl = sqrtf(px * px + pz * pz);
		if (cl < 0.01f || pl < 0.01f || (cx * px + cz * pz) / (cl * pl) < -0.35f) {
			return false;
		}
		if (!pathClear(world, o, nearPoint(world, o, mCastA, kVisitDistance), 6.0f)) {
			return false;
		}
	}
	if (forced) {
		mForcedDone++;
	}
	mEvent     = ev;
	mLastEvent = ev;
	mEventStep = 0;
	mEventTime = 0.0f;
	for (int i = 0; i < ACTOR_Count; i++) {
		mBrain[i].mInEvent = present(world, i);
	}
	OSReport("scene: event %s at %.1f s\n", kEventNames[ev], world.mTime);
	return true;
}

void Director::endEvent(SceneWorld& world)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		mBrain[i].mInEvent = false;
		// A short pause before their own fidgets come back, so the end of an event reads.
		if (mBrain[i].mFidgetIn < 1.5f) {
			mBrain[i].mFidgetIn = randRange(1.5f, 4.0f);
		}
	}
	mEvent   = -1;
	mEventIn = (reduced(world) ? 1.5f : 1.0f) * interval(6.0f, 13.0f);
}

// Each event is a few beats; a beat queues steps and the next beat starts when the actors it
// waits for have emptied their queues (or a time-out passes).
void Director::updateEvent(SceneWorld& world, f32 dt)
{
	mEventTime += dt;
	const int o = ACTOR_Olimar;
	const int a = mCastA, b = mCastB, c = mCastC;
	int everyone[ACTOR_Count] = { ACTOR_Olimar, ACTOR_Red, ACTOR_Yellow, ACTOR_Blue };
	for (int i = 0; i < ACTOR_Count; i++) {
		if (!present(world, everyone[i])) {
			everyone[i] = -1;
		}
	}
	bool ready = allIdle(everyone, ACTOR_Count) || mEventTime > 20.0f;
	if (!ready) {
		return;
	}
#define NEXT_BEAT()        \
	do {                   \
		mEventStep++;      \
		mEventTime = 0.0f; \
	} while (0)

	switch (mEvent) {
	case EV_Pester:
		switch (mEventStep) {
		case 0: {
			// Runs up beside him, looking up.
			Vector3f spot = nearPoint(world, a, o, 12.0f);
			look(o, a, 0.0f);
			clip(a, CLIP_Kizuku);
			walk(a, spot, kRunSpeed);
			face(a, o, 0.25f);
			look(a, o, 0.0f);
			hold(o, 0.6f);
			NEXT_BEAT();
			break;
		}
		case 1:
			clip(a, CLIP_Hop);
			clip(a, CLIP_Hop);
			hold(o, 0.4f);
			face(o, a, 0.5f);
			if (b >= 0) {
				look(b, a, 2.5f);
			}
			if (c >= 0) {
				look(c, a, 2.0f);
			}
			NEXT_BEAT();
			break;
		case 2:
			clip(o, CLIP_Gattu);
			hold(a, 0.5f);
			clip(a, CLIP_Hop);
			NEXT_BEAT();
			break;
		case 3:
			walk(a, world.home(a), kWalkSpeed);
			face(a, Target_Default, 0.3f);
			look(a, Target_None);
			hold(o, 1.2f);
			face(o, Target_Default, 0.6f);
			look(o, Target_None);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_Chat:
		switch (mEventStep) {
		case 0: {
			// a walks over to b; they turn to each other.
			Vector3f spot = nearPoint(world, a, b, 8.0f);
			walk(a, spot, kWalkSpeed);
			face(a, b, 0.2f);
			look(a, b);
			hold(b, 0.6f);
			face(b, a, 0.3f);
			look(b, a);
			NEXT_BEAT();
			break;
		}
		case 1:
			clip(a, CLIP_Chatting);
			hold(b, 0.35f);
			clip(b, CLIP_Chatting);
			hold(o, 1.0f);
			look(o, a, 3.0f);
			clip(o, CLIP_Furimuku);
			if (c >= 0) {
				look(c, a, 2.5f);
			}
			NEXT_BEAT();
			break;
		case 2:
			// One more exchange, then a happy hop from the one who was visited.
			clip(b, CLIP_Chatting);
			hold(a, 0.5f);
			clip(a, CLIP_Gattu);
			NEXT_BEAT();
			break;
		case 3:
			clip(b, CLIP_Hop);
			walk(a, world.home(a), kWalkSpeed);
			face(a, Target_Default, 0.3f);
			look(a, Target_None);
			look(b, Target_None);
			face(b, Target_Default, 0.4f);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_RollCall:
		switch (mEventStep) {
		case 0:
			look(o, Target_None);
			face(o, Target_Default, 0.2f);
			loop(o, CLIP_Fue, 1.1f);
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (present(world, p)) {
					hold(p, 0.25f + 0.15f * (f32)i);
					clip(p, CLIP_Kizuku);
				}
			}
			NEXT_BEAT();
			break;
		case 1: {
			// They gather in an arc round his front-side, in profile to the player.
			Vector3f op  = world.mActors[o]->mPos;
			Vector3f eye = world.cameraEye();
			f32 base     = atan2f(eye.x - op.x, eye.z - op.z);
			static const f32 kArc[3] = { 1.6f, -1.6f, 2.25f }; // red right, yellow left, blue behind-right
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (!present(world, p)) {
					continue;
				}
				// The nearest Pikmin to each slot would be nicer; the homes already sit that way.
				f32 ang = base + kArc[i];
				Vector3f spot(op.x + 15.0f * sinf(ang), op.y, op.z + 15.0f * cosf(ang));
				if (!inSubjectZone(world, spot)) {
					spot = world.mActors[p]->mPos; // a slot off the picture: answer from where it stands
				}
				walk(p, spot, kRunSpeed);
				face(p, o, 0.3f);
				look(p, o);
			}
			NEXT_BEAT();
			break;
		}
		case 2:
			// He looks them over one by one and nods; they cheer.
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (present(world, p)) {
					look(o, p, 0.0f);
					hold(o, 0.6f);
				}
			}
			look(o, Target_Camera, 2.0f);
			clip(o, CLIP_Gattu);
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (present(world, p)) {
					hold(p, 2.0f + 0.2f * (f32)i);
					clip(p, CLIP_Hop);
				}
			}
			NEXT_BEAT();
			break;
		case 3:
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (present(world, p)) {
					look(p, Target_None);
					walk(p, world.home(p), kWalkSpeed);
					face(p, Target_Default, 0.3f);
				}
			}
			face(o, Target_Default, 0.5f);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_Chase:
		switch (mEventStep) {
		case 0: {
			// a chases b round the blue Pikmin's spot, trips on the last lap and gets up; b stops,
			// turns and hops about it. Olimar's eyes follow the chase, then he sighs.
			const f32 cx = kCast[ACTOR_Blue].mHomeX, cz = kCast[ACTOR_Blue].mHomeZ, r = 15.0f;
			const int n          = 8;
			const f32 chaseSpeed = kRunSpeed * 0.97f;
			Vector3f first       = world.stagePos(cx, cz + r);
			f32 legA             = dist2D(world.mActors[a]->mPos, first) + (f32)(n * 2 - 3) * 2.0f * r * sinf(kPi / (f32)n);
			f32 tripAt           = 0.15f + clipSeconds(world.mActors[a], CLIP_Kizuku, 30.0f) + legA / chaseSpeed;
			for (int k = 0; k < n * 2; k++) {
				f32 ang = (f32)k * 2.0f * kPi / (f32)n;
				walk(b, world.stagePos(cx + r * sinf(ang), cz + r * cosf(ang)), kRunSpeed);
			}
			face(b, a, 0.4f);
			look(b, a, 0.0f);
			clip(b, CLIP_Hop);
			clip(b, CLIP_Hop);
			hold(a, 0.15f);
			clip(a, CLIP_Kizuku);
			for (int k = 0; k < n * 2 - 2; k++) {
				f32 ang = (f32)k * 2.0f * kPi / (f32)n;
				walk(a, world.stagePos(cx + r * sinf(ang), cz + r * cosf(ang)), chaseSpeed);
			}
			clip(a, CLIP_Korobu);
			clip(a, CLIP_Getup);
			look(o, b, 0.0f);
			hold(o, tripAt);
			look(o, a, 0.0f);
			hold(o, 0.5f);
			clip(o, CLIP_Gakkari);
			if (c >= 0) {
				look(c, b, tripAt);
				hold(c, tripAt);
				look(c, a, 3.0f);
				clip(c, CLIP_Kizuku);
			}
			NEXT_BEAT();
			break;
		}
		case 1:
			hold(b, 0.4f);
			clip(b, CLIP_Gattu);
			NEXT_BEAT();
			break;
		case 2:
			for (int i = 0; i < 3; i++) {
				int p = kPikmin[i];
				if (present(world, p)) {
					look(p, Target_None);
					walk(p, world.home(p), kWalkSpeed);
					face(p, Target_Default, 0.3f);
				}
			}
			look(o, Target_None);
			face(o, Target_Default, 0.4f);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_YawnChain:
		switch (mEventStep) {
		case 0:
			clip(a, CLIP_Akubi);
			if (b >= 0) {
				look(b, a, 1.2f);
				hold(b, 1.0f);
				clip(b, CLIP_Akubi);
			}
			if (c >= 0) {
				hold(c, 1.9f);
				clip(c, CLIP_Akubi);
			}
			look(o, a, 2.5f);
			hold(o, 3.0f);
			clip(o, CLIP_Akubi);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_HopWave:
		switch (mEventStep) {
		case 0: {
			// Left to right on screen: yellow, blue, red, twice round.
			static const int kOrder[3] = { ACTOR_Yellow, ACTOR_Blue, ACTOR_Red };
			for (int i = 0; i < 3; i++) {
				int p = kOrder[i];
				if (!present(world, p)) {
					continue;
				}
				hold(p, 0.3f * (f32)i);
				clip(p, CLIP_Hop);
				hold(p, 0.2f);
				clip(p, CLIP_Hop);
			}
			look(o, ACTOR_Yellow, 0.7f);
			hold(o, 0.6f);
			look(o, ACTOR_Red, 1.0f);
			hold(o, 0.8f);
			look(o, Target_Camera, 1.8f);
			clip(o, CLIP_Gattu);
			NEXT_BEAT();
			break;
		}
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_Visit:
		switch (mEventStep) {
		case 0: {
			// He strolls over to a Pikmin, which turns and looks up at him.
			Vector3f spot = nearPoint(world, o, a, kVisitDistance);
			look(o, a, 0.0f);
			walk(o, spot, kWalkSpeed * 0.85f);
			face(o, a, 0.3f);
			hold(a, 0.8f);
			face(a, o, 0.3f);
			look(a, o);
			NEXT_BEAT();
			break;
		}
		case 1:
			clip(o, CLIP_Mizunomi);
			hold(a, 0.6f);
			clip(a, CLIP_Hop);
			if (b >= 0) {
				look(b, a, 2.0f);
			}
			NEXT_BEAT();
			break;
		case 2:
			clip(o, CLIP_Gattu);
			clip(a, CLIP_Gattu);
			NEXT_BEAT();
			break;
		case 3:
			look(o, Target_None);
			walk(o, world.home(o), kWalkSpeed * 0.85f);
			face(o, Target_Default, 0.5f);
			look(a, o, 2.0f);
			hold(a, 1.5f);
			face(a, Target_Default, 0.3f);
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_Watch:
		switch (mEventStep) {
		case 0:
			for (int i = 0; i < ACTOR_Count; i++) {
				if (present(world, i)) {
					hold(i, 0.15f * (f32)i);
					look(i, Target_Pointer, 3.0f);
					hold(i, 3.0f);
				}
			}
			if (present(world, a)) {
				clip(a, CLIP_Kizuku);
			}
			NEXT_BEAT();
			break;
		default:
			endEvent(world);
			break;
		}
		break;

	case EV_Bloom:
		switch (mEventStep) {
		case 0: {
			// GROWUP1, and halfway through the leaf becomes a bud (or the bud a flower).
			Actor* actor = world.mActors[a];
			int next     = actor->mHappa + 1 > HAPPA_Flower ? HAPPA_Flower : actor->mHappa + 1;
			f32 half     = clipSeconds(actor, CLIP_Growup1, 30.0f) * 0.5f;
			loop(a, CLIP_Growup1, half);
			Step& h = push(a, Step::Happa);
			h.mClip = (s16)next;
			hold(a, half + 0.3f); // the rest of the clip keeps playing over the swap
			clip(a, CLIP_Hop);
			for (int i = 0; i < ACTOR_Count; i++) {
				if (i == a || !present(world, i)) {
					continue;
				}
				hold(i, 0.2f + 0.2f * randFloat());
				look(i, a, 4.0f);
				if (i != ACTOR_Olimar) {
					hold(i, half);
					clip(i, CLIP_Hop);
				} else {
					hold(i, half + 0.6f);
					clip(i, CLIP_Gattu);
				}
			}
			NEXT_BEAT();
			break;
		}
		default:
			endEvent(world);
			break;
		}
		break;

	default:
		endEvent(world);
		break;
	}
#undef NEXT_BEAT
}

// ---- Long idle -----------------------------------------------------------------------------------

void Director::wakeUp(SceneWorld& world)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		if (!present(world, i) || mBrain[i].mPosture == Posture_Stand) {
			continue;
		}
		clearQueue(i);
		mBrain[i].mPosture = Posture_Stand;
		hold(i, 0.12f * (f32)i);
		clip(i, CLIP_Kizuku);
		look(i, Target_Camera, 1.6f);
		hold(i, 1.0f);
		face(i, Target_Default, 0.3f);
		mBrain[i].mFidgetIn = randRange(3.0f, 6.0f);
	}
	mWindDown = 0;
	mEventIn  = randRange(6.0f, 10.0f);
}

void Director::updateLongIdle(SceneWorld& world, f32 dt)
{
	// The host counts from the last input it saw and reports 0 until the first one, so until then
	// the scene's own clock stands in.
	int host = p2_launcher_scene_idle_seconds();
	f32 idleNow;
	if (host > 0) {
		mHostIdleSeen = true;
	}
	if (mHostIdleSeen) {
		idleNow = (f32)host;
	} else {
		idleNow = mIdleTime + dt;
	}
	bool input = idleNow + 0.5f < mIdleTime; // the count went back: somebody touched something
	mIdleTime  = idleNow;
	if (input && mWindDown > 0) {
		wakeUp(world);
		return;
	}
	if (mEvent >= 0) {
		return;
	}
	f32 t = mIdleTime * mIdleScale;
	const int o = ACTOR_Olimar;
	// Stage 1: Olimar sits. 2-4: the Pikmin sit one by one. 5-7: they nod off. 8: so does he.
	if (mWindDown == 0 && t >= kOlimarSits && present(world, o) && idle(o)) {
		look(o, Target_None);
		face(o, Target_Default, 0.3f);
		mBrain[o].mPosture = Posture_Sit;
		loop(o, CLIP_Suwaru, 1.0e9f);
		mWindDown = 1;
		OSReport("scene: long idle, Olimar sits\n");
	}
	for (int i = 0; i < 3; i++) {
		int p = kPikmin[i];
		if (mWindDown == 1 + i && t >= kPikiSits[i]) {
			if (present(world, p)) {
				if (!idle(p)) {
					return; // let it finish what it is doing
				}
				look(p, Target_None);
				walk(p, world.home(p), kWalkSpeed);
				face(p, Target_Default, 0.3f);
				mBrain[p].mPosture = Posture_Sit;
				loop(p, CLIP_Suwaru, 1.0e9f);
			}
			mWindDown++;
		}
	}
	for (int i = 0; i < 3; i++) {
		int p = kPikmin[i];
		if (mWindDown == 4 + i && t >= kPikiDozes[i]) {
			if (present(world, p)) {
				clearQueue(p);
				mBrain[p].mPosture = Posture_Sleep;
				loop(p, CLIP_Neru, 1.0e9f);
			}
			mWindDown++;
		}
	}
	if (mWindDown == 7 && t >= kOlimarDozes && present(world, o)) {
		clearQueue(o);
		mBrain[o].mPosture = Posture_Sleep;
		// From the loop: NERU's lead-in has him flop onto his back across the yellow Pikmin's spot.
		loop(o, CLIP_Neru, 1.0e9f);
		mBrain[o].mQueue[(mBrain[o].mHead + mBrain[o].mCount - 1) % Brain::kQueueSize].mSpeed = 30.0f;
		mWindDown = 8;
		OSReport("scene: long idle, everyone asleep\n");
	}
}

// ---- Per frame -----------------------------------------------------------------------------------

void Director::update(SceneWorld& world, f32 dt)
{
	if (!mActive) {
		return;
	}
	mWorld = &world;
	if (world.mTune) {
		// The clip viewer owns the cast: only the base layer (a finished clip falls back to WAIT).
		for (int i = 0; i < ACTOR_Count; i++) {
			Actor* actor = world.mActors[i];
			if (actor != nullptr && actor->isClipDone() && !actor->isMoving()) {
				actor->startClip(CLIP_Wait);
			}
		}
		return;
	}

	// Pointer: in use when it moved lately; "still" while it rests over the scene.
	float px = 0.5f, py = 0.5f;
	int active = 0;
	p2_launcher_scene_pointer(&px, &py, &active);
	// The first sample only sets the reference (where the cursor happened to rest at start-up);
	// "in use" means moved within the last 12 s and resting over the scene, not the menu.
	f32 moved = fabsf(px - mPointerX) + fabsf(py - mPointerY);
	if (mPointerStill < 0.0f) {
		mPointerStill = 1000.0f;
	} else {
		mPointerStill = active && moved > 0.002f ? 0.0f : mPointerStill + dt;
	}
	mPointerX    = px;
	mPointerY    = py;
	mPointerUsed = active != 0 && mPointerX > 0.35f && mPointerStill < 12.0f;

	updateLongIdle(world, dt);

	if (mEvent >= 0) {
		updateEvent(world, dt);
	} else if (mWindDown == 0) {
		mEventIn -= dt;
		if (mEventIn <= 0.0f && !startEvent(world)) {
			mEventIn = 0.7f; // someone was busy: try again in a moment
		}
	}

	for (int i = 0; i < ACTOR_Count; i++) {
		if (!present(world, i)) {
			continue;
		}
		Brain& b = mBrain[i];
		// While an event waits to start nobody begins anything new, so it never waits long.
		bool eventDue = mEvent < 0 && mEventIn <= 0.0f;
		if (!b.mInEvent && !eventDue && b.mPosture == Posture_Stand && mWindDown == 0 && idle(i)) {
			b.mFidgetIn -= dt;
			if (b.mFidgetIn <= 0.0f) {
				rollFidget(world, i);
			}
		}
		runQueue(world, i, dt);
		// A fidget's sit / doze ends with its last step.
		if (idle(i) && b.mPosture != Posture_Stand && mWindDown == 0) {
			b.mPosture = Posture_Stand;
		}
		updateLook(world, i, dt);
	}
	separate(world, dt);
}

// Personal space. The steps pick their spots from where the others stand when they start, so two
// actors can still end up on one spot (two wanderers choose the same place, one walks home through
// another's fidget spot). The ones closer than their two radii are eased apart; one that is
// walking gives way, one that stands still is barely nudged. Event spots (a chat at 8 units, a
// pester at 12) stay outside these distances, so the choreography is untouched.
void Director::separate(SceneWorld& world, f32 dt)
{
	const f32 kRadiusOlimar = 5.5f;
	const f32 kRadiusPiki   = 3.75f;
	f32 rate = 1.0f - expf(-10.0f * dt);
	for (int i = 0; i < ACTOR_Count; i++) {
		Actor* a = world.mActors[i];
		if (!present(world, i) || !a->mVisible) {
			continue;
		}
		for (int j = i + 1; j < ACTOR_Count; j++) {
			Actor* b = world.mActors[j];
			if (!present(world, j) || !b->mVisible) {
				continue;
			}
			f32 minDist = (i == ACTOR_Olimar ? kRadiusOlimar : kRadiusPiki) + kRadiusPiki;
			f32 dx      = b->mPos.x - a->mPos.x;
			f32 dz      = b->mPos.z - a->mPos.z;
			f32 d       = sqrtf(dx * dx + dz * dz);
			if (d >= minDist) {
				continue;
			}
			if (d < 0.01f) {
				// On the same spot: part them sideways to the camera.
				dx = world.mFrame.dirToWorld(Vector3f(1.0f, 0.0f, 0.0f)).x;
				dz = world.mFrame.dirToWorld(Vector3f(1.0f, 0.0f, 0.0f)).z;
			} else {
				dx /= d;
				dz /= d;
			}
			// A walk that ends inside the other one ends here instead, at arm's length.
			if (a->isMoving() && dist2D(a->mMoveTarget, b->mPos) < minDist) {
				a->stopMove();
			}
			if (b->isMoving() && dist2D(b->mMoveTarget, a->mPos) < minDist) {
				b->stopMove();
			}
			bool ma  = a->isMoving();
			bool mb  = b->isMoving();
			f32 wa   = ma == mb ? 0.5f : (ma ? 0.9f : 0.1f);
			f32 push = (minDist - d) * rate;
			a->mPos.x -= dx * push * wa;
			a->mPos.z -= dz * push * wa;
			b->mPos.x += dx * push * (1.0f - wa);
			b->mPos.z += dz * push * (1.0f - wa);
		}
	}
}

} // namespace LauncherScene
