/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * API C de libarcsoft_faceid.so, recuperada do disassembly da cola JNI
 * (libarcsoft-lib.so) do MotoFaceUnlockArcSoft.apk. NÃO é um header oficial da ArcSoft.
 * [OBSERVADO] = offset lido do disassembly; [INFERIDO] = deduzido.
 * A libarcsoft_faceid.so não usa JNI e só depende de libmpbase/liblog/libc/libm/libdl.
 */

#pragma once

#include <cstdint>

namespace motorola::face::arcsoft {

using MHandle = void*;
using MRESULT = int32_t;

constexpr uint32_t kAsvlPafNv21 = 0x802; /* [OBSERVADO] formato usado por arcCompare/arcSaveFeature */

/* [OBSERVADO] 64 bytes: fmt@0 w@4 h@8 plane[4]@0x10 pitch[4]@0x30. Em NV21:
 * plane[0] = Y, plane[1] = início do VU (= plane[0] + w*h), pitch[0] = pitch[1] = w. */
struct ASVLOFFSCREEN {
    uint32_t format;
    int32_t width;
    int32_t height;
    uint8_t* plane[4];
    int32_t pitch[4];
};
static_assert(sizeof(ASVLOFFSCREEN) == 64, "ASVLOFFSCREEN deve ter 64 bytes");

/* [OBSERVADO] 0x48 bytes; a cola só lê o contador de rostos em +0x40. O resto é opaco. */
struct ArcFaceInfo {
    uint8_t opaque[0x40];
    int32_t faceCount;
    int32_t pad;
};
static_assert(sizeof(ArcFaceInfo) == 0x48, "ArcFaceInfo deve ter 0x48 bytes");

/* [OBSERVADO] saída de ExtractFeaturesForRegister: 0x58 bytes =
 * 5 x {ptr, size, pad} (16 bytes) + featureCount@0x50. Os ponteiros pertencem à lib
 * até o próximo ARC_FI_Reset. */
struct ArcFeatureRef {
    void* data;
    int32_t size;
    int32_t pad;
};
struct ArcRegisterFeatures {
    ArcFeatureRef feature[5];
    int32_t featureCount;
    int32_t pad;
};
static_assert(sizeof(ArcRegisterFeatures) == 0x58, "ArcRegisterFeatures deve ter 0x58 bytes");

/* [OBSERVADO] 24 bytes por entrada de ARC_FI_SetRegisterData:
 * data@0, size@8, (+0xc = 0), registId@0x10 (= 1 no enroll stock). A lib copia os dados. */
struct ArcRegisterRecord {
    void* data;
    int32_t size;
    int32_t reserved;
    int32_t registId;
    int32_t pad;
};
static_assert(sizeof(ArcRegisterRecord) == 24, "ArcRegisterRecord deve ter 24 bytes");

/* [OBSERVADO] 0x18 bytes; "reconheceu" = int em +0x14 != 0. O resto é opaco. */
struct ArcRecognizeResult {
    uint8_t opaque[0x14];
    int32_t matched;
};
static_assert(sizeof(ArcRecognizeResult) == 0x18, "ArcRecognizeResult deve ter 0x18 bytes");

extern "C" {
MRESULT ARC_FI_Init(MHandle* handle, int32_t mode /* sempre 1 */);
MRESULT ARC_FI_Uninit(MHandle* handle);
MRESULT ARC_FI_SetDeviceUniqueTag(MHandle handle, const char* tag, int32_t length);
MRESULT ARC_FI_SetProcessDataType(MHandle handle, int32_t type /* sempre 1 */);
MRESULT ARC_FI_SetRegisterData(MHandle handle, ArcRegisterRecord* records, int32_t count);
MRESULT ARC_FI_DetectFace(MHandle handle, ASVLOFFSCREEN* image, ArcFaceInfo* faces);
MRESULT ARC_FI_ExtractFeaturesForRegister(MHandle handle, ASVLOFFSCREEN* image,
                                          ArcFaceInfo* faces, ArcRegisterFeatures* out);
MRESULT ARC_FI_Recognize(MHandle handle, ASVLOFFSCREEN* image, ArcFaceInfo* faces,
                         ArcRecognizeResult* out);
MRESULT ARC_FI_Reset(MHandle handle);
}

}  // namespace motorola::face::arcsoft
