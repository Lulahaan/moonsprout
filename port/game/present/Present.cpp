// Game side of the frame pacing contract (p2_host.h, "Frame pacing"): the tick's game time, the
// sound system's per-frame work, and Smooth Motion (fps_design 7, stages 2a, 3b and 3c).
//
// Smooth Motion draws in-between pictures while the host waits for the next 30 Hz tick. At the end
// of every tick the inputs of the draw matrices of each J3D model that entered the draw buffers
// (edit G4) and the view of every camera are copied ("captured"), with what the direct draws read:
// the cylinder shadows' world matrices, the life gauges' and carry numbers' positions, and each
// particle's position, scale and rotation. An in-between pass writes lerp(previous capture, newest
// capture, alpha) into the live objects, redoes the view calculation, draws the section once more,
// and copies the live values back. The game's logic never sees an in-between value: no game code
// runs between the write and the restore except the section's draw, which edit G6 keeps from
// emptying the draw buffers. The play cameras no longer step in the draw at all (M5.1b: they step
// at the start of the tick, port/game/net/tick.cpp), so the capture at the end of a tick holds that
// tick's camera and the in-between camera interpolates two stored tick states.
//
// Safety: a capture holds raw pointers. The models' are used only while sCurAlive says no update
// has run since they were captured (an update may delete them); every other object is reached by
// walking the game's live lists in the pass and looking its key up in the captures, never through
// a captured pointer.
//
// Still steps at 30 Hz: material and texture animation, visibility, HUD (2D), particles born this
// tick (they appear where they were born), flock items (bugs, weeds) and buried sprouts, and models
// that skin on the CPU (none seen in field or cave).
//
// This file is built with the game's options: its operator new goes to JKRHeap, so it allocates
// only with malloc (the game's heaps must not see the host's bookkeeping).
#include "System.h"
#include "PSSystem/PSSystemIF.h"
#include "Camera.h"
#include "Graphics.h"
#include "GameFlow.h"
#include "ResetManager.h"
#include "wipe.h"
#include "Viewport.h"
#include "SysShape/Model.h"
#include "Game/BaseGameSection.h"
#include "Game/GameSystem.h"
#include "Game/MoviePlayer.h"
#include "Game/SingleGame.h"
#include "Game/SingleGameSection.h"
#include "Game/VsGame.h"
#include "Game/VsGameSection.h"
#include "Game/shadowMgr.h"
#include "CarryInfo.h"
#include "LifeGaugeMgr.h"
#include "Light.h"
#include "Game/GameLight.h"
#include "Game/Cave/RandMapMgr.h"
#include "JSystem/J2D/J2DGrafContext.h"
#include "JSystem/J2D/J2DPane.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DMtxBuffer.h"
#include "JSystem/J3D/J3DSys.h"
#include "JSystem/JFramework/JFWDisplay.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "ParticleMgr.h"
#include "JSystem/JUtility/JUTFader.h"
#include "JSystem/JUtility/JUTGamePad.h"
#include "JSystem/JUtility/JUTVideo.h"
#include "Dolphin/os.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void* MEM1Start; // Aurora (lib/dolphin/os/OSMemory.cpp): the emulated main memory
extern void* MEM1End;
extern "C" void p2_host_block_begin(); // port/sdk/os_thread.cpp: other game threads may run between
extern "C" void p2_host_block_end();

namespace {

// ---- malloc-backed growable array of trivially copyable values ----
template <typename T>
struct Buf {
	T* p       = nullptr;
	size_t n   = 0;
	size_t cap = 0;

	void clear() { n = 0; }
	void reserve(size_t want)
	{
		if (want <= cap) {
			return;
		}
		size_t c = cap != 0 ? cap * 2 : 256;
		while (c < want) {
			c *= 2;
		}
		p   = static_cast<T*>(realloc(p, c * sizeof(T)));
		cap = c;
	}
	T* grow(size_t k)
	{
		reserve(n + k);
		T* r = p + n;
		n += k;
		return r;
	}
	void push(const T& v) { *grow(1) = v; }
	T& operator[](size_t i) { return p[i]; }
};

// ---- Captured state ----
// Per model: [posMtx 12][scale 3][world matrices nJ x 12][envelope matrices nW x 12] floats.
struct Snap {
	J3DModel* model;
	J3DModelData* data;
	J3DMtxBuffer* mb; // with data: tells a model apart from a new one at a freed one's address
	size_t off;
	u16 nJ, nW;
	bool lerpable; // no CPU skinning: the draw matrices are the whole picture of the model
};

// Direct-draw state (stage 3b/3c): what the draw reads besides the models' matrices, keyed by the
// object that holds it and checked against its owner. The pass walks the live lists and only looks
// the keys up here, so a capture never makes it follow a pointer to something since freed.
enum RecKind {
	kRecShadow,      // ShadowNode: the cylinder's world SRT
	kRecJointShadow, // JointShadowNode: its world matrix (mMainMtx)
	kRecGauge,       // LifeGaugeList: position
	kRecCarry,       // CarryInfoList: position, rise, scale
	kRecParticle,    // JPABaseParticle: position, scale, rotation, age
	kRecSingle,      // a single-frame particle, by its place in its emitter's list: the same
	kRecKinds
};

struct Rec {
	const void* key;
	const void* owner;
	float v[12];
};

struct Frame {
	Buf<Snap> snaps; // sorted by model
	Buf<float> data;
	Buf<Rec> recs[kRecKinds]; // each sorted by key
	void clear()
	{
		snaps.clear();
		data.clear();
		for (Buf<Rec>& r : recs) {
			r.clear();
		}
	}
};

struct CamSnap {
	Viewport* vp;
	Camera* cam;
	float view[12];
	f32 angle;
};

enum { kMaxCams = 4 };

Frame sFrames[2];
int sNewest = 0;  // sFrames[sNewest] is the newest capture, sFrames[sNewest ^ 1] the one before
int sHistory = 0; // captures in a row in this section (0..2)
CamSnap sCams[2][kMaxCams];
int sCamCount[2] = { 0, 0 };
Buf<J3DModel*> sNoted; // models that entered the draw buffers in this tick's update
bool sEnabled = false;
bool sInUpdate = false; // between the tick's draw (System::endRender, edit G3) and its end
// The newest capture's models are still alive: set at the tick's end, cleared when the next update
// starts (it may delete them) and whenever the history is dropped. A pass never runs without it.
bool sCurAlive = false;
Game::BaseGameSection* sSection = nullptr;

// Pass bookkeeping
Buf<float> sSave;   // live values replaced by the pass
Buf<void*> sPtrs;   // draw/normal matrix buffer pointers swapped by viewCalc
int sLerped = 0;    // models written by the last pass (log/check)

// What apply() did to each model, so restore() undoes exactly that.
struct Act {
	J3DModel* m;
	J3DMtxBuffer* mb;
	size_t save;  // in sSave: internal view (12), then the replaced matrices when lerped
	size_t ptrs;  // in sPtrs, or SIZE_MAX when the draw matrices are not double-buffered
	u16 nJ, nW;
	bool lerped;
};
Buf<Act> sActs;

// Direct-draw values written by the pass: where, and the bytes that were there.
struct DSave {
	void* dst;
	u32 n;
	unsigned char bytes[48];
};
Buf<DSave> sDSave;
int sDirectLerped = 0; // direct-draw objects written by the last pass (check log)
int sPtclUnpaired = 0, sPtclSingle = 0; // debug log: particles drawn at the tick state, of them single-frame

void dsave(void* dst, u32 n)
{
	DSave* d = sDSave.grow(1);
	d->dst   = dst;
	d->n     = n;
	memcpy(d->bytes, dst, n);
}

constexpr float kTeleport2   = 300.0f * 300.0f; // a model root moving further than this snaps
constexpr float kSingleMatch2 = 40.0f * 40.0f;  // a one-frame particle further from its stand-in is a new one
constexpr float kCamJump2    = 400.0f * 400.0f; // camera cut: position jump
constexpr float kCamTurnCos  = 0.70710678f;     // camera cut: more than 45 degrees of turn
constexpr float kMtxTurnFrac = 1.0f;            // per matrix: a basis vector turning > 60 degrees snaps

int cmp_model(const void* a, const void* b)
{
	const uintptr_t x = reinterpret_cast<uintptr_t>(*static_cast<J3DModel* const*>(a));
	const uintptr_t y = reinterpret_cast<uintptr_t>(*static_cast<J3DModel* const*>(b));
	return x < y ? -1 : (x > y ? 1 : 0);
}

const Snap* find_snap(Frame& f, J3DModel* m)
{
	size_t lo = 0, hi = f.snaps.n;
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		J3DModel* k      = f.snaps[mid].model;
		if (k == m) {
			return &f.snaps[mid];
		}
		if (reinterpret_cast<uintptr_t>(k) < reinterpret_cast<uintptr_t>(m)) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return nullptr;
}

void drop_history()
{
	sFrames[0].clear();
	sFrames[1].clear();
	sCamCount[0] = sCamCount[1] = 0;
	sHistory                    = 0;
	sCurAlive                   = false;
}

const Rec* find_rec(const Buf<Rec>& b, const void* key)
{
	size_t lo = 0, hi = b.n;
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		const void* k    = b.p[mid].key;
		if (k == key) {
			return &b.p[mid];
		}
		if (reinterpret_cast<uintptr_t>(k) < reinterpret_cast<uintptr_t>(key)) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return nullptr;
}

int cmp_rec(const void* a, const void* b)
{
	const uintptr_t x = reinterpret_cast<uintptr_t>(static_cast<const Rec*>(a)->key);
	const uintptr_t y = reinterpret_cast<uintptr_t>(static_cast<const Rec*>(b)->key);
	return x < y ? -1 : (x > y ? 1 : 0);
}

void sort_recs(Buf<Rec>& b)
{
	if (b.n > 1) {
		qsort(b.p, b.n, sizeof(Rec), cmp_rec);
	}
}

// Itanium C++ ABI: an object's first word points two slots into its class's vtable. The game is
// built without RTTI, so this is how a live section is told apart from a stale Game::gameSystem
// (it is never cleared when a game section ends, and its memory is reused).
#if defined(__GNUC__) || defined(__clang__)
extern "C" void* _ZTVN4Game17SingleGameSectionE[];
extern "C" void* _ZTVN4Game13VsGameSectionE[];
int section_kind(void* s) // 1 story, 2 challenge / versus, 0 other
{
	void* vptr = *static_cast<void**>(s);
	return vptr == &_ZTVN4Game17SingleGameSectionE[2] ? 1 : (vptr == &_ZTVN4Game13VsGameSectionE[2] ? 2 : 0);
}
#else
int section_kind(void*) { return 0; }
#endif

// The running section when it is a game section in its field or cave play state (not the world
// map, loading, day end, results or a save screen, whose managers may not exist), else nullptr.
Game::BaseGameSection* game_section()
{
	if (sys == nullptr || sys->mGameFlow == nullptr) {
		return nullptr;
	}
	ISection* cur = sys->mGameFlow->getCurrentSection();
	if (cur == nullptr) {
		return nullptr;
	}
	const int kind = section_kind(cur);
	if (kind == 1) {
		Game::SingleGameSection* sgs = static_cast<Game::SingleGameSection*>(static_cast<Section*>(cur));
		Game::SingleGame::State* st  = sgs->mCurrentState;
		if (sgs->mDoEnd || st == nullptr) {
			return nullptr;
		}
		if (st->mId == Game::SingleGame::SGS_Game) {
			Game::SingleGame::GameState* gs = static_cast<Game::SingleGame::GameState*>(st);
			if (gs->mDoExit || gs->mInSaveScreen) {
				return nullptr;
			}
		} else if (st->mId == Game::SingleGame::SGS_Cave) {
			Game::SingleGame::CaveState* cs = static_cast<Game::SingleGame::CaveState*>(st);
			if (cs->mFadeout || cs->mDrawSave) {
				return nullptr;
			}
		} else {
			return nullptr;
		}
	} else if (kind == 2) {
		Game::VsGameSection* vgs = static_cast<Game::VsGameSection*>(static_cast<Section*>(cur));
		Game::VsGame::State* st  = vgs->mCurrentState;
		if (st == nullptr || st->mId != Game::VsGame::VGS_Game
		    || static_cast<Game::VsGame::GameState*>(st)->isFlag(Game::VsGame::VSGS_IsSectionFadeout)) {
			return nullptr;
		}
	} else {
		return nullptr;
	}
	Game::BaseGameSection* sec = static_cast<Game::BaseGameSection*>(static_cast<Section*>(cur));
	if (Game::gameSystem == nullptr || Game::gameSystem->getSection() != sec) {
		return nullptr;
	}
	return sec;
}

// fps_design 7.1: no movie, not paused or frozen, no DVD error, no reset or fade in progress, one
// viewport, and nothing that a draw would do only once (one-shot screen captures).
Game::BaseGameSection* guarded_section()
{
	Game::BaseGameSection* sec = game_section();
	if (sec == nullptr) {
		return nullptr;
	}
	Game::GameSystem* gs = Game::gameSystem;
	if (gs->isMultiplayerMode() || gs->paused() || gs->mIsFrozen || gs->mIsMoviePause) {
		return nullptr;
	}
	if (Game::moviePlayer != nullptr
	    && (Game::moviePlayer->mDemoState != Game::DEMOSTATE_Inactive || Game::moviePlayer->isFlag(Game::MVP_IsActive))) {
		return nullptr;
	}
	if (sys->isDvdErrorOccured()) {
		return nullptr;
	}
	if ((sys->mResetMgr != nullptr && sys->mResetMgr->mState != ResetManager::ResetState_Inactive)
	    || JUTGamePad::C3ButtonReset::sResetOccurred) {
		return nullptr;
	}
	if (!sec->mIsMainActive || sec->mDisplay == nullptr) {
		return nullptr;
	}
	if (sec->mDisplayWiper != nullptr && sec->mDisplayWiper->mIsActive) { // drawn by endRender only
		return nullptr;
	}
	JUTFader* fader = sec->mDisplay->mFader;
	if (fader != nullptr && fader->getStatus() != JUTFader::Status_In) {
		return nullptr;
	}
	if ((sec->mXfbFlags & 2) != 0) { // the pause-menu backdrop is captured by the next real draw
		return nullptr;
	}
	if (Game::Cave::randMapMgr != nullptr && Game::Cave::randMapMgr->mIsCaptureOn) {
		return nullptr;
	}
	return sec;
}

// ---- Capture ----
void capture_model(Frame& f, J3DModel* m)
{
	J3DModelData* d  = m->mModelData;
	J3DMtxBuffer* mb = m->mMtxBuffer;
	if (d == nullptr || mb == nullptr || mb->mWorldMatrices == nullptr) {
		return;
	}
	Snap s;
	s.model    = m;
	s.data     = d;
	s.mb       = mb;
	s.nJ       = d->getJointNum();
	s.nW       = mb->mWeightEnvelopeMatrices != nullptr ? d->getWEvlpMtxNum() : 0;
	s.lerpable = m->mSkinDeform == nullptr && !m->checkFlag(J3DMODEL_SkinPosCpu);
	s.off      = f.data.n;
	float* out = f.data.grow(15 + 12 * (static_cast<size_t>(s.nJ) + s.nW));
	memcpy(out, m->mPosMtx, 12 * sizeof(float));
	out[12] = m->mModelScale.x;
	out[13] = m->mModelScale.y;
	out[14] = m->mModelScale.z;
	memcpy(out + 15, mb->mWorldMatrices, s.nJ * sizeof(Mtx));
	if (s.nW != 0) {
		memcpy(out + 15 + 12 * s.nJ, mb->mWeightEnvelopeMatrices, s.nW * sizeof(Mtx));
	}
	f.snaps.push(s);
}

void capture_cameras(int slot)
{
	Graphics* gfx = sys->mGfx;
	int n         = 0;
	for (int i = 0; i < gfx->mActiveViewports && n < kMaxCams; i++) {
		Viewport* vp = gfx->getViewport(i);
		if (vp == nullptr || !vp->viewable() || vp->mCamera == nullptr || vp->mCamera->mViewMatrix == nullptr) {
			continue;
		}
		CamSnap& c = sCams[slot][n++];
		c.vp       = vp;
		c.cam      = vp->mCamera;
		memcpy(c.view, vp->mCamera->mViewMatrix->mMatrix.mtxView, sizeof(c.view));
		c.angle = vp->mCamera->mViewAngle;
	}
	sCamCount[slot] = n;
}

// ---- Lerp ----
inline float dist2(float ax, float ay, float az, float bx, float by, float bz)
{
	return (ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz);
}

// 3x4 row-major matrices. A basis vector that turned too far (an animation cut, a flip) makes the
// matrix snap to the newer one instead of showing a squashed in-between.
void lerp_mtx(const float* a, const float* b, float t, float* out)
{
	for (int c = 0; c < 3; c++) {
		const float d  = dist2(a[c], a[4 + c], a[8 + c], b[c], b[4 + c], b[8 + c]);
		const float nb = b[c] * b[c] + b[4 + c] * b[4 + c] + b[8 + c] * b[8 + c];
		if (d > nb * kMtxTurnFrac) {
			memcpy(out, b, 12 * sizeof(float));
			return;
		}
	}
	for (int i = 0; i < 12; i++) {
		out[i] = a[i] + (b[i] - a[i]) * t;
	}
}

// Camera position and forward vector from a view matrix [R | t]: position = -R^T t.
void view_pose(const float* v, float* pos, float* fwd)
{
	for (int i = 0; i < 3; i++) {
		pos[i] = -(v[0 * 4 + i] * v[3] + v[1 * 4 + i] * v[7] + v[2 * 4 + i] * v[11]);
		fwd[i] = v[2 * 4 + i];
	}
}

// ---- Direct draw (stage 3b) and particles (stage 3c) ----
// 3x4 affine inverse (row-major [A | t]): [A^-1 | -A^-1 t]. False when A is singular.
bool invert_affine(const float* m, float* out)
{
	const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
	const float c0 = e * k - f * h, c1 = f * g - d * k, c2 = d * h - e * g;
	const float det = a * c0 + b * c1 + c * c2;
	if (fabsf(det) < 1e-12f) {
		return false;
	}
	const float r = 1.0f / det;
	out[0]  = c0 * r;
	out[1]  = (c * h - b * k) * r;
	out[2]  = (b * f - c * e) * r;
	out[4]  = c1 * r;
	out[5]  = (a * k - c * g) * r;
	out[6]  = (c * d - a * f) * r;
	out[8]  = c2 * r;
	out[9]  = (b * g - a * h) * r;
	out[10] = (a * e - b * d) * r;
	for (int i = 0; i < 3; i++) {
		out[i * 4 + 3] = -(out[i * 4 + 0] * m[3] + out[i * 4 + 1] * m[7] + out[i * 4 + 2] * m[11]);
	}
	return true;
}

// out = x * y (3x4 affine, row-major); out may not alias.
void mul34(const float* x, const float* y, float* out)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			out[r * 4 + c] = x[r * 4 + 0] * y[0 * 4 + c] + x[r * 4 + 1] * y[1 * 4 + c] + x[r * 4 + 2] * y[2 * 4 + c]
			               + (c == 3 ? x[r * 4 + 3] : 0.0f);
		}
	}
}

// The shadow managers' viewports that ShadowMgr::update writes (it skips the others).
template <typename F>
void for_shadow_viewports(Game::ShadowMgr* sm, F&& f)
{
	const int n = sm->getViewportCount() < 2 ? sm->getViewportCount() : 2;
	for (int i = 0; i < n; i++) {
		Viewport* vp = sm->getViewport(i);
		if (vp != nullptr && vp->viewable() && vp->getCamera() != nullptr) {
			f(i, vp->getCamera()->getViewMatrix(false)->mMatrix.mtxView[0]);
		}
	}
}

inline const void* single_key(const void* list, uintptr_t index)
{
	static_assert(sizeof(uintptr_t) == 8, "single-frame particle keys need 64-bit pointers");
	return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(list) | ((index + 1) << 48));
}

template <typename F>
void for_particles(F&& f)
{
	if (particleMgr == nullptr || particleMgr->mEmitterManager == nullptr) {
		return;
	}
	JPAEmitterManager* mgr = particleMgr->mEmitterManager;
	for (u8 g = 0; g < mgr->mGrpMax; g++) {
		for (JSULink<JPABaseEmitter>* l = mgr->mGrpEmtr[g].getFirst(); l != nullptr; l = l->getNext()) {
			JPABaseEmitter* e = l->getObject();
			JPAList<JPABaseParticle>* lists[2] = { &e->mAlivePtclBase, &e->mAlivePtclChld };
			for (JPAList<JPABaseParticle>* list : lists) {
				uintptr_t single = 0; // single-frame particles before this one in the list
				for (JPANode<JPABaseParticle>* n = list->mFirst; n != nullptr; n = n->mNext) {
					// A particle that lives one frame is born again every tick: its stand-in in the
					// last capture is the one in the same place among the list's single-frame ones.
					// The key is the list's address with the index in the top 16 bits: user-space
					// addresses stay below 2^48, so it never meets another list's keys or a real
					// particle's address (a plain list + index ran into the child list's keys).
					const void* slot = n->mData.mLifeTime == 1 ? single_key(list, single++) : nullptr;
					f(e, &n->mData, slot);
				}
			}
		}
	}
}

// At the tick's end, after its update: what the direct draws will read.
void capture_direct(Frame& f)
{
	// Test hook: MPPM_PRESENT_NO_DIRECT=1 leaves every direct draw and particle at the tick's state
	// (stage 2a), for side-by-side screenshots.
	static const bool sOff = getenv("MPPM_PRESENT_NO_DIRECT") != nullptr;
	if (sOff) {
		return;
	}
	// Cylinder shadows. ShadowMgr::update (in the update, with the camera the tick's draw set)
	// left view * SRT in each node; the world SRT is the inverse view times that.
	Game::ShadowMgr* sm = Game::shadowMgr;
	if (sm != nullptr && sm->mEnabled && sm->getViewportCount() > 0) {
		Viewport* vp = sm->getViewport(0);
		float inv[12];
		if (vp != nullptr && vp->viewable() && vp->getCamera() != nullptr
		    && invert_affine(vp->getCamera()->getViewMatrix(false)->mMatrix.mtxView[0], inv)) {
			FOREACH_NODE(Game::ShadowNode, sm->mActiveShadows->getNext(), node)
			{
				if (node->mCreature != nullptr && node->mCreature->needShadow()) {
					Rec* r   = f.recs[kRecShadow].grow(1);
					r->key   = node;
					r->owner = node->mCreature;
					mul34(inv, node->getMtx(0)->mMatrix.mtxView[0], r->v);
				}
			}
		}
		FOREACH_NODE(Game::JointShadowRootNode, sm->mActiveJointShadows->getNext(), root)
		{
			if (root->mCreature == nullptr || !root->mCreature->needShadow()) {
				continue;
			}
			FOREACH_NODE(Game::JointShadowNode, root->getNext(), child)
			{
				if (child->mMainMtx != nullptr) {
					Rec* r   = f.recs[kRecJointShadow].grow(1);
					r->key   = child;
					r->owner = root->mCreature;
					memcpy(r->v, child->getMtxA()->mMatrix.mtxView, 12 * sizeof(float));
				}
			}
		}
	}
	if (lifeGaugeMgr != nullptr) {
		for (LifeGaugeList* l = lifeGaugeMgr->mListActive.mNext; l != nullptr; l = l->mNext) {
			Rec* r   = f.recs[kRecGauge].grow(1);
			r->key   = l;
			r->owner = l->mGameObject;
			r->v[0]  = l->mParam.mPosition.x;
			r->v[1]  = l->mParam.mPosition.y;
			r->v[2]  = l->mParam.mPosition.z;
		}
	}
	if (carryInfoMgr != nullptr) {
		for (CarryInfoList* l = carryInfoMgr->mActiveList.mNext; l != nullptr; l = static_cast<CarryInfoList*>(l->mNext)) {
			Rec* r   = f.recs[kRecCarry].grow(1);
			r->key   = l;
			r->owner = l->mOwner;
			r->v[0]  = l->mParam.mPosition.x;
			r->v[1]  = l->mParam.mPosition.y;
			r->v[2]  = l->mParam.mPosition.z;
			r->v[3]  = l->mParam.mCarryInfo.mYOffset;
			r->v[4]  = l->mParam.mCarryInfo.mScale;
		}
	}
	for_particles([&f](JPABaseEmitter* e, JPABaseParticle* p, const void* slot) {
		Rec* r   = f.recs[slot != nullptr ? kRecSingle : kRecParticle].grow(1);
		r->key   = slot != nullptr ? slot : p;
		r->owner = e;
		r->v[0]  = p->mPosition.x;
		r->v[1]  = p->mPosition.y;
		r->v[2]  = p->mPosition.z;
		r->v[3]  = p->mParticleScaleX;
		r->v[4]  = p->mParticleScaleY;
		r->v[5]  = p->mRotateAngle;
		r->v[6]  = p->mAge;
	});
	for (Buf<Rec>& b : f.recs) {
		sort_recs(b);
	}
}

// The two captures of an object, when it existed in both with the same owner.
bool rec_pair(int kind, const void* key, const void* owner, const Rec*& a, const Rec*& b)
{
	b = find_rec(sFrames[sNewest].recs[kind], key);
	if (b == nullptr || b->owner != owner) {
		return false;
	}
	a = find_rec(sFrames[sNewest ^ 1].recs[kind], key);
	return a != nullptr && a->owner == owner;
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Life gauges, carry numbers and particles: positions written into the live objects (restored
// after the pass). Walks the live lists: only objects that exist now are touched.
void apply_direct(float alpha)
{
	sDSave.clear();
	sDirectLerped = 0;
	// Lights placed from the camera in the draw (LightMgr::set): put back after the pass.
	if (sSection != nullptr && sSection->mLightMgr != nullptr) {
		FOREACH_NODE(LightObj, sSection->mLightMgr->mLightObjChain.mChild, light)
		{
			dsave(&light->mPosition, sizeof(Vector3f) * 2); // mPosition, mElevation
		}
	}
	const Rec *a, *b;
	if (lifeGaugeMgr != nullptr) {
		for (LifeGaugeList* l = lifeGaugeMgr->mListActive.mNext; l != nullptr; l = l->mNext) {
			if (!rec_pair(kRecGauge, l, l->mGameObject, a, b) || dist2(a->v[0], a->v[1], a->v[2], b->v[0], b->v[1], b->v[2]) > kTeleport2) {
				continue;
			}
			dsave(&l->mParam.mPosition, sizeof(Vector3f));
			l->mParam.mPosition.x = lerpf(a->v[0], b->v[0], alpha);
			l->mParam.mPosition.y = lerpf(a->v[1], b->v[1], alpha);
			l->mParam.mPosition.z = lerpf(a->v[2], b->v[2], alpha);
			sDirectLerped++;
		}
	}
	if (carryInfoMgr != nullptr) {
		for (CarryInfoList* l = carryInfoMgr->mActiveList.mNext; l != nullptr; l = static_cast<CarryInfoList*>(l->mNext)) {
			if (!rec_pair(kRecCarry, l, l->mOwner, a, b) || dist2(a->v[0], a->v[1], a->v[2], b->v[0], b->v[1], b->v[2]) > kTeleport2) {
				continue;
			}
			dsave(&l->mParam.mPosition, sizeof(Vector3f));
			dsave(&l->mParam.mCarryInfo.mYOffset, 2 * sizeof(f32)); // mYOffset, mScale
			l->mParam.mPosition.x           = lerpf(a->v[0], b->v[0], alpha);
			l->mParam.mPosition.y           = lerpf(a->v[1], b->v[1], alpha);
			l->mParam.mPosition.z           = lerpf(a->v[2], b->v[2], alpha);
			l->mParam.mCarryInfo.mYOffset   = lerpf(a->v[3], b->v[3], alpha);
			l->mParam.mCarryInfo.mScale     = lerpf(a->v[4], b->v[4], alpha);
			sDirectLerped++;
		}
	}
	// Particles: one that is a tick older than its last capture (not a new one in a recycled
	// slot) moves between its two positions; a new one appears where it was born.
	sPtclUnpaired = sPtclSingle = 0;
	for_particles([alpha](JPABaseEmitter* e, JPABaseParticle* p, const void* slot) {
		const Rec *pa, *pb;
		if (slot != nullptr) {
			// Single-frame: the stand-in must be one too, and the newest capture must be this
			// particle (same place).
			if (!rec_pair(kRecSingle, slot, e, pa, pb) || pb->v[0] != p->mPosition.x || pb->v[1] != p->mPosition.y
			    || pb->v[2] != p->mPosition.z) {
				sPtclUnpaired++;
				sPtclSingle++;
				return;
			}
		} else {
			if (!rec_pair(kRecParticle, p, e, pa, pb)) {
				sPtclUnpaired++;
				return;
			}
			const float age = p->mAge;
			if (pb->v[6] != age || !(pa->v[6] < age && age <= pa->v[6] + 2.0f)) {
				return;
			}
		}
		// Single-frame particles are paired by their place in the list only: an emitter that
		// respawns them at random spots would pair unrelated ones, so they must be close.
		if (dist2(pa->v[0], pa->v[1], pa->v[2], pb->v[0], pb->v[1], pb->v[2]) > (slot != nullptr ? kSingleMatch2 : kTeleport2)) {
			return;
		}
		dsave(&p->mPosition, sizeof(p->mPosition));
		dsave(&p->mParticleScaleX, 2 * sizeof(f32)); // mParticleScaleX, mParticleScaleY
		dsave(&p->mRotateAngle, sizeof(u16));
		p->mPosition.x      = lerpf(pa->v[0], pb->v[0], alpha);
		p->mPosition.y      = lerpf(pa->v[1], pb->v[1], alpha);
		p->mPosition.z      = lerpf(pa->v[2], pb->v[2], alpha);
		p->mParticleScaleX  = lerpf(pa->v[3], pb->v[3], alpha);
		p->mParticleScaleY  = lerpf(pa->v[4], pb->v[4], alpha);
		int turn            = (static_cast<int>(pb->v[5]) - static_cast<int>(pa->v[5]) + 32768) & 65535;
		turn -= 32768;
		p->mRotateAngle     = static_cast<u16>(static_cast<int>(pa->v[5]) + static_cast<int>(lrintf(turn * alpha)));
		sDirectLerped++;
	});
}

// Everything ShadowMgr::update writes, saved before the pass's own update and restored with the
// other direct-draw values: the tick's draw then finds exactly what the tick's update left (a fresh
// update after the pass would see the tick's end state, not the state its update ran in).
void save_shadows()
{
	Game::ShadowMgr* sm = Game::shadowMgr;
	if (sm == nullptr || !sm->mEnabled) {
		return;
	}
	const int n            = sm->getViewportCount() < 2 ? sm->getViewportCount() : 2;
	Game::CylinderBase* cy = sm->getActiveCylinder();
	if (cy != nullptr) {
		dsave(cy->mCamPosition, sizeof(cy->mCamPosition));
		dsave(cy->mCamLookAt, sizeof(cy->mCamLookAt));
		dsave(cy->mCameraSizeMod, sizeof(cy->mCameraSizeMod));
	}
	FOREACH_NODE(Game::ShadowNode, sm->mActiveShadows->getNext(), node)
	{
		dsave(&node->mFlags, sizeof(node->mFlags));
		for (int i = 0; i < n; i++) {
			dsave(node->getMtx(i), sizeof(Matrixf));
		}
	}
	FOREACH_NODE(Game::JointShadowRootNode, sm->mActiveJointShadows->getNext(), root)
	{
		FOREACH_NODE(Game::JointShadowNode, root->getNext(), child)
		{
			for (int i = 0; i < n; i++) {
				dsave(child->getMtxB(i), sizeof(Matrixf));
			}
		}
	}
}

// Cylinder shadows, after the pass's ShadowMgr::update (which wrote view * the tick's SRT with the
// in-between camera): view * lerp(SRT).
void apply_shadows(float alpha)
{
	Game::ShadowMgr* sm = Game::shadowMgr;
	if (sm == nullptr || !sm->mEnabled) {
		return;
	}
	const Rec *a, *b;
	float w[12];
	FOREACH_NODE(Game::ShadowNode, sm->mActiveShadows->getNext(), node)
	{
		if (!rec_pair(kRecShadow, node, node->mCreature, a, b) || !node->mCreature->needShadow()
		    || dist2(a->v[3], a->v[7], a->v[11], b->v[3], b->v[7], b->v[11]) > kTeleport2) {
			continue;
		}
		lerp_mtx(a->v, b->v, alpha, w);
		for_shadow_viewports(sm, [&](int i, const float* view) { mul34(view, w, node->getMtx(i)->mMatrix.mtxView[0]); });
		sDirectLerped++;
	}
	FOREACH_NODE(Game::JointShadowRootNode, sm->mActiveJointShadows->getNext(), root)
	{
		if (root->mCreature == nullptr || !root->mCreature->needShadow()) {
			continue;
		}
		FOREACH_NODE(Game::JointShadowNode, root->getNext(), child)
		{
			if (!rec_pair(kRecJointShadow, child, root->mCreature, a, b)
			    || dist2(a->v[3], a->v[7], a->v[11], b->v[3], b->v[7], b->v[11]) > kTeleport2) {
				continue;
			}
			lerp_mtx(a->v, b->v, alpha, w);
			for_shadow_viewports(sm, [&](int i, const float* view) { mul34(view, w, child->getMtxB(i)->mMatrix.mtxView[0]); });
			sDirectLerped++;
		}
	}
}

// ---- Pass state: apply the lerp, restore the live values ----
struct CamSave {
	Camera* cam;
	Matrixf* viewPtr;
	float cur[12];
	float view[12];
	f32 angle;
	// The whole camera as the tick left it: the pass's setProjection rewrites its projection
	// matrix from the in-between view angle, and other view-derived fields follow the view.
	alignas(16) unsigned char bytes[sizeof(Camera)];
};
CamSave sCamSave[kMaxCams];
int sCamSaveN = 0;

void apply(float alpha)
{
	Frame& cur  = sFrames[sNewest];
	Frame& prev = sFrames[sNewest ^ 1];
	sSave.clear();
	sPtrs.clear();
	sActs.clear();
	sLerped = 0;
	float tmp[12];
	for (size_t i = 0; i < cur.snaps.n; i++) {
		const Snap& s    = cur.snaps[i];
		J3DModel* m      = s.model;
		J3DMtxBuffer* mb = m->mMtxBuffer;
		if (mb == nullptr) {
			continue;
		}
		// The model is alive (sCurAlive); only the one captured is lerped.
		const bool same = m->mModelData == s.data && mb == s.mb;
		Act* act   = sActs.grow(1);
		act->m     = m;
		act->mb    = mb;
		act->nJ    = s.nJ;
		act->nW    = s.nW;
		act->lerped = false;
		act->ptrs  = SIZE_MAX;
		// viewCalc swaps the draw and normal matrix buffers: the pass's results go to the other
		// buffer, and the tick's stay where the next real draw finds them.
		if (mb->mDrawMatrices[0] != mb->mDrawMatrices[1]) {
			act->ptrs = sPtrs.n;
			for (u32 v = 0; v < mb->mViewCount; v++) {
				sPtrs.push(mb->mDrawMatrices[0][v]);
				sPtrs.push(mb->mDrawMatrices[1][v]);
				sPtrs.push(mb->mNormMatrices[0][v]);
				sPtrs.push(mb->mNormMatrices[1][v]);
			}
		}
		act->save   = sSave.n;
		float* save = sSave.grow(12);
		memcpy(save, m->mInternalView, 12 * sizeof(float));
		const Snap* p = s.lerpable && same ? find_snap(prev, m) : nullptr;
		if (p == nullptr || !p->lerpable || p->data != s.data || p->mb != s.mb || p->nJ != s.nJ || p->nW != s.nW) {
			continue; // new this tick, or not lerpable: drawn at the tick state
		}
		const float* a = prev.data.p + p->off;
		const float* b = cur.data.p + s.off;
		if (dist2(a[3], a[7], a[11], b[3], b[7], b[11]) > kTeleport2
		    || (s.nJ != 0 && dist2(a[15 + 3], a[15 + 7], a[15 + 11], b[15 + 3], b[15 + 7], b[15 + 11]) > kTeleport2)) {
			continue;
		}
		const size_t count = 15 + 12 * (static_cast<size_t>(s.nJ) + s.nW);
		if (memcmp(a, b, count * sizeof(float)) == 0) {
			continue; // did not move
		}
		// Save the live values, then write the in-between.
		act->lerped = true;
		float* live = sSave.grow(count);
		memcpy(live, m->mPosMtx, 12 * sizeof(float));
		live[12] = m->mModelScale.x;
		live[13] = m->mModelScale.y;
		live[14] = m->mModelScale.z;
		memcpy(live + 15, mb->mWorldMatrices, s.nJ * sizeof(Mtx));
		if (s.nW != 0) {
			memcpy(live + 15 + 12 * s.nJ, mb->mWeightEnvelopeMatrices, s.nW * sizeof(Mtx));
		}
		lerp_mtx(a, b, alpha, tmp);
		memcpy(m->mPosMtx, tmp, sizeof(tmp));
		m->mModelScale.x = a[12] + (b[12] - a[12]) * alpha;
		m->mModelScale.y = a[13] + (b[13] - a[13]) * alpha;
		m->mModelScale.z = a[14] + (b[14] - a[14]) * alpha;
		float* world = &mb->mWorldMatrices[0][0][0];
		for (u16 j = 0; j < s.nJ; j++) {
			lerp_mtx(a + 15 + 12 * j, b + 15 + 12 * j, alpha, world + 12 * j);
		}
		if (s.nW != 0) {
			float* env = &mb->mWeightEnvelopeMatrices[0][0][0];
			for (u16 j = 0; j < s.nW; j++) {
				lerp_mtx(a + 15 + 12 * (s.nJ + j), b + 15 + 12 * (s.nJ + j), alpha, env + 12 * j);
			}
		}
		sLerped++;
	}

	// Cameras: the draw reads the view from mCurViewMatrix and the view calculation from
	// *mViewMatrix. Since M5.1b both hold the newest tick's view between ticks (the camera steps in
	// doUpdate and the tick copies the stepped view into mCurViewMatrix), so writing the in-between
	// into both and restoring them afterwards leaves the tick's view in place.
	sCamSaveN        = 0;
	const int newest = sNewest;
	for (int i = 0; i < sCamCount[newest]; i++) {
		const CamSnap& b = sCams[newest][i];
		Camera* cam      = b.cam;
		if (b.vp->mCamera != cam || cam->mViewMatrix == nullptr) {
			continue;
		}
		CamSave& sv = sCamSave[sCamSaveN++];
		sv.cam      = cam;
		sv.viewPtr  = cam->mViewMatrix;
		memcpy(sv.cur, cam->mCurViewMatrix.mMatrix.mtxView, sizeof(sv.cur));
		memcpy(sv.view, cam->mViewMatrix->mMatrix.mtxView, sizeof(sv.view));
		sv.angle = cam->mViewAngle;
		memcpy(sv.bytes, static_cast<void*>(cam), sizeof(Camera));
		// The newest view, or the in-between when the camera did not cut.
		float out[12];
		memcpy(out, b.view, sizeof(out));
		float angle = b.angle;
		for (int k = 0; k < sCamCount[newest ^ 1]; k++) {
			const CamSnap& a = sCams[newest ^ 1][k];
			if (a.cam != cam || a.vp != b.vp) {
				continue;
			}
			float pa[3], fa[3], pb[3], fb[3];
			view_pose(a.view, pa, fa);
			view_pose(b.view, pb, fb);
			const float turn = fa[0] * fb[0] + fa[1] * fb[1] + fa[2] * fb[2];
			if (dist2(pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]) <= kCamJump2 && turn >= kCamTurnCos) {
				for (int e = 0; e < 12; e++) {
					out[e] = a.view[e] + (b.view[e] - a.view[e]) * alpha;
				}
				angle = a.angle + (b.angle - a.angle) * alpha;
			}
			break;
		}
		memcpy(cam->mCurViewMatrix.mMatrix.mtxView, out, sizeof(out));
		memcpy(cam->mViewMatrix->mMatrix.mtxView, out, sizeof(out));
		cam->mViewAngle = angle;
	}

	apply_direct(alpha);
}

void restore()
{
	for (int i = sCamSaveN - 1; i >= 0; i--) {
		CamSave& sv = sCamSave[i];
		memcpy(sv.viewPtr->mMatrix.mtxView, sv.view, sizeof(sv.view));
		memcpy(sv.cam->mCurViewMatrix.mMatrix.mtxView, sv.cur, sizeof(sv.cur));
		sv.cam->mViewAngle = sv.angle;
		memcpy(static_cast<void*>(sv.cam), sv.bytes, sizeof(Camera));
	}
	sCamSaveN = 0;

	// Undo what apply() recorded.
	for (size_t i = 0; i < sActs.n; i++) {
		const Act& act   = sActs[i];
		J3DModel* m      = act.m;
		J3DMtxBuffer* mb = act.mb;
		if (act.ptrs != SIZE_MAX) {
			size_t pi = act.ptrs;
			for (u32 v = 0; v < mb->mViewCount; v++) {
				mb->mDrawMatrices[0][v] = static_cast<Mtx*>(sPtrs[pi++]);
				mb->mDrawMatrices[1][v] = static_cast<Mtx*>(sPtrs[pi++]);
				mb->mNormMatrices[0][v] = static_cast<Mtx33*>(sPtrs[pi++]);
				mb->mNormMatrices[1][v] = static_cast<Mtx33*>(sPtrs[pi++]);
			}
		}
		const float* save = sSave.p + act.save;
		memcpy(m->mInternalView, save, 12 * sizeof(float));
		if (!act.lerped) {
			continue;
		}
		const float* live = save + 12;
		memcpy(m->mPosMtx, live, 12 * sizeof(float));
		m->mModelScale.x = live[12];
		m->mModelScale.y = live[13];
		m->mModelScale.z = live[14];
		memcpy(mb->mWorldMatrices, live + 15, act.nJ * sizeof(Mtx));
		if (act.nW != 0) {
			memcpy(mb->mWeightEnvelopeMatrices, live + 15 + 12 * act.nJ, act.nW * sizeof(Mtx));
		}
	}
	sActs.clear();

	// Direct-draw values, newest write last: restore backwards.
	for (size_t i = sDSave.n; i-- > 0;) {
		memcpy(sDSave[i].dst, sDSave[i].bytes, sDSave[i].n);
	}
	sDSave.clear();
}

// ---- Purity check (fps_design 7.5, MPPM_PRESENT_CHECK=1, or 2 for every range of stage 1) ----
// Once a second: copy MEM1 and a few host globals, run the pass, and diff (stage 1: everything the
// pass wrote and did not restore). Then copy again, run the same pass once more, and diff again
// (stage 2: state that moves on with every draw, such as a counter or a buffer swap, as opposed to
// matrices and GX caches that a draw rewrites to the same values). Ranges registered as render-only
// are not reported; the rest are grouped by the JKRExpHeap block they fall in. A run is bad when
// stage 2 finds anything or a global changed.
struct Range {
	uintptr_t lo, hi;
};
int sCheckMode   = -1;
unsigned char* sMemCopy = nullptr;
size_t sMemSize  = 0;
time_t sLastCheck = 0;
Buf<Range> sRenderOnly;
unsigned long sCheckRuns = 0, sCheckBad = 0, sStage1Total = 0, sStage2Total = 0;

bool check_wanted()
{
	if (sCheckMode < 0) {
		const char* e = getenv("MPPM_PRESENT_CHECK");
		sCheckMode    = (e != nullptr && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
	}
	if (sCheckMode == 0) {
		return false;
	}
	const time_t now = time(nullptr);
	if (now == sLastCheck) {
		return false;
	}
	sLastCheck = now;
	return true;
}

void add_render_only(const void* p, size_t size)
{
	if (p != nullptr && size != 0) {
		const uintptr_t lo = reinterpret_cast<uintptr_t>(p);
		sRenderOnly.push(Range { lo, lo + size });
	}
}

// What a draw writes on every call and the next real draw writes again before reading: per model,
// both draw/normal matrix buffers, the shape and material packets, the shapes' current matrix
// pointers, and the matrix buffer's swapped pointer arrays.
void register_render_only()
{
	sRenderOnly.clear();
	Frame& cur = sFrames[sNewest];
	for (size_t i = 0; i < cur.snaps.n; i++) {
		J3DModel* m      = cur.snaps[i].model;
		J3DModelData* d  = m->mModelData;
		J3DMtxBuffer* mb = m->mMtxBuffer;
		const u32 nd     = d->getDrawMtxNum();
		if (mb->mDrawMatrices[0] != mb->mDrawMatrices[1]) {
			for (int k = 0; k < 2; k++) {
				add_render_only(mb->mDrawMatrices[k], mb->mViewCount * sizeof(void*));
				add_render_only(mb->mNormMatrices[k], mb->mViewCount * sizeof(void*));
				for (u32 v = 0; v < mb->mViewCount; v++) {
					add_render_only(mb->mDrawMatrices[k][v], nd * sizeof(Mtx));
					add_render_only(mb->mNormMatrices[k][v], nd * sizeof(Mtx33));
				}
			}
		}
		add_render_only(m->mShapePackets, d->getShapeNum() * sizeof(J3DShapePacket));
		add_render_only(m->mMatPackets, d->getMaterialNum() * sizeof(J3DMatPacket));
		for (u16 k = 0; k < d->getShapeNum(); k++) {
			add_render_only(d->getShapeNodePointer(k), sizeof(J3DShape));
		}
	}
}

#if defined(__GNUC__) || defined(__clang__)
extern "C" void* _ZTV12J3DMtxBuffer[];
#endif

bool in_mem1(const void* p, size_t size)
{
	const uintptr_t a = reinterpret_cast<uintptr_t>(p), lo = reinterpret_cast<uintptr_t>(MEM1Start),
	                hi = reinterpret_cast<uintptr_t>(MEM1End);
	return a >= lo && a <= hi && size <= hi - a;
}

// Every J3DMtxBuffer in MEM1 (found by its vtable): the draw and normal matrices of all models,
// including the ones that were not entered this tick but still had their view calculated (their
// buffers swap, and the next view calculation rewrites them before any draw reads them).
void register_all_mtx_buffers()
{
#if defined(__GNUC__) || defined(__clang__)
	const void* vptr         = &_ZTV12J3DMtxBuffer[2];
	const unsigned char* mem = static_cast<const unsigned char*>(MEM1Start);
	for (size_t k = 0; k + sizeof(J3DMtxBuffer) <= sMemSize; k += 8) {
		const void* v;
		memcpy(&v, mem + k, sizeof(v));
		if (v != vptr) {
			continue;
		}
		const J3DMtxBuffer* mb = reinterpret_cast<const J3DMtxBuffer*>(mem + k); // Itanium: vptr first
		// Freed memory keeps old vtable words: check every pointer before following it.
		if (mb->mViewCount == 0 || mb->mViewCount > 4 || !in_mem1(mb->mJointTree, sizeof(J3DJointTree))
		    || mb->mDrawMatrices[0] == mb->mDrawMatrices[1]) {
			continue;
		}
		bool sane = true;
		for (int b = 0; b < 2 && sane; b++) {
			sane = in_mem1(mb->mDrawMatrices[b], mb->mViewCount * sizeof(void*))
			    && in_mem1(mb->mNormMatrices[b], mb->mViewCount * sizeof(void*));
		}
		if (!sane) {
			continue;
		}
		const u32 nd = mb->mJointTree->getDrawMtxNum();
		if (nd > 4096) {
			continue;
		}
		for (int b = 0; b < 2; b++) {
			add_render_only(mb->mDrawMatrices[b], mb->mViewCount * sizeof(void*));
			add_render_only(mb->mNormMatrices[b], mb->mViewCount * sizeof(void*));
			for (u32 v2 = 0; v2 < mb->mViewCount; v2++) {
				add_render_only(mb->mDrawMatrices[b][v2], nd * sizeof(Mtx));
				add_render_only(mb->mNormMatrices[b][v2], nd * sizeof(Mtx33));
			}
		}
	}
#endif
}

// Particles: every draw renormalises each particle's mBaseAxis in place (the direction shapes of
// JPABaseShape), which moves it by an ulp at most, and uses the manager's work data as scratch.
void register_particles()
{
	if (particleMgr == nullptr || particleMgr->mEmitterManager == nullptr) {
		return;
	}
	JPAEmitterManager* mgr = particleMgr->mEmitterManager;
	add_render_only(mgr->mWorkData, sizeof(JPAEmitterWorkData));
	for (u8 g = 0; g < mgr->mGrpMax; g++) {
		for (JSULink<JPABaseEmitter>* l = mgr->mGrpEmtr[g].getFirst(); l != nullptr; l = l->getNext()) {
			JPABaseEmitter* e = l->getObject();
			JPAList<JPABaseParticle>* lists[2] = { &e->mAlivePtclBase, &e->mAlivePtclChld };
			for (JPAList<JPABaseParticle>* list : lists) {
				for (JPANode<JPABaseParticle>* n = list->mFirst; n != nullptr; n = n->mNext) {
					add_render_only(&n->mData.mBaseAxis, sizeof(n->mData.mBaseAxis));
				}
			}
		}
	}
}

// GXLoadTexObj writes the texture map slot into the top byte of a GXTexObj's register words (as
// the GameCube SDK does): which slot depends on draw order, and every load rewrites it.
bool gx_slot_bytes(const unsigned char* now, const unsigned char* was, size_t i, size_t end, uintptr_t base)
{
	for (size_t k = i; k < end; k++) {
		if (now[k] != was[k] && ((base + k) & 3) != 3) {
			return false;
		}
	}
	return true;
}

// Aurora's GXInitTexObj gives the object a new texObjId (u32 at +0x30) from a global counter;
// textures that are initialised in draw (shadow cylinders, screen captures) change it every draw.
// Recognised by the register words in front of it (mode0 and image0 carry BP register ids once
// loaded).
bool gx_texobj_id(const unsigned char* now, size_t i, size_t end)
{
	const size_t a = i & ~static_cast<size_t>(3);
	if (end > a + 4 || a < 0x30) {
		return false;
	}
	u32 mode0, image0;
	memcpy(&mode0, now + a - 0x30, 4);
	memcpy(&image0, now + a - 0x28, 4);
	const u32 m = mode0 >> 24, im = image0 >> 24;
	const bool modeOk  = m == 0 || (m >= 0x80 && m <= 0x83) || (m >= 0xa0 && m <= 0xa3); // 0: never loaded
	const bool imageOk = (im >= 0x88 && im <= 0x8b) || (im >= 0xa8 && im <= 0xab) || im == 0xff; // 0xff: never loaded
	return modeOk && imageOk;
}

// J2DPane::draw writes each drawn pane's global bounds and matrix from its parent's and its own
// position: a pure function of what the update left, which the next real draw writes again before
// any update can read it. A pane is recognised by its tree link pointing back at it.
bool j2d_pane_output(const JKRExpHeap::CMemBlock* b, uintptr_t lo, uintptr_t hi)
{
	if (b == nullptr || static_cast<size_t>(b->mAllocatedSpace) < sizeof(J2DPane)) {
		return false;
	}
	const uintptr_t start = reinterpret_cast<uintptr_t>(b) + sizeof(*b);
	const J2DPane* pane   = reinterpret_cast<const J2DPane*>(start);
	if (!in_mem1(pane, sizeof(J2DPane)) || pane->mTree.getObject() != pane) {
		return false;
	}
	// Also the alpha it draws with (parent's times its own), which follows the matrix.
	const uintptr_t gb = reinterpret_cast<uintptr_t>(&pane->mGlobalBounds);
	const uintptr_t gm = reinterpret_cast<uintptr_t>(&pane->mGlobalMtx);
	const uintptr_t ca = reinterpret_cast<uintptr_t>(&pane->mColorAlpha) + sizeof(pane->mColorAlpha);
	return (lo >= gb && hi <= gb + sizeof(pane->mGlobalBounds)) || (lo >= gm && hi <= ca);
}

bool render_only(uintptr_t lo, uintptr_t hi)
{
	for (size_t i = 0; i < sRenderOnly.n; i++) {
		if (lo >= sRenderOnly[i].lo && hi <= sRenderOnly[i].hi) {
			return true;
		}
	}
	return false;
}

// The JKRExpHeap block holding `addr`: the nearest valid header before it whose size covers it.
const JKRExpHeap::CMemBlock* block_of(uintptr_t addr)
{
	const uintptr_t base = reinterpret_cast<uintptr_t>(MEM1Start);
	uintptr_t a          = addr & ~static_cast<uintptr_t>(15);
	for (int steps = 0; steps < 65536 && a >= base + 16; steps++, a -= 16) {
		const JKRExpHeap::CMemBlock* b = reinterpret_cast<const JKRExpHeap::CMemBlock*>(a);
		if (b->isValid() && b->mAllocatedSpace > 0) {
			const uintptr_t start = a + sizeof(JKRExpHeap::CMemBlock);
			if (addr >= start && addr < start + static_cast<uintptr_t>(b->mAllocatedSpace)) {
				return b;
			}
		}
	}
	return nullptr;
}

struct Globals {
	f32 deltaTime;
	alignas(16) unsigned char j3d[sizeof(J3DSys)]; // raw bytes: no J3DSys constructor runs
	u8 viewCalcMode;
};
Globals sGlobalsBefore;

void check_begin()
{
	sMemSize = reinterpret_cast<uintptr_t>(MEM1End) - reinterpret_cast<uintptr_t>(MEM1Start);
	if (sMemCopy == nullptr) {
		sMemCopy = static_cast<unsigned char*>(malloc(sMemSize));
	}
	if (sMemCopy != nullptr) {
		memcpy(sMemCopy, MEM1Start, sMemSize);
	}
	sGlobalsBefore.deltaTime    = sys->mDeltaTime;
	sGlobalsBefore.viewCalcMode = SysShape::Model::viewCalcMode;
	memcpy(sGlobalsBefore.j3d, &j3dSys, sizeof(J3DSys));
}

struct DiffResult {
	int ranges, unexplained;
	size_t bytes;
};

// Blocks that change with no pass at all (audio buffers, OS thread records, the retrace counter:
// other host threads write them). Found by stage 0 and never reported.
Buf<Range> sNoise;
bool in_noise(uintptr_t lo)
{
	for (size_t i = 0; i < sNoise.n; i++) {
		if (lo >= sNoise[i].lo && lo < sNoise[i].hi) {
			return true;
		}
	}
	return false;
}

// What stage 2 found, by kind: block size, offset in the block and the block's first word (a vtable
// for most objects), summed over the session and printed every 30 checks.
struct Category {
	int stage;
	int size;
	size_t offset;
	const void* first;
	unsigned long count;
	size_t bytes;
};
Buf<Category> sCategories;
void note_category(int stage, int size, size_t offset, const void* first, size_t bytes)
{
	for (size_t i = 0; i < sCategories.n; i++) {
		Category& c = sCategories[i];
		if (c.stage == stage && c.size == size && c.offset == offset && c.first == first) {
			c.count++;
			c.bytes += bytes;
			return;
		}
	}
	if (sCategories.n < 4096) {
		sCategories.push(Category { stage, size, offset, first, 1, bytes });
	}
}

DiffResult check_diff(int stage, bool verbose)
{
	DiffResult res = { 0, 0, 0 };
	if (sMemCopy == nullptr) {
		return res;
	}
	if (stage != 0) {
		register_render_only();
		register_all_mtx_buffers();
		register_particles();
	}
	const unsigned char* now = static_cast<const unsigned char*>(MEM1Start);
	const uintptr_t base     = reinterpret_cast<uintptr_t>(MEM1Start);
	int ranges = 0, unexplained = 0;
	size_t unexplainedBytes = 0;
	const JKRExpHeap::CMemBlock* lastBlock = nullptr;
	size_t i = 0;
	while (i < sMemSize) {
		// Skip equal 8-byte words quickly.
		if (i + 8 <= sMemSize && memcmp(now + i, sMemCopy + i, 8) == 0) {
			i += 8;
			continue;
		}
		if (now[i] == sMemCopy[i]) {
			i++;
			continue;
		}
		size_t j = i + 1, same = 0;
		while (j < sMemSize && same < 32) {
			same = now[j] == sMemCopy[j] ? same + 1 : 0;
			j++;
		}
		const size_t end = j - same;
		const uintptr_t lo = base + i, hi = base + end;
		if (stage == 0) {
			const JKRExpHeap::CMemBlock* b = block_of(lo);
			const uintptr_t start = b != nullptr ? reinterpret_cast<uintptr_t>(b) + sizeof(*b) : lo & ~static_cast<uintptr_t>(4095);
			const uintptr_t stop  = b != nullptr ? start + static_cast<uintptr_t>(b->mAllocatedSpace) : start + 4096;
			if (!in_noise(lo) && sNoise.n < 8192) {
				sNoise.push(Range { start, stop });
			}
			ranges++;
			i = end;
			continue;
		}
		if (in_noise(lo)) {
			i = end;
			continue;
		}
		ranges++;
		if (!render_only(lo, hi) && !gx_slot_bytes(now, sMemCopy, i, end, base) && !gx_texobj_id(now, i, end)
		    && !j2d_pane_output(block_of(lo), lo, hi)) {
			unexplained++;
			unexplainedBytes += end - i;
			const JKRExpHeap::CMemBlock* b = block_of(lo);
			{
				const uintptr_t start = b != nullptr ? reinterpret_cast<uintptr_t>(b) + sizeof(*b) : 0;
				note_category(stage, b != nullptr ? b->mAllocatedSpace : 0, b != nullptr ? lo - start : lo - base,
				              b != nullptr ? *reinterpret_cast<void* const*>(start) : nullptr, end - i);
			}
			if (verbose && b != lastBlock && unexplained <= 40) {
				const uintptr_t start = b != nullptr ? reinterpret_cast<uintptr_t>(b) + sizeof(*b) : 0;
				const void* vptr      = b != nullptr ? *reinterpret_cast<void* const*>(start) : nullptr;
				fprintf(stderr,
				        "mppm: present check: stage %d changed 0x%07zx +%zu (block 0x%07zx size %d +0x%zx, first word %p)\n",
				        stage, static_cast<size_t>(lo - base), end - i, b != nullptr ? static_cast<size_t>(start - base) : 0,
				        b != nullptr ? b->mAllocatedSpace : 0, b != nullptr ? static_cast<size_t>(lo - start) : 0, vptr);
				char hex[2][3 * 16 + 1];
				const size_t shown = end - i < 16 ? end - i : 16;
				for (size_t k = 0; k < shown; k++) {
					snprintf(hex[0] + 3 * k, 4, "%02x ", sMemCopy[i + k]);
					snprintf(hex[1] + 3 * k, 4, "%02x ", now[i + k]);
				}
				hex[0][3 * shown] = hex[1][3 * shown] = '\0';
				fprintf(stderr, "mppm: present check:   was %s\nmppm: present check:   now %s\n", hex[0], hex[1]);
				// Who points at this block: the owner's block and its first word (often its vtable).
				if (stage == 2 && i >= 0x40) {
					char ctx[3 * 0x48 + 1];
					for (size_t k = 0; k < 0x48; k++) {
						snprintf(ctx + 3 * k, 4, "%02x ", now[i - 0x40 + k]);
					}
					fprintf(stderr, "mppm: present check:   context -0x40: %s\n", ctx);
				}
				if (stage == 2 && (b == nullptr || static_cast<size_t>(b->mAllocatedSpace) > sMemSize)) {
					// No sane block: list pointers to just before the address (an object start).
					int found = 0;
					for (size_t k = 0; k + 8 <= sMemSize && found < 6; k += 8) {
						uintptr_t v;
						memcpy(&v, now + k, sizeof(v));
						if (v <= lo && v + 1024 > lo) {
							const JKRExpHeap::CMemBlock* ob = block_of(base + k);
							const uintptr_t os = ob != nullptr ? reinterpret_cast<uintptr_t>(ob) + sizeof(*ob) : 0;
							fprintf(stderr, "mppm: present check:   pointer to -0x%zx from 0x%07zx (block size %d +0x%zx, first word %p)\n",
							        static_cast<size_t>(lo - v), k, ob != nullptr ? ob->mAllocatedSpace : 0,
							        ob != nullptr ? base + k - os : 0, ob != nullptr ? *reinterpret_cast<void* const*>(os) : nullptr);
							found++;
						}
					}
				}
				if (b != nullptr && stage == 2) {
					const uintptr_t want = start;
					int found            = 0;
					for (size_t k = 0; k + 8 <= sMemSize && found < 3; k += 8) {
						uintptr_t v;
						memcpy(&v, now + k, sizeof(v));
						if (v == want) {
							const JKRExpHeap::CMemBlock* ob = block_of(base + k);
							const uintptr_t os = ob != nullptr ? reinterpret_cast<uintptr_t>(ob) + sizeof(*ob) : 0;
							fprintf(stderr, "mppm: present check:   referenced from 0x%07zx (block size %d +0x%zx, first word %p)\n",
							        k, ob != nullptr ? ob->mAllocatedSpace : 0, ob != nullptr ? base + k - os : 0,
							        ob != nullptr ? *reinterpret_cast<void* const*>(os) : nullptr);
							found++;
						}
					}
				}
			}
			lastBlock = b;
		}
		i = end;
	}
	res.ranges      = ranges;
	res.unexplained = unexplained;
	res.bytes       = unexplainedBytes;
	return res;
}

void check_end(const DiffResult& s1)
{
	const DiffResult s2 = check_diff(2, sCheckMode == 2);
	int globals = 0;
	if (sys->mDeltaTime != sGlobalsBefore.deltaTime) {
		globals++;
		fprintf(stderr, "mppm: present check: sys->mDeltaTime changed\n");
	}
	if (SysShape::Model::viewCalcMode != sGlobalsBefore.viewCalcMode) {
		globals++;
		fprintf(stderr, "mppm: present check: SysShape::Model::viewCalcMode changed\n");
	}
	if (memcmp(sGlobalsBefore.j3d, &j3dSys, sizeof(J3DSys)) != 0) {
		globals++;
		fprintf(stderr, "mppm: present check: j3dSys changed\n");
	}
	sCheckRuns++;
	if (sCheckRuns % 30 == 0) {
		fprintf(stderr, "mppm: present check: kinds after %lu checks (stage, count, bytes, block size, offset, first word):\n",
		        sCheckRuns);
		for (size_t k = 0; k < sCategories.n; k++) {
			const Category& c = sCategories[k];
			if (c.count >= 2) {
				fprintf(stderr, "mppm: present check:   %d %5lu %7zu  size %8d +0x%-6zx %p\n", c.stage, c.count, c.bytes,
				        c.size, c.offset, c.first);
			}
		}
	}
	sStage1Total += s1.unexplained;
	sStage2Total += s2.unexplained;
	// The design's criterion is zero unexplained ranges at either stage: a stage 1 leftover is a
	// change the pass made that its own restore missed, so it counts too.
	if (s1.unexplained != 0 || s2.unexplained != 0 || globals != 0) {
		sCheckBad++;
	}
	fprintf(stderr,
	        "mppm: present check %lu: stage 1 %d ranges (%d render-only, %d other, %zu bytes); stage 2 %d ranges (%d "
	        "render-only, %d other, %zu bytes); %d globals; %zu models, %d lerped, %d direct; bad runs %lu, other ranges so far "
	        "%lu / %lu\n",
	        sCheckRuns, s1.ranges, s1.ranges - s1.unexplained, s1.unexplained, s1.bytes, s2.ranges,
	        s2.ranges - s2.unexplained, s2.unexplained, s2.bytes, globals, sFrames[sNewest].snaps.n, sLerped, sDirectLerped, sCheckBad,
	        sStage1Total, sStage2Total);
}

void draw_pass(Game::BaseGameSection* sec, float alpha);

} // namespace

extern "C" {

P2PresentState gP2Present = { 0 };

float p2_game_tick_seconds(void)
{
	return sys != nullptr ? sys->mDeltaTime : 0.0f;
}

// Edit G3: the host decides how many sound frames this tick owes (1 at the original pacing).
void p2_game_sound_main_loop(void* sysif)
{
	PSSystem::SysIF* sif = static_cast<PSSystem::SysIF*>(sysif);
	if (sif == nullptr) {
		return;
	}
	for (int n = p2_host_sound_frames(); n > 0; n--) {
		sif->mainLoop();
	}
	// The tick's draw is done and its update follows: models entered from here on are the ones
	// the next draw shows, and they live until then. Models entered by a draw (2D screens' own 3D
	// models) may be gone by the tick's end, so they are not listed. The update may delete what the
	// newest capture holds: no pass may use it from here until the next capture.
	sInUpdate = true;
	sCurAlive = false;
}

// Host -> game (port/main.cpp declares it): Smooth Motion is wanted for the coming ticks. Off drops
// the history and stops listing models.
void p2_game_present_enable(int on)
{
	if (sEnabled && !on) {
		drop_history();
		sNoted.clear();
		sSection  = nullptr;
		sInUpdate = false;
	}
	sEnabled = on != 0;
	if (!sEnabled) {
		sCurAlive = false;
	}
}

int p2_game_present_ready(void)
{
	return sEnabled && sCurAlive && sHistory >= 2 && guarded_section() == sSection && sSection != nullptr;
}

// End of a tick (after update): capture S(N) and C(N).
void p2_game_present_tick_end(void)
{
	const bool sawUpdate = sInUpdate;
	sInUpdate            = false;
	sCurAlive            = false;
	if (!sEnabled) {
		return;
	}
	Game::BaseGameSection* sec = guarded_section();
	if (sec == nullptr || sec != sSection || !sawUpdate) {
		// A new section (or state): the listed models may belong to the one that just ended and
		// be freed already. Start again from the next tick, which lists only its own.
		drop_history();
		sSection = sec;
		sNoted.clear();
		return;
	}
	sNewest ^= 1;
	Frame& f = sFrames[sNewest];
	f.clear();
	if (sNoted.n > 1) {
		qsort(sNoted.p, sNoted.n, sizeof(J3DModel*), cmp_model);
	}
	J3DModel* last = nullptr;
	for (size_t i = 0; i < sNoted.n; i++) {
		if (sNoted[i] != last) {
			last = sNoted[i];
			capture_model(f, last);
		}
	}
	sNoted.clear();
	capture_direct(f);
	capture_cameras(sNewest);
	if (sHistory < 2) {
		sHistory++;
	}
	sCurAlive = true;
}

// One in-between picture at `alpha` (0 = the previous tick's state, 1 = the newest) into the open
// Aurora frame. Returns 0, having drawn nothing, when it may not run.
int p2_game_present(float alpha)
{
	if (!p2_game_present_ready()) {
		return 0;
	}
	Game::BaseGameSection* sec = sSection;
	// Below 0 only from the MPPM_SMOOTH_ALPHA test hook: extrapolating backwards magnifies whatever
	// does not follow the lerped state.
	alpha                      = alpha < -8.0f ? -8.0f : (alpha > 1.0f ? 1.0f : alpha);

	const bool check = check_wanted();
	static const bool sDebug = getenv("MPPM_PRESENT_DEBUG") != nullptr;
	if (sDebug) {
		static time_t last = 0;
		const time_t now   = time(nullptr);
		if (now != last) {
			last = now;
			const Frame& c = sFrames[sNewest];
			const Frame& p = sFrames[sNewest ^ 1];
			int cpu        = 0;
			for (size_t i = 0; i < c.snaps.n; i++) {
				cpu += c.snaps.p[i].lerpable ? 0 : 1;
			}
			fprintf(stderr, "mppm: present debug: models %zu/%zu (%d skinned on the CPU) shadows %zu/%zu joint %zu/%zu gauges %zu/%zu carry %zu/%zu particles %zu+%zu/%zu+%zu (%d unpaired, %d single-frame); last pass %d lerped, %d direct\n",
			        c.snaps.n, p.snaps.n, cpu, c.recs[kRecShadow].n, p.recs[kRecShadow].n, c.recs[kRecJointShadow].n,
			        p.recs[kRecJointShadow].n, c.recs[kRecGauge].n, p.recs[kRecGauge].n, c.recs[kRecCarry].n,
			        p.recs[kRecCarry].n, c.recs[kRecParticle].n, c.recs[kRecSingle].n, p.recs[kRecParticle].n, p.recs[kRecSingle].n, sPtclUnpaired, sPtclSingle, sLerped, sDirectLerped);
		}
	}
	if (!check) {
		draw_pass(sec, alpha);
		return 1;
	}
	// Stage 0: what changes with no pass, over about the time of one, with the other game threads
	// (sound, DVD) allowed to run as they may while a pass waits on the GPU.
	check_begin();
	{
		p2_host_block_begin();
		const struct timespec ts = { 0, 20000000 }; // more than one retrace
		nanosleep(&ts, nullptr);
		p2_host_block_end();
	}
	check_diff(0, false);
	// Interrupts stay queued while the passes run and are diffed: their handlers (audio frames,
	// which drive the BGM sequencer) would otherwise run at the pass's safe points and show up as
	// changes the pass did not make. Normal passes leave interrupts alone, as the tick's draw does.
	const BOOL level = OSDisableInterrupts();
	memcpy(sMemCopy, MEM1Start, sMemSize);
	draw_pass(sec, alpha);
	const DiffResult s1 = check_diff(1, sCheckMode == 2);
	memcpy(sMemCopy, MEM1Start, sMemSize);
	draw_pass(sec, alpha); // overdraws the same picture
	check_end(s1);
	OSRestoreInterrupts(level);
	return 1;
}

// Edit G4 (J3DModel::entry): the model entered the draw buffers.
void p2_present_note_model(void* model)
{
	if (sEnabled && sInUpdate && !gP2Present.active) {
		sNoted.push(static_cast<J3DModel*>(model));
	}
}

} // extern "C"

namespace {
void draw_pass(Game::BaseGameSection* sec, float alpha)
{
	Graphics& gfx       = *sys->mGfx;
	JFWDisplay* display = sec->mDisplay;
	gP2Present.active   = 1;
	apply(alpha);
	alignas(16) static unsigned char savedSys[sizeof(J3DSys)];
	memcpy(savedSys, &j3dSys, sizeof(J3DSys));
	alignas(16) static unsigned char savedGfx[sizeof(Graphics)];
	memcpy(savedGfx, &gfx, sizeof(Graphics));
	const u8 savedMode = SysShape::Model::viewCalcMode;

	// As beginRender. The EFB is already clear: the pass is the first drawing of a new Aurora frame,
	// as a tick's draw is (JFWDisplay::clearEfb's depth quad does not reach far depth on Aurora).
	display->preGX();
	// The view calculation of BaseGameSection::doUpdate for the models that are not immediate (the
	// immediate ones are calculated by the draw), and the shadow matrices, which hold the view.
	SysShape::Model::setViewCalcModeInd();
	for (int i = 0; i < gfx.mActiveViewports; i++) {
		Viewport* vp = gfx.getViewport(i);
		if (vp != nullptr && vp->viewable()) {
			sec->j3dSetView(vp, false);
		}
	}
	if (Game::shadowMgr != nullptr) {
		save_shadows();
		Game::shadowMgr->update();
		apply_shadows(alpha);
	}
	sec->draw(gfx);
	// As endGX, without the fader (only full-in faders get here) or debug output.
	J2DOrthoGraph ortho(0.0f, 0.0f, JUTVideo::getManager()->getFbWidth(), JUTVideo::getManager()->getEfbHeight(), -1.0f,
	                    1.0f);
	ortho.setPort();
	GXFlush();

	restore(); // also the shadow matrices, as the tick's update left them (save_shadows)
	memcpy(static_cast<void*>(&j3dSys), savedSys, sizeof(J3DSys));
	memcpy(static_cast<void*>(&gfx), savedGfx, sizeof(Graphics));
	SysShape::Model::viewCalcMode = savedMode;
	gP2Present.active             = 0;
}
} // namespace
