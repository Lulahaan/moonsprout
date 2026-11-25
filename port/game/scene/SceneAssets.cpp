// Loading of the start-menu scene's disc resources (docs/launcher/SCENE.md 6.2). Runs on the DVD
// thread; every loader call and flag mirrors the gameplay code named next to it.
#include "SceneAssets.h"
#include "SceneStage.h"

#include "System.h"
#include "stream.h"
#include "JSystem/J3D/J3DModelLoader.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include "SysShape/AnimMgr.h"
#include "SysShape/Model.h"
#include "Game/GameLight.h"

#include <stdio.h>
#include <stdlib.h>

namespace LauncherScene {
namespace {

void* getResource(JKRArchive* arc, const char* name)
{
	return arc != nullptr ? arc->getResource(name) : nullptr;
}

// Display-list flags gameplay uses for the Pikmin and their leaves (PikiMgr::loadBmd).
u32 pikiDLFlags()
{
	const u32 lightObjNum = 0;
	const u32 texGenNum   = 0;
	const u32 texCoordNum = 4;
	const u32 tevStageNum = 0;
	return CREATE_DIFF_FLAG(lightObjNum, texGenNum, texCoordNum, tevStageNum);
}

// SCENE.md 10.4: the idle lens stands 11 units above the ground, so the near ground is seen at a
// grazing angle the gameplay camera never has. Where a map texture has mips, an isotropic mip pick
// smears it into horizontal streaks (most ground textures have none and are simply low-res for this
// distance: SceneGround.h adds detail for those). Every mipmapped map texture is asked for the most
// anisotropic sampling Aurora gives (GX_ANISO_4 = the host's limit) with trilinear filtering, and
// a small negative LOD bias. MPPM_SCENE_LODBIAS (in mip levels) and MPPM_SCENE_TEXLOG=1 are for tuning.
void sharpenMapTextures(J3DModelData* data)
{
	J3DTexture* textures = data->mMaterialTable.getTexture();
	if (textures == nullptr) {
		return;
	}
	f32 bias = -0.6f;
	if (const char* env = getenv("MPPM_SCENE_LODBIAS")) {
		bias = (f32)atof(env);
	}
	const bool log = getenv("MPPM_SCENE_TEXLOG") != nullptr;
	for (u32 i = 0; i < textures->getNum(); i++) {
		ResTIMG* img = textures->getResTIMG(i);
		if (log) {
			OSReport("scene: map tex %u: %ux%u fmt %u mips %u images %u min %u mag %u lod %d..%d bias %d aniso %u\n", i,
			         (u32)img->mSizeX, (u32)img->mSizeY, img->mTextureFormat, img->mIsMIPmapEnabled, img->mTotalImageCount,
			         img->mMinFilterType, img->mMagFilterType, img->mMinLOD, img->mMaxLOD, (int)img->mLODBias,
			         img->mIsMaxAnisotropy);
		}
		if (!img->mIsMIPmapEnabled || img->mTotalImageCount < 2 || img->mLODBias > 0) {
			// No mips, or authored soft on purpose (the light shafts' 64x64 glow has +1.8).
			continue;
		}
		img->mIsMaxAnisotropy = GX_ANISO_4;
		img->mMinFilterType   = GX_LIN_MIP_LIN;
		img->mMagFilterType   = GX_LINEAR;
		img->mLODBias         = (s16)(bias * 100.0f);
	}
}

} // namespace

Assets::Assets()
    : mPikiArc(nullptr)
    , mOlimarData(nullptr)
    , mAnimMgr(nullptr)
    , mShipData(nullptr)
    , mShipAnimMgr(nullptr)
    , mOnionData(nullptr)
    , mOnionAnimMgr(nullptr)
    , mMapData(nullptr)
    , mMapTexAnimCount(0)
    , mMapTexAnims(nullptr)
    , mLightSetting(nullptr)
    , mError(nullptr)
{
	for (int i = 0; i < PIKI_ColourCount; i++) {
		mPikiData[i] = nullptr;
	}
	for (int i = 0; i < HAPPA_Count; i++) {
		mHappaData[i] = nullptr;
	}
}

bool Assets::load(const StageInfo& stage)
{
	if (!loadCharacters()) {
		return false;
	}
	loadShip();
	loadOnions();
	loadMap(stage);
	mWater.load(stage.mMapFolder);
	loadLight(stage);
	return true;
}

// Olimar as NaviMgr::load (naviMgr.cpp:190-231), the Pikmin and leaves as PikiMgr::load/loadBmd
// (pikiMgr.cpp:178-252). The clip set is loaded with Olimar's model data, like NaviMgr::animMgr,
// which gameplay shares between captains and Pikmin.
bool Assets::loadCharacters()
{
	JKRArchive* texts = JKRMountArchive("/user/Kando/piki/texts.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Tail);
	mPikiArc          = JKRMountArchive("/user/Kando/piki/pikis.szs", JKRArchive::EMM_Mem, sys->mSysHeap, JKRArchive::EMD_Head);
	if (texts == nullptr || mPikiArc == nullptr) {
		mError = "pikis.szs / texts.szs not found";
		if (texts != nullptr) {
			texts->unmount();
		}
		return false;
	}

	void* file = getResource(mPikiArc, "orima_model/orima1.bmd");
	if (file == nullptr) {
		mError = "orima1.bmd not found";
		texts->unmount();
		return false;
	}
	mOlimarData = J3DModelLoaderDataBase::load(file, J3DMLF_Material_PE_FogOff | J3DMLF_UsePostTexMtx | J3DMLF_UseImmediateMtx);
	for (u16 i = 0; i < mOlimarData->getShapeNum(); i++) {
		mOlimarData->mShapeTable.mItems[i]->setTexMtxLoadType(0x2000);
	}

	if (texts->getResource("animMgr.txt") == nullptr) {
		mError = "animMgr.txt not found";
		texts->unmount();
		return false;
	}
	mAnimMgr = SysShape::AnimMgr::load(texts, "animMgr.txt", mOlimarData, mPikiArc, "motion");
	texts->unmount();
	if (mAnimMgr == nullptr) {
		mError = "the clip set did not load";
		return false;
	}

	static const char* const kPikiNames[PIKI_ColourCount] = { "piki_p2_red", "piki_p2_yellow", "piki_p2_blue" };
	for (int i = 0; i < PIKI_ColourCount; i++) {
		char path[64];
		sprintf(path, "piki_model/%s.bmd", kPikiNames[i]);
		file = getResource(mPikiArc, path);
		if (file == nullptr) {
			mError = "a Pikmin model is missing";
			return false;
		}
		J3DModelData* data = J3DModelLoaderDataBase::load(file, J3DMLF_UseSingleSharedDL | J3DMLF_18 | J3DMLF_UseImmediateMtx);
		data->newSharedDisplayList(pikiDLFlags());
		data->makeSharedDL();
		mPikiData[i] = data;
	}

	static const char* const kHappaNames[HAPPA_Count] = { "happa_model/leaf.bmd", "happa_model/bud.bmd", "happa_model/flower.bmd" };
	for (int i = 0; i < HAPPA_Count; i++) {
		file = getResource(mPikiArc, kHappaNames[i]);
		if (file == nullptr) {
			mError = "a leaf model is missing";
			return false;
		}
		u32 flags     = i == HAPPA_Leaf ? J3DMLF_Material_PE_FogOff : (J3DMLF_UseUniqueMaterials | J3DMLF_UseSingleSharedDL);
		mHappaData[i] = J3DModelLoaderDataBase::load(file, flags);
		mHappaData[i]->newSharedDisplayList(pikiDLFlags());
		mHappaData[i]->simpleCalcMaterial(0, *(Mtx*)j3dDefaultMtx);
		mHappaData[i]->makeSharedDL();
	}
	return true;
}

// The ship as ItemOnyon::Mgr::loadResource (onyonMgr.cpp:2255-2302), reading from its own
// archive rather than the global resource search (several mounted archives have an animmgr.txt).
void Assets::loadShip()
{
	JKRArchive* arc   = JKRMountArchive("/user/Kando/ufo/arc.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	JKRArchive* texts = JKRMountArchive("/user/Kando/ufo/texts.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Tail);
	void* file        = getResource(arc, "ufo.bmd");
	void* brk0        = getResource(arc, "ufo.brk");
	void* brk1        = getResource(arc, "ufo_2.brk");
	if (file == nullptr || brk0 == nullptr || brk1 == nullptr || getResource(texts, "animmgr.txt") == nullptr) {
		OSReport("scene: the ship's resources are missing; no ship\n");
		if (texts != nullptr) {
			texts->unmount();
		}
		return;
	}

	J3DModelData* data = J3DModelLoaderDataBase::load(file, J3DMLF_Material_PE_FogOff | J3DMLF_Material_UseIndirect | J3DMLF_18
	                                                            | J3DMLF_UsePostTexMtx | J3DMLF_UseImmediateMtx);
	data->newSharedDisplayList(J3DMLF_UseSingleSharedDL);
	data->makeSharedDL();
	for (u16 i = 0; i < data->getShapeNum(); i++) {
		data->getShapeNodePointer(i)->setTexMtxLoadType(0x2000);
	}
	SysShape::Model::enableMaterialAnim(data, 0);
	mShipTev[0].attachResource(brk0, data);
	mShipTev[1].attachResource(brk1, data);

	mShipAnimMgr = SysShape::AnimMgr::load(texts, "animmgr.txt", data, arc, nullptr);
	texts->unmount();
	if (mShipAnimMgr != nullptr) {
		mShipData = data;
	}
}

// The Onions as ItemOnyon::Mgr::loadResource (onyonMgr.cpp:2187-2237): unique materials, no
// shared display list calls, the three colour animations on the one model data, and the clip set
// from its own text archive (with its loop markers, unlike AnimInfo::attach).
void Assets::loadOnions()
{
	JKRArchive* arc   = JKRMountArchive("/user/Kando/onyon/arc.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	JKRArchive* texts = JKRMountArchive("/user/Kando/onyon/texts.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Tail);
	static const char* const kBrk[PIKI_ColourCount] = { "kidou_red.brk", "kidou_yellow.brk", "kidou_blue.brk" };
	void* file = getResource(arc, "goal.bmd");
	bool ok    = file != nullptr && getResource(texts, "onyonanimmgr.txt") != nullptr;
	for (int i = 0; i < PIKI_ColourCount; i++) {
		ok = ok && getResource(arc, kBrk[i]) != nullptr;
	}
	if (!ok) {
		OSReport("scene: the Onions' resources are missing; no Onions\n");
		if (texts != nullptr) {
			texts->unmount();
		}
		return;
	}

	J3DModelData* data = J3DModelLoaderDataBase::load(file, J3DMLF_UseUniqueMaterials | J3DMLF_UseSingleSharedDL);
	SysShape::Model::enableMaterialAnim(data, 0);
	for (int i = 0; i < PIKI_ColourCount; i++) {
		mOnionTev[i].attachResource(getResource(arc, kBrk[i]), data);
	}
	mOnionAnimMgr = SysShape::AnimMgr::load(texts, "onyonanimmgr.txt", data, arc, nullptr);
	texts->unmount();
	if (mOnionAnimMgr != nullptr) {
		mOnionData = data;
	} else {
		OSReport("scene: the Onions' clips did not load; no Onions\n");
	}
}

// The area model as ShapeMapMgr::load (mapMgr.cpp:283-360), without the farm, sea, routes or
// collision: nothing walks on it, and the landing sites are flat.
void Assets::loadMap(const StageInfo& stage)
{
	char path[128];
	sprintf(path, "%s/arc.szs", stage.mMapFolder);
	JKRArchive* arc = JKRMountArchive(path, JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	void* file      = getResource(arc, "model.bmd");
	if (file == nullptr) {
		OSReport("scene: %s has no model.bmd; no map\n", path);
		return;
	}
	J3DModelData* data = J3DModelLoaderDataBase::load(file, J3DMLF_Material_PE_FogOff | J3DMLF_UseSingleSharedDL);
	sharpenMapTextures(data);
	if (getenv("MPPM_SCENE_TEXLOG") != nullptr) {
		for (u16 i = 0; i < data->mMaterialTable.getMaterialNum(); i++) {
			J3DMaterial* m = data->mMaterialTable.getMaterialNodePointer(i);
			OSReport("scene: map mat %u %s mode %u tex %d\n", i, data->mMaterialTable.getMaterialName()->getName(i), m->mMaterialMode,
			         (int)(s16)m->getTexNo(0));
		}
	}
	data->newSharedDisplayList(J3DMLF_UseSingleSharedDL);
	data->makeSharedDL();
	mMapData = data;

	for (int i = 0; i < 100; i++) {
		sprintf(path, "texanm_%d.btk", i + 1);
		if (getResource(arc, path) == nullptr) {
			break;
		}
		mMapTexAnimCount++;
	}
	if (mMapTexAnimCount > 0) {
		// The material animators themselves are switched on by MapActor::init, after the model's
		// display lists exist, in ShapeMapMgr's order.
		mMapTexAnims = new Sys::MatTexAnimation[mMapTexAnimCount];
		for (int i = 0; i < mMapTexAnimCount; i++) {
			sprintf(path, "texanm_%d.btk", i + 1);
			mMapTexAnims[i].attachResource(getResource(arc, path), data);
		}
	}
}

// light.ini as ShapeMapMgr::load and GameLightMgr::loadParm read it (mapMgr.cpp:367-376,
// gameLightMgr.cpp:466-475).
void Assets::loadLight(const StageInfo& stage)
{
	char path[128];
	sprintf(path, "%s/light.ini", stage.mAbeFolder);
	void* file = JKRDvdToMainRam(path, nullptr, Switch_0, 0, nullptr, JKRDvdRipper::ALLOC_DIR_BOTTOM, 0, nullptr, nullptr);
	if (file == nullptr) {
		OSReport("scene: %s not found; default light\n", path);
		return;
	}
	RamStream stream(file, -1);
	stream.setMode(STREAM_MODE_TEXT, 1);
	stream.mTabCount = 0;
	mLightSetting    = new Game::GameLightMgrSetting;
	mLightSetting->read(stream);
	delete[] (u8*)file;
}

} // namespace LauncherScene
