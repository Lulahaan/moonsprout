// Water of the start-menu scene (docs/launcher/SCENE.md 10.5). See SceneWater.h.
#include "SceneWater.h"

#include "System.h"
#include "stream.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DModelLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "Sys/MatBaseAnimation.h"
#include "Sys/MatBaseAnimator.h"
#include "SysShape/Model.h"

#include <math.h>
#include <stdio.h>

namespace LauncherScene {

namespace {
const f32 kPiece = 100.0f; // the water model is a kPiece x kPiece square (AABBWaterBox::attachModel)
}

SceneWater::SceneWater()
    : mCount(0)
    , mData(nullptr)
    , mAnim(nullptr)
    , mAnimator(nullptr)
{
	for (int i = 0; i < kMaxBoxes; i++) {
		mModels[i] = nullptr;
	}
}

// The boxes as SeaMgr::read takes them (gameSeaMgr.cpp), the piece as SeaMgr::SeaMgr loads its
// two-player model.
void SceneWater::load(const char* mapFolder)
{
	char path[128];
	sprintf(path, "%s/texts.szs", mapFolder);
	JKRArchive* texts = JKRMountArchive(path, JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	void* boxes       = texts != nullptr ? texts->getResource("waterbox.txt") : nullptr;
	if (boxes == nullptr) {
		if (texts != nullptr) {
			texts->unmount();
		}
		return; // no water in this area
	}
	RamStream stream(boxes, -1);
	stream.setMode(STREAM_MODE_TEXT, 1);
	stream.readInt(); // version
	int count = stream.readInt();
	for (int i = 0; i < count; i++) {
		f32 v[6];
		for (int k = 0; k < 6; k++) {
			v[k] = stream.readFloat();
		}
		if (mCount < kMaxBoxes) {
			mBox[mCount][0] = v[0];
			mBox[mCount][1] = v[2];
			mBox[mCount][2] = v[3];
			mBox[mCount][3] = v[5];
			mTop[mCount]    = v[4];
			mCount++;
		}
	}
	texts->unmount(); // the boxes are copied out; the archive is not needed any more
	if (mCount == 0) {
		return;
	}

	JKRArchive* arc = JKRMountArchive("/user/Kando/map/waterbox2p.szs", JKRArchive::EMM_Mem, JKRGetCurrentHeap(), JKRArchive::EMD_Head);
	void* model     = arc != nullptr ? arc->getResource("2p/2p.bmd") : nullptr;
	void* scroll    = arc != nullptr ? arc->getResource("2p/2p.btk") : nullptr;
	if (model == nullptr) {
		OSReport("scene: waterbox2p.szs has no 2p.bmd; no water\n");
		mCount = 0;
		return;
	}
	mData = J3DModelLoaderDataBase::load(model, J3DMLF_Material_PE_FogOff | J3DMLF_UseUniqueMaterials | J3DMLF_UseSingleSharedDL
	                                                | J3DMLF_UseImmediateMtx);
	SysShape::Model::enableMaterialAnim(mData, 0);
	if (scroll != nullptr) {
		mAnim = new Sys::MatTexAnimation;
		mAnim->attachResource(scroll, mData);
	}
}

void SceneWater::init()
{
	if (mData == nullptr) {
		return;
	}
	if (mAnim != nullptr) {
		mAnimator = new Sys::MatLoopAnimator;
		mAnimator->start(mAnim);
	}
	for (int i = 0; i < mCount; i++) {
		mModels[i] = new SysShape::Model(mData, 0, 2);
		// AABBWaterBox::calcMatrix: the piece scaled to the box, at the water's top.
		Vector3f scale(fabsf(mBox[i][2] - mBox[i][0]) / kPiece, 1.0f, fabsf(mBox[i][3] - mBox[i][1]) / kPiece);
		Vector3f centre((mBox[i][0] + mBox[i][2]) * 0.5f, mTop[i], (mBox[i][1] + mBox[i][3]) * 0.5f);
		Matrixf mtx;
		mtx.makeSRT(scale, Vector3f::zero, centre);
		PSMTXCopy(mtx.mMatrix.mtxView, mModels[i]->getJ3DModel()->mPosMtx);
		mModels[i]->getJ3DModel()->calc();
	}
}

void SceneWater::update(f32 dt)
{
	if (mAnimator != nullptr && sys->mDeltaTime > 0.0f) {
		mAnimator->animate(30.0f * dt / sys->mDeltaTime); // MatBaseAnimator multiplies by mDeltaTime
	}
}

void SceneWater::entry()
{
	for (int i = 0; i < mCount; i++) {
		if (mModels[i] != nullptr) {
			mModels[i]->getJ3DModel()->calcMaterial();
			mModels[i]->getJ3DModel()->entry();
		}
	}
}

void SceneWater::viewCalc()
{
	for (int i = 0; i < mCount; i++) {
		if (mModels[i] != nullptr) {
			mModels[i]->viewCalc();
		}
	}
}

} // namespace LauncherScene
