#ifndef _JSYSTEM_J3D_J3DFILEBLOCK_H
#define _JSYSTEM_J3D_J3DFILEBLOCK_H

#include "types.h"

struct J3DFileBlockBase;

// Offsets within a block. Declared as pointers in the original structs; 32 bits in the file.
#define J3D_OFFSET PC_SWITCH(u32, void*)

#if TARGET_PC
void p2_hostJ3DAnm(const void* data);   // port/assets/j3d_anim.cpp
void p2_hostJ3DModel(const void* data); // port/assets/j3d_model.cpp
#endif

/**
 * @size{0x20}
 */
struct J3DFileHeader {
	BE(u32) mJ3dVersion;  // _00
	BE(u32) mFileVersion; // _04
	u8 _08[4];        // _08
	BE(u32) mBlockCount;  // _0C
	u8 _10[0x10];     // _10

	const J3DFileBlockBase* getFirstBlock() const { return reinterpret_cast<const J3DFileBlockBase*>(this + 1); }
};

/**
 * @size{0x8}
 */
struct J3DFileBlockBase {
	u32 mBlockType; // _00
	int mSize;      // _04

	const J3DFileBlockBase* getNext() const
	{
		return reinterpret_cast<const J3DFileBlockBase*>(reinterpret_cast<const u8*>(this) + this->mSize);
	}
};

enum J3DFileBlockType {
	J3DFBT_Draw        = 'DRW1',
	J3DFBT_Envelope    = 'EVP1',
	J3DFBT_Info        = 'INF1',
	J3DFBT_Joint       = 'JNT1',
	J3DFBT_MaterialV21 = 'MAT2',
	J3DFBT_Material    = 'MAT3',
	J3DFBT_MaterialDL  = 'MDL3',
	J3DFBT_Texture     = 'TEX1',
	J3DFBT_Shape       = 'SHP1',
	J3DFBT_Vertex      = 'VTX1',

	//  Anm Block Types:
	J3DFBT_AnmTexPattern     = 'TPT1',
	J3DFBT_AnmClusterFull    = 'CLF1',
	J3DFBT_AnmClusterKey     = 'CLK1',
	J3DFBT_AnmTransformFull  = 'ANF1',
	J3DFBT_AnmTransformKey   = 'ANK1',
	J3DFBT_AnmColorFull      = 'PAF1',
	J3DFBT_AnmColorKey       = 'PAK1',
	J3DFBT_AnmVtxColorFull   = 'VCF1',
	J3DFBT_AnmVtxColorKey    = 'VCK1',
	J3DFBT_AnmVisibilityFull = 'VAF1',
	J3DFBT_AnmTextureSRTKey  = 'TTK1',
	J3DFBT_AnmTevRegKey      = 'TRK1',
};

/**
 *    Block members courtesy of https://wiki.cloudmodding.com/tww/BMD_and_BDL
 */

struct J3DDrawBlock : J3DFileBlockBase {
	u16 mCount;                   // _08
	J3D_OFFSET mMatrixTypeArrayOffset; // _0C
	J3D_OFFSET mDataArrayOffset;       // _14
};

struct J3DEnvelopeBlock : J3DFileBlockBase {
	u16 mCount;                   // _08
	J3D_OFFSET mJointCountTableOffset; // _0C
	J3D_OFFSET mIndexTableOffset;      // _10
	J3D_OFFSET mWeightTableOffset;     // _14
	J3D_OFFSET mInvBindTableOffset;    // _18
};

struct J3DJointBlock : J3DFileBlockBase {
	u16 mCount;            // _08
	u32 mJointInitData;    // _0C
	u32 mRemapTableOffset; // _10
	u32 mNameTableOffset;  // _14
};

struct J3DMaterialBlock : J3DFileBlockBase {
	u16 mNumMaterials;                 // _08
	J3D_OFFSET mMatEntryDataOffset;         // _0C
	J3D_OFFSET mMatRemapTableOffset;        // _10
	J3D_OFFSET mStringTableOffset;          // _14
	J3D_OFFSET mIndTextureInfoOffset;       // _18
	J3D_OFFSET mCullModeInfoOffset;         // _1C
	J3D_OFFSET mMatColorsOffset;            // _20
	J3D_OFFSET mNumColorChansOffset;        // _24
	J3D_OFFSET mColorChanInfoOffset;        // _28
	J3D_OFFSET mAmbientColorOffset;         // _2C
	J3D_OFFSET mLightInfoOffset;            // _30
	J3D_OFFSET mNumTexCoordsOffset;         // _34
	J3D_OFFSET mTexCoordInfoOffset;         // _38
	J3D_OFFSET mTexCoord2InfoOffset;        // _3C
	J3D_OFFSET mTexMtxInfoOffset;           // _40
	J3D_OFFSET mTexMtxInfo2Offset;          // _44
	J3D_OFFSET mTextureRemapTableOffset;    // _48
	J3D_OFFSET mTevOrderInfoOffset;         // _4C
	J3D_OFFSET mTevColorsOffset;            // _50
	J3D_OFFSET mTevKColorsOffset;           // _54
	J3D_OFFSET mNumTevStagesOffset;         // _58
	J3D_OFFSET mTevStageInfoOffset;         // _5C
	J3D_OFFSET mTevSwapModeInfoOffset;      // _60
	J3D_OFFSET mTevSwapModeTableInfoOffset; // _64
	J3D_OFFSET mFogInfoOffset;              // _68
	J3D_OFFSET mAlphaCompInfoOffset;        // _6C
	J3D_OFFSET mBlendInfoOffset;            // _70
	J3D_OFFSET mZModeInfoOffset;            // _74
	J3D_OFFSET mZCompareInfoOffset;         // _78
	J3D_OFFSET mDitherInfoOffset;           // _7C
	J3D_OFFSET mNBTScaleInfoOffset;         // _80
};

struct J3DMaterialBlock_v21 : J3DFileBlockBase {
	u16 mNumMaterials;                 // _08
	J3D_OFFSET mMatEntryDataOffset;         // _0C
	J3D_OFFSET mMatRemapTableOffset;        // _10
	J3D_OFFSET mIndTexInfoOffset;           // _14
	J3D_OFFSET mCullModeInfoOffset;         // _18
	J3D_OFFSET mMatColorsOffset;            // _1C
	J3D_OFFSET mNumColorChansOffset;        // _20
	J3D_OFFSET mColorChanInfoOffset;        // _24
	J3D_OFFSET mNumTexCoordsOffset;         // _28
	J3D_OFFSET mTexCoordInfoOffset;         // _2C
	J3D_OFFSET mTexCoord2InfoOffset;        // _30
	J3D_OFFSET mTexMtxInfoOffset;           // _34
	J3D_OFFSET mTexMtxInfo2Offset;          // _38
	J3D_OFFSET mTextureRemapTableOffset;    // _3C
	J3D_OFFSET mTevOrderInfoOffset;         // _40
	J3D_OFFSET mTevColorsOffset;            // _44
	J3D_OFFSET mTevKColorsOffset;           // _48
	J3D_OFFSET mNumTevStagesOffset;         // _4C
	J3D_OFFSET mTevStageInfoOffset;         // _50
	J3D_OFFSET mTevSwapModeInfoOffset;      // _54
	J3D_OFFSET mTevSwapModeTableInfoOffset; // _58
	J3D_OFFSET mFogInfoOffset;              // _5C
	J3D_OFFSET mAlphaCompInfoOffset;        // _60
	J3D_OFFSET mBlendInfoOffset;            // _64
	J3D_OFFSET mZModeInfoOffset;            // _68
	J3D_OFFSET mZCompareInfoOffset;         // _6C
	J3D_OFFSET mDitherInfoOffset;           // _70
	J3D_OFFSET mNBTScaleInfoOffset;         // _74
};

struct J3DMaterialDLBlock : J3DFileBlockBase {
	u16 mEntries;                   // _08
	J3D_OFFSET mPacketOffset;            // _0C
	J3D_OFFSET mSubPacketLocationOffset; // _10
	J3D_OFFSET mMatrixIndexOffset;       // _14
	J3D_OFFSET mPixelEngineModesOffset;  // _18
	J3D_OFFSET mIndexesOffset;           // _1C
	J3D_OFFSET mStringTableOffset;       // _20
};

struct J3DModelInfoBlock : J3DFileBlockBase {
	u16 mFlags;                 // _08
	u32 mMatrixGroupCount;      // _0C
	u32 mVertexCount;           // _10
	J3D_OFFSET mHierarchyDataOffset; // _14
};

struct J3DShapeBlock : J3DFileBlockBase {
	u16 mShapeNum;             // _08
	u32 mShapeDataOffset;      // _0C
	u32 mRemapTableOffset;     // _10
	u32 mNameTableOffset;      // _14
	u32 mAttribTableOffset;    // _18
	u32 mMatrixTableOffset;    // _1C
	u32 mPrimDataOffset;       // _20
	u32 mMatrixInitDataOffset; // _24
	u32 mMtxGroupTableOffset;  // _28
};

struct J3DTextureBlock : J3DFileBlockBase {
	u16 mTextureCount;      // _08
	J3D_OFFSET mTexHeaderOffset; // _0C
	J3D_OFFSET mTexNameOffset;   // _10
};

struct J3DVertexBlock : J3DFileBlockBase {
	J3D_OFFSET mVertexFormatOffset;    // _08
	J3D_OFFSET mPositionDataOffset;    // _0C
	J3D_OFFSET mNormalDataOffset;      // _10
	J3D_OFFSET mNBTDataOffset;         // _14
	J3D_OFFSET mColorDataOffset[2];    // _18
	J3D_OFFSET mTexCoordDataOffset[8]; // _20
};

#endif
