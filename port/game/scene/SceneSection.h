// The live start-menu scene as a game section (docs/launcher/SCENE.md 1, 5, 6).
//
// A small section in the style of Demo::Section: it does not use BaseGameSection, gameSystem,
// the creature managers or a save file. Hook H1 (GameFlow::createSection) builds it instead of
// the title while the launcher's scene bridge says a scene is pending; when it ends, GameFlow
// continues with the title screen as usual.
#pragma once

#include "Game/BaseHIO.h"
#include "DvdThreadCommand.h"
#include "Viewport.h"
#include "p2_host.h"

#include "SceneActor.h"
#include "SceneAssets.h"
#include "SceneCamera.h"
#include "SceneDebug.h"
#include "SceneDirector.h"
#include "SceneEffects.h"
#include "SceneLight.h"
#include "SceneOnion.h"
#include "SceneTakeoff.h"
#include "SceneWorld.h"

struct J3DDrawBuffer;

namespace LauncherScene {

struct Section : public Game::BaseHIOSection {
	enum State {
		ST_Load,    // DVD thread loading behind the launcher's opaque backdrop
		ST_Idle,    // the cast idles; the start menu is up
		ST_Takeoff, // Play was pressed: the take-off script runs
		ST_Done,    // faded out; the section ends
		ST_Failed,  // the disc lacked something: draw nothing, wait for Play, end (SCENE.md 4.3)
		ST_Parked,  // Live Start Menu off: loaded (or not) but not drawn and silent; Play ends it
	};

	Section(JKRHeap* heap);
	virtual ~Section();

	virtual void init();
	virtual void doLoadingStart();
	virtual bool doLoading();
	virtual bool doUpdate();
	virtual void doDraw(Graphics& gfx);
	virtual void doExit();
	virtual bool forceReset() { return false; }

	void loadResource(); // DVD thread
	void loadMusic();    // DVD thread: the sound scene of a music swap

private:
	void buildScene();   // main thread, once loading is done
	f32 frameDt();       // this frame's scene seconds (see SceneSection.cpp)
	void updateScene(f32 dt);
	void drawSky(Graphics& gfx);
	f32 groundDetailStrength() const;
	JUtility::TColor skyColour(f32 elevDeg) const;
	void drawFade(Graphics& gfx, f32 alpha);
	void endFreeRun(); // ST_Done and doExit: back to the retrace pacing

	// Live settings (live_options 3.3): the host's config generation changed.
	void applyConfig();
	void park();
	void unpark();
	void stepMusicSwap(); // per frame while a swap runs
	bool swapping() const { return mSwapPhase != SWAP_None; }

	enum SwapPhase { SWAP_None, SWAP_Fading, SWAP_Loading };

	int mState;
	DvdThreadCommand mThreadCommand;
	bool mLoadOk;
	bool mReadySent;
	int mFramesDrawn; // complete frames drawn since loading (READY waits for the first)
	u32 mLastRetrace; // VIGetRetraceCount() at the previous frame
	bool mClockStarted;
	f32 mSpeed;       // MPPM_SCENE_SPEED
	f32 mHold;        // MPPM_SCENE_HOLD (0 = off)

	SceneWorld mWorld;
	Assets mAssets;
	J3DDrawBuffer* mOpaBuffer;
	J3DDrawBuffer* mXluBuffer;
	Viewport mViewport;
	SceneCamera mCamera;
	SceneLightMgr* mLightMgr;
	SceneShadows mShadows;
	MapActor* mMap;
	ShipActor* mShip;
	OnionActor* mOnions[PIKI_ColourCount];
	Actor* mActors[ACTOR_Count];
	Director mDirector;
	Takeoff mTakeoff;
	SceneEffects mEffects;
	SceneDebug* mDebug; // MPPM_SCENE_TUNE only
	bool mFreeRun;      // p2_host_free_run(1) was sent

	bool mLoaded;       // the area's assets are loaded and the scene is built
	bool mReload;       // the section ends to be created again (area change, unpark with no assets)
	unsigned mConfigGen; // p2_launcher_scene_config_gen() last applied in full
	int mLoadedArea;    // the area this section loaded (or tried to)
	int mPendingCmd;    // a Play or Skip that arrived during a music swap
	int mSwapPhase;     // SwapPhase
	int mSwapMusic;     // the track being swapped in
	f32 mSwapTime;      // seconds in SWAP_Fading
	DvdThreadCommand mSwapCommand;
};

} // namespace LauncherScene
