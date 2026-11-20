/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native ArcSoft engine wrapper. Fala direto com libarcsoft_faceid.so (ARC_FI_*);
 * libarcsoft-lib.so (a cola JNI do APK) não é mais usada nem linkada.
 * Os templates ficam em /data/vendor_de/<user>/facedata/facedata.bin no mesmo
 * formato do stock.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ArcSoftApi.h"

namespace motorola::face::arcsoft {

/**
 * Process-wide ArcSoft engine.
 *
 * Só existe uma engine por processo (um handle ARC_FI). A separação por usuário é
 * feita reinicializando a engine (Uninit + Init) quando o framework cria uma sessão
 * para outro usuário.
 */
class ArcSoftEngine {
  public:
    static ArcSoftEngine& Get();

    ArcSoftEngine(const ArcSoftEngine&) = delete;
    ArcSoftEngine& operator=(const ArcSoftEngine&) = delete;

    /** (Re)initialise the engine for |userId|. Safe to call repeatedly. */
    bool setUser(int32_t userId);

    bool ready() const;

    /** Number of templates currently loaded. */
    int32_t storedFaceCount() const;

    /** Enrollment ids stored in the database (empty when nothing is enrolled). */
    std::vector<int32_t> storedFaceIds() const;

    /** Remove every template of the current user (maxEnrollmentsPerUser == 1). */
    bool removeAll();

    /** Per-enrollment feature flags (REQUIRE_ATTENTION, REQUIRE_DIVERSE_POSES). */
    bool getFeature(int32_t feature, bool* enabled) const;
    bool setFeature(int32_t feature, bool enabled);

    /**
     * Run detection + recognition on an NV21 frame. Returns 0 on match,
     * a positive ArcSoft error code otherwise, negative on internal errors.
     */
    int compareFrame(const uint8_t* nv21, int32_t width, int32_t height);

    /**
     * Detect + extract the face in |nv21|. Returns 0 only when the enrollment is
     * complete and persisted; 0x700d (PROCESSING) while frames are still being
     * collected; other ArcSoft codes are quality feedback.
     */
    int enrollFrame(const uint8_t* nv21, int32_t width, int32_t height);

  private:
    struct Template {
        int32_t id = 1;
        std::vector<uint8_t> data;
    };

    ArcSoftEngine() = default;

    bool ensureInitLocked(int32_t userId);
    bool initHandleLocked();     // ARC_FI_Init + tag + templates + process type
    void releaseHandleLocked();  // ARC_FI_Uninit
    bool reloadHandleLocked();   // release + init (recarrega mTemplates)
    std::string pathLocked(const char* name) const;

    mutable std::mutex mLock;
    int32_t mUserId = -1;
    std::string mDir;
    bool mReady = false;

    MHandle mHandle = nullptr;
    std::vector<Template> mTemplates;

    /* Equivalente ao sSavedHanledImages da cola: frames válidos do enroll em curso. */
    int mEnrollFrames = 0;
    std::chrono::steady_clock::time_point mLastEnrollFrame{};
};

}  // namespace motorola::face::arcsoft
