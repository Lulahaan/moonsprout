// Particles of the start-menu scene (docs/launcher/SCENE.md 6.2 step 9, 10.2, 11): the game's
// particle manager with its own resources plus three demo resource sets, each in its own slot of
// the emitter manager (SCENE.md R6, second option; no swapping):
//
//   slot 7  s00_coursein   the area's air (petals, snow or leaves) during the idle
//   slot 5  s01_dayend     the tractor beam that lifts Olimar aboard, and the ship's burp after it
//   slot 6  s02_dayend_result  the lift-off: four jets, two smoke plumes, the leaves blown away
//
// The demo emitters are started the way the day-end movies' STB particle objects start them
// (JStudio_JParticle::TAdaptor_particle): in the draw group the STB names, following a joint of
// the ship every frame (setGlobalSRTMatrix of the joint's world matrix), and ended by the script,
// not by themselves. The take-off script (SceneTakeoff.cpp) decides when.
//
// ParticleMgr and TPkEffectMgr are global singletons BaseGameSection creates for itself; the
// scene creates them in its own heap and deletes both on exit, so the pointers are null again
// when the next section starts (R8).
#pragma once

#include "types.h"
#include "Vector3.h"
#include "Matrixf.h"

struct Graphics;
struct JKRArchive;
struct JPABaseEmitter;
struct JPAResourceManager;
struct Viewport;

namespace LauncherScene {

struct SceneLightMgr;
struct StageInfo;
struct StageFrame;

enum DemoEffectSet {
	DEMO_Beam,    // s01_dayend/efx.jpc
	DEMO_Liftoff, // s02_dayend_result/efx.jpc
	DEMO_SetCount,
};

struct SceneEffects {
	SceneEffects();

	// DVD thread. Returns false (and the scene runs without particles) when the base resources are
	// missing; a missing take-off set only drops its emitters.
	bool load();
	// Main thread, once the scene is built: the area's ambient emitter.
	void start(const StageInfo& stage, const StageFrame& frame);
	// Per frame, after the camera: `eye` (world) hides the ambient particles that drift too close
	// to the lens; `dt` (scene seconds) runs the emitters' timers and the particle steps.
	void update(const Vector3f& eye, f32 dt);
	// Particle steps per second of scene time (60 by default: the game's own rate; the day-end
	// movies are authored at 30). Steps come from an accumulator, at most 3 per frame; 0 steps
	// once per update whatever dt is. Changing the rate keeps the accumulator.
	void setStepHz(f32 hz) { mStepHz = hz; }
	f32 stepHz() const { return mStepHz; }
	// The next update steps the particles at least once even if the accumulator says no (a cut:
	// the near cull and newly started emitters are current in the cut's first picture).
	void forceStep() { mForceStep = true; }
	// Draw groups in gameplay's order (BaseGameSection::drawParticle): 0, 3-5 (demo, before),
	// 1, then fog off for 6-8 (demo, after) and 2.
	void draw(Graphics& gfx, Viewport* vp, SceneLightMgr* light);
	// Stars in the sky, 0 (none, the default) to 1: the take-off's last shot fades them in as the
	// sky darkens toward space. Drawn first in draw(), depth-tested behind everything.
	void setStars(f32 alpha) { mStarAlpha = alpha; }
	void exit();

	// Starts emitter `id` of a take-off set in draw group `group`. With `follow` it tracks that
	// matrix (a joint's world matrix) every frame; otherwise it stays at `at`. `seconds` > 0 ends
	// it after that long (the STB's own length), 0 runs it until stop(). `scale` sizes the
	// emitter and its particles (the movies' cameras stand much farther back than this one).
	// Returns a handle for stop(), or -1 when the set or the emitter is not available.
	int startDemo(int set, u16 id, u8 group, const Matrixf* follow, const Matrixf* at = nullptr, f32 seconds = 0.0f, f32 scale = 1.0f);
	// Lets an emitter fade out: no new particles, the live ones finish (ParticleMgr::fade).
	void stop(int handle);
	void stopSet(int set);
	// Scales the emitter's particle alpha (JPABaseEmitter::setGlobalAlpha).
	void setAlpha(int handle, u8 alpha);
	// Recolours emitter `id` of a take-off set for every emitter started from it afterwards: its
	// primary and environment colours (0xRRGGBB; alphas stay as authored) and, for a colour
	// animation, every frame of it, keeping a quarter of each frame's own brightness so the
	// animation still reads.
	void tint(int set, u16 id, u32 prm, u32 env);
	bool hasSet(int set) const { return mDemo[set] != nullptr; }

	// The take-off's own lift-off animation for the ship (s02_ufo.bck), null when missing.
	void* liftoffAnim() const { return mLiftoffBck; }
	// The Onions' flight clips of the same movie (s02_onyonr/y/b.bck), `colour` 0 red, 1 yellow,
	// 2 blue; null when missing. Sampled for their root height, never bound to a model.
	enum { ONION_Red, ONION_Yellow, ONION_Blue, ONION_Count };
	void* onionFlightAnim(int colour) const { return colour >= 0 && colour < ONION_Count ? mOnionFlightBck[colour] : nullptr; }

	bool mActive;
	JKRArchive* mCourseInArc;          // s00_coursein/demo.szs
	JPAResourceManager* mDemoResource; // its efx.jpc, in slot 7
	JPABaseEmitter* mAmbient;

private:
	enum { kMaxTracked = 32 };
	struct Tracked {
		JPABaseEmitter* mEmitter; // null: free
		const Matrixf* mFollow;
		f32 mTimeLeft; // seconds; <= 0 runs until stopped
		f32 mScale;
	};

	void loadDemo(int set, const char* path, u8 slot);
	void release(int handle);
	static void place(JPABaseEmitter* emitter, const Matrixf& mtx, f32 scale);
	void drawStars(Viewport* vp);

	JKRArchive* mDemoArc[DEMO_SetCount];
	JPAResourceManager* mDemo[DEMO_SetCount];
	void* mLiftoffBck;
	void* mOnionFlightBck[ONION_Count];
	Tracked mTracked[kMaxTracked];
	f32 mStepHz;
	f32 mStepAcc; // scene seconds not yet stepped
	bool mForceStep;
	f32 mStarAlpha;
};

} // namespace LauncherScene
