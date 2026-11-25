// Particles of the start-menu scene (docs/launcher/SCENE.md 10.2, 11). Setup as BaseGameSection
// (baseGameSection.cpp:2081-2101) and MoviePlayer (moviePlayer.cpp:456-464) do it; the take-off
// emitters as the day-end movies' STB particle objects drive them
// (JStudio_JParticle/object-particle.cpp, TJPACallback_::execute).
#include "SceneEffects.h"
#include "SceneLight.h"
#include "SceneStage.h"

#include "System.h"
#include "Graphics.h"
#include "Viewport.h"
#include "ParticleMgr.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JParticle/JPAResource.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JParticle/JPAShape.h"
#include "JSystem/J3D/J3DShape.h"
#include "Dolphin/gx.h"

#include <math.h>

namespace LauncherScene {
namespace {

// Room for game.jpc (555 KB, loaded into this heap), the three demo sets and the particles. The
// lift-off's four jets and two plumes are the busiest moment, about a thousand particles.
const u32 kHeapSize     = 0x200000;
const u32 kMaxParticles = 2000;
const u32 kMaxEmitters  = 160;

// Emitter-manager slots of the demo sets (8 slots exist, particleMgr.cpp:109; 0 is game.jpc and 7
// the s00 set ParticleMgr::setDemoResourceManager uses).
const u8 kDemoSlot[DEMO_SetCount] = { 5, 6 };

// The area emitters scatter their particles over the whole map for the arrival movie's high
// camera; drawn in to the landing site they fill the air around the cast instead.
const f32 kAmbientSpread = 0.07f;

// Near cull of the ambient particles. The emitter's volume reaches past the camera, and a petal
// that drifts within a few units of the lens fills a third of the picture as a pink blur over the
// menu (or, during the take-off, over the ship). A petal closer than sNearHide is not drawn; the
// distance is from the eye, so it vanishes while still small on screen, near the picture's edge
// as a rule.
f32 sNearHide = 95.0f; // the area's StageInfo::mAmbientNear

// SceneEffects::setStepHz: most particle steps one frame may take.
const int kMaxStepsPerFrame = 3;
Vector3f sEye(0.0f, 0.0f, 0.0f);

struct NearCull : public JPAParticleCallBack {
	virtual ~NearCull() { }
	virtual void execute(JPABaseEmitter*, JPABaseParticle* p)
	{
		f32 dx = p->mPosition.x - sEye.x;
		f32 dy = p->mPosition.y - sEye.y;
		f32 dz = p->mPosition.z - sEye.z;
		if (dx * dx + dy * dy + dz * dz < sNearHide * sNearHide) {
			p->mFlags |= 8; // JPABaseShape skips invisible particles
		} else {
			p->mFlags &= ~8u;
		}
	}
};
NearCull sNearCull;

// Stars of the take-off's last shot (SceneEffects::setStars): fixed points on the sky above
// kStarMinElev, drawn as small additive soft dots kStarDistance from the eye (inside the far plane,
// behind every craft), fading in toward the horizon.
const int kStarCount      = 420;
const f32 kStarDistance   = 9000.0f;
const f32 kStarSize       = 17.0f;   // dot radius at kStarDistance (soft edge): a 1-3 pixel core at 720p
const f32 kStarMinElev    = 0.14f;   // sin of 8 degrees
const f32 kStarFadeElev   = 0.45f;   // fully bright from here up
struct Star {
	f32 mX, mY, mZ; // unit direction, world
	f32 mSize;      // times kStarSize
	u8 mBright;
};
Star sStars[kStarCount];
bool sStarsMade = false;

void makeStars()
{
	u32 seed = 0x5EED1234u;
	for (int i = 0; i < kStarCount; i++) {
		seed      = seed * 1664525u + 1013904223u;
		f32 r0    = (f32)(seed >> 8) / 16777216.0f;
		seed      = seed * 1664525u + 1013904223u;
		f32 r1    = (f32)(seed >> 8) / 16777216.0f;
		seed      = seed * 1664525u + 1013904223u;
		f32 r2    = (f32)(seed >> 8) / 16777216.0f;
		f32 y     = kStarMinElev + (1.0f - kStarMinElev) * r0; // uniform on the cap
		f32 a     = 6.2831853f * r1;
		f32 h     = sqrtf(1.0f - y * y);
		Star& st  = sStars[i];
		st.mX     = h * cosf(a);
		st.mY     = y;
		st.mZ     = h * sinf(a);
		st.mSize  = 0.6f + 0.9f * r2 * r2;
		st.mBright = (u8)(110 + 145 * r2);
	}
	sStarsMade = true;
}

} // namespace

SceneEffects::SceneEffects()
    : mActive(false)
    , mCourseInArc(nullptr)
    , mDemoResource(nullptr)
    , mAmbient(nullptr)
    , mLiftoffBck(nullptr)
    , mStepHz(60.0f)
    , mStepAcc(0.0f)
    , mForceStep(false)
    , mStarAlpha(0.0f)
{
	for (int i = 0; i < ONION_Count; i++) {
		mOnionFlightBck[i] = nullptr;
	}
	for (int i = 0; i < DEMO_SetCount; i++) {
		mDemoArc[i] = nullptr;
		mDemo[i]    = nullptr;
	}
	for (int i = 0; i < kMaxTracked; i++) {
		mTracked[i].mEmitter = nullptr;
	}
}

bool SceneEffects::load()
{
	mCourseInArc = JKRMountArchive("/user/Mukki/movie/s00_coursein/demo.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	void* efx    = mCourseInArc != nullptr ? mCourseInArc->getResource("efx.jpc") : nullptr;
	if (efx == nullptr || particleMgr != nullptr) {
		OSReport("scene: no particles (%s)\n", efx == nullptr ? "s00_coursein efx.jpc missing" : "a particle manager already exists");
		return false;
	}
	ParticleMgr::globalInstance();
	particleMgr->createHeap(kHeapSize);
	particleMgr->createMgr("user/Ebisawa/effect/game.jpc", kMaxParticles, kMaxEmitters, 0x80);
	particleMgr->Instance_TPkEffectMgr(); // ParticleMgr::update resets its context (SCENE.md 2.3)
	mDemoResource = new (particleMgr->mHeap, 0) JPAResourceManager(efx, particleMgr->mHeap);
	particleMgr->setDemoResourceManager(mDemoResource);
	mActive = true;

	loadDemo(DEMO_Beam, "/user/Mukki/movie/s01_dayend/demo.szs", kDemoSlot[DEMO_Beam]);
	loadDemo(DEMO_Liftoff, "/user/Mukki/movie/s02_dayend_result/demo.szs", kDemoSlot[DEMO_Liftoff]);
	if (mDemoArc[DEMO_Liftoff] != nullptr) {
		mLiftoffBck = mDemoArc[DEMO_Liftoff]->getResource("s02_ufo.bck");
		static const char* const kOnionClips[ONION_Count] = { "s02_onyonr.bck", "s02_onyony.bck", "s02_onyonb.bck" };
		for (int i = 0; i < ONION_Count; i++) {
			mOnionFlightBck[i] = mDemoArc[DEMO_Liftoff]->getResource(kOnionClips[i]);
			if (mOnionFlightBck[i] == nullptr) {
				OSReport("scene: %s missing\n", kOnionClips[i]);
			}
		}
	}
	if (mLiftoffBck == nullptr) {
		OSReport("scene: s02_ufo.bck missing; the ship lifts off without it\n");
	}
	return true;
}

// The archives stay mounted in the section heap: the resource managers point into their data.
void SceneEffects::loadDemo(int set, const char* path, u8 slot)
{
	mDemoArc[set] = JKRMountArchive(path, JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	void* efx     = mDemoArc[set] != nullptr ? mDemoArc[set]->getResource("efx.jpc") : nullptr;
	if (efx == nullptr) {
		OSReport("scene: %s efx.jpc missing; no take-off particles from it\n", path);
		return;
	}
	mDemo[set] = new (particleMgr->mHeap, 0) JPAResourceManager(efx, particleMgr->mHeap);
	particleMgr->getManager()->entryResourceManager(mDemo[set], slot);
}

void SceneEffects::start(const StageInfo& stage, const StageFrame& frame)
{
	if (!mActive) {
		return;
	}
	Graphics* gfx = sys->mGfx;
	particleMgr->setViewport(*gfx);
	if (stage.mAmbientEfx != 0xFFFF) {
		Vector3f pos = frame.toWorld(stage.mAmbientEfxOffset);
		mAmbient     = particleMgr->createDemo(stage.mAmbientEfx, pos, 5, 7);
		if (mAmbient != nullptr) {
			// The arrival movie runs this emitter for a few seconds; here it never ends (a max
			// frame of 0), which also keeps the pointer valid until exit(). Its volume is drawn in
			// around the landing site.
			mAmbient->mMaxFrame = 0;
			mAmbient->setScaleMain(kAmbientSpread, kAmbientSpread, kAmbientSpread);
			mAmbient->setGlobalScale(stage.mAmbientSize, stage.mAmbientSize);
			mAmbient->mParticleCallback = &sNearCull;
			sNearHide                   = stage.mAmbientNear;
		}
	}
}

// As TJPACallback_::execute with a parent node: the emitter takes the joint's whole matrix.
void SceneEffects::place(JPABaseEmitter* emitter, const Matrixf& mtx, f32 scale)
{
	emitter->setGlobalSRTMatrix(mtx.mMatrix.mtxView);
	if (scale != 1.0f) {
		emitter->mGlobalScl.set(emitter->mGlobalScl.x * scale, emitter->mGlobalScl.y * scale, emitter->mGlobalScl.z * scale);
		emitter->mGlobalPScl.set(emitter->mGlobalPScl.x * scale, emitter->mGlobalPScl.y * scale);
	}
}

int SceneEffects::startDemo(int set, u16 id, u8 group, const Matrixf* follow, const Matrixf* at, f32 seconds, f32 scale)
{
	if (!mActive || set < 0 || set >= DEMO_SetCount || mDemo[set] == nullptr || (follow == nullptr && at == nullptr)) {
		return -1;
	}
	int handle = -1;
	for (int i = 0; i < kMaxTracked; i++) {
		if (mTracked[i].mEmitter == nullptr) {
			handle = i;
			break;
		}
	}
	if (handle < 0) {
		return -1;
	}
	const Matrixf* mtx = follow != nullptr ? follow : at;
	Vector3f pos(mtx->mMatrix.mtxView[0][3], mtx->mMatrix.mtxView[1][3], mtx->mMatrix.mtxView[2][3]);
	JPABaseEmitter* emitter = particleMgr->createDemo(id, pos, group, kDemoSlot[set]);
	if (emitter == nullptr) {
		return -1;
	}
	// The script ends it, never the emitter itself: an emitter that deleted itself would leave a
	// dangling pointer here (the STB adaptor deletes its emitters explicitly too).
	emitter->setFlag(JPAEMIT_Immortal);
	place(emitter, *mtx, scale);

	Tracked& t  = mTracked[handle];
	t.mEmitter  = emitter;
	t.mFollow   = follow;
	t.mTimeLeft = seconds;
	t.mScale    = scale;
	return handle;
}

void SceneEffects::release(int handle)
{
	Tracked& t = mTracked[handle];
	if (t.mEmitter != nullptr) {
		particleMgr->fade(t.mEmitter); // clears Immortal: it goes once its particles are gone
		t.mEmitter = nullptr;
	}
}

void SceneEffects::stop(int handle)
{
	if (mActive && handle >= 0 && handle < kMaxTracked) {
		release(handle);
	}
}

void SceneEffects::setAlpha(int handle, u8 alpha)
{
	if (mActive && handle >= 0 && handle < kMaxTracked && mTracked[handle].mEmitter != nullptr) {
		mTracked[handle].mEmitter->setGlobalAlpha(alpha);
	}
}

namespace {

void tintColor(GXColor& c, u32 rgb, bool keepShade)
{
	f32 k = 1.0f;
	if (keepShade) {
		f32 lum = (0.3f * c.r + 0.59f * c.g + 0.11f * c.b) / 255.0f;
		k       = 0.75f + 0.25f * lum;
	}
	c.r = (u8)((f32)((rgb >> 16) & 0xFF) * k);
	c.g = (u8)((f32)((rgb >> 8) & 0xFF) * k);
	c.b = (u8)((f32)(rgb & 0xFF) * k);
}

} // namespace

void SceneEffects::tint(int set, u16 id, u32 prm, u32 env)
{
	if (!mActive || set < 0 || set >= DEMO_SetCount || mDemo[set] == nullptr) {
		return;
	}
	JPAResource* res = mDemo[set]->getResource(id);
	JPABaseShape* bsp = res != nullptr ? res->getBsp() : nullptr;
	if (bsp == nullptr || bsp->mData == nullptr) {
		return;
	}
	// The shape data points into the archive this section mounted in its own heap.
	tintColor(bsp->mData->mClrPrm, prm, false);
	tintColor(bsp->mData->mClrEnv, env, false);
	int frames = bsp->getClrAnmMaxFrm() + 1;
	for (int i = 0; i < frames; i++) {
		if (bsp->mPrmClrAnmTbl != nullptr) {
			tintColor(bsp->mPrmClrAnmTbl[i], prm, true);
		}
		if (bsp->mEnvClrAnmTbl != nullptr) {
			tintColor(bsp->mEnvClrAnmTbl[i], env, true);
		}
	}
}

void SceneEffects::stopSet(int set)
{
	if (!mActive) {
		return;
	}
	for (int i = 0; i < kMaxTracked; i++) {
		if (mTracked[i].mEmitter != nullptr && mTracked[i].mEmitter->getResourceManagerID() == kDemoSlot[set]) {
			release(i);
		}
	}
}

// After the section has posed every model for this frame, so emitters sit on their joints with no
// lag (the ship climbs about 12 units a frame).
void SceneEffects::update(const Vector3f& eye, f32 dt)
{
	if (!mActive) {
		return;
	}
	sEye = eye;
	for (int i = 0; i < kMaxTracked; i++) {
		Tracked& t = mTracked[i];
		if (t.mEmitter == nullptr) {
			continue;
		}
		if (t.mFollow != nullptr) {
			place(t.mEmitter, *t.mFollow, t.mScale);
		}
		if (t.mTimeLeft > 0.0f) {
			t.mTimeLeft -= dt;
			if (t.mTimeLeft <= 0.0f) {
				release(i);
			}
		}
	}

	int steps = 1;
	if (mStepHz > 0.0f) {
		const f32 step = 1.0f / mStepHz;
		mStepAcc += dt > 0.0f ? dt : 0.0f;
		steps = 0;
		while (mStepAcc >= step && steps < kMaxStepsPerFrame) {
			mStepAcc -= step;
			steps++;
		}
		if (mStepAcc >= step) {
			mStepAcc = 0.0f; // a long stall: drop the backlog rather than fast-forward
		}
	}
	if (steps == 0 && mForceStep) {
		steps = 1;
	}
	mForceStep = false;
	for (int i = 0; i < steps; i++) {
		particleMgr->update();
	}
}

void SceneEffects::draw(Graphics& gfx, Viewport* vp, SceneLightMgr* light)
{
	if (!mActive) {
		return;
	}
	vp->setProjection();
	vp->setViewport();
	light->fogOff(gfx);
	if (mStarAlpha > 0.0f) {
		drawStars(vp);
	}
	particleMgr->draw(vp, 0);
	light->setFog(gfx);
	for (u8 i = 3; i <= 5; i++) {
		particleMgr->draw(vp, i);
	}
	particleMgr->draw(vp, 1);
	light->fogOff(gfx);
	for (u8 i = 6; i <= 8; i++) {
		particleMgr->draw(vp, i);
	}
	particleMgr->draw(vp, 2);
	light->setFog(gfx);
}

void SceneEffects::drawStars(Viewport* vp)
{
	if (!sStarsMade) {
		makeStars();
	}
	Mtx& view = vp->getMatrix(false)->mMatrix.mtxView;
	// The camera's right and up axes in the world: the first two rows of the view matrix.
	const f32 rx = view[0][0], ry = view[0][1], rz = view[0][2];
	const f32 ux = view[1][0], uy = view[1][1], uz = view[1][2];

	GXSetNumChans(1);
	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetNumIndStages(0);
	GXSetTevDirect(GX_TEVSTAGE0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_SET);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetCullMode(GX_CULL_NONE);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXLoadPosMtxImm(view, GX_PNMTX0);
	GXSetCurrentMtx(GX_PNMTX0);

	// Each star is a soft dot: a hexagon fan, full alpha at the centre and none at the rim, so it
	// reads as a point of light at any size instead of a hard square.
	static const f32 kRimCos[6] = { 1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f };
	static const f32 kRimSin[6] = { 0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f };
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, kStarCount * 18);
	for (int i = 0; i < kStarCount; i++) {
		const Star& st = sStars[i];
		f32 fade       = (st.mY - kStarMinElev) / (kStarFadeElev - kStarMinElev);
		fade           = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
		u8 a           = (u8)(st.mBright * fade * mStarAlpha);
		u32 centre     = 0xF0F4FF00u | a;
		u32 rim        = 0xC8D4FF00u;
		f32 cx = sEye.x + st.mX * kStarDistance, cy = sEye.y + st.mY * kStarDistance, cz = sEye.z + st.mZ * kStarDistance;
		f32 h  = kStarSize * st.mSize;
		for (int k = 0; k < 6; k++) {
			int n   = (k + 1) % 6;
			f32 c0  = kRimCos[k] * h, s0 = kRimSin[k] * h;
			f32 c1  = kRimCos[n] * h, s1 = kRimSin[n] * h;
			GXPosition3f32(cx, cy, cz);
			GXColor1u32(centre);
			GXPosition3f32(cx + rx * c0 + ux * s0, cy + ry * c0 + uy * s0, cz + rz * c0 + uz * s0);
			GXColor1u32(rim);
			GXPosition3f32(cx + rx * c1 + ux * s1, cy + ry * c1 + uy * s1, cz + rz * c1 + uz * s1);
			GXColor1u32(rim);
		}
	}
	GXEnd();
	J3DShape::resetVcdVatCache();
}

// Emitters first, then the demo slots, then both singletons (BaseGameSection::~BaseGameSection).
void SceneEffects::exit()
{
	if (!mActive) {
		return;
	}
	for (int i = 0; i < kMaxTracked; i++) {
		mTracked[i].mEmitter = nullptr;
	}
	particleMgr->killAll();
	for (int i = 0; i < DEMO_SetCount; i++) {
		if (mDemo[i] != nullptr) {
			particleMgr->getManager()->clearResourceManager(kDemoSlot[i]);
			mDemo[i] = nullptr;
		}
	}
	particleMgr->clearDemoResourceManager();
	particleMgr->deleteInstance_TPkEffectMgr();
	ParticleMgr::deleteInstance();
	mAmbient = nullptr;
	mActive  = false;
}

} // namespace LauncherScene
