#pragma once

extern VirtFuncDetour kSkyShaderConstantsDetour;
void __fastcall SkyShader__UpdateConstants(SkyShader* apThis, void*, const NiPropertyState* apProperties);

// UNOFFICIAL the skin shader's hooks (effects/Skin.h, Shaders.cpp).
extern VirtFuncDetour kSkinPrepareGeometryDetour;
extern VirtFuncDetour kSkinPostGeometryDetour;
void* __fastcall SkinShader__PrepareGeometryForRendering(void* apThis, void*, void* apGeometry, void* apPartition, void* apRendererData, void* apState);
void __fastcall SkinShader__PostGeometry(void* apThis, void*, void* apProperties);
void InstallFaceGenInteriorPatch();
void UpdateFaceGenInteriorFlag();
