/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/face/BnSession.h>
#include <aidl/android/hardware/biometrics/face/ISessionCallback.h>
#include <aidl/android/hardware/common/NativeHandle.h>
#include <android/binder_ibinder.h>
#include <android/native_window.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace common = ::aidl::android::hardware::biometrics::common;
namespace keymaster = ::aidl::android::hardware::keymaster;

namespace aidl::android::hardware::biometrics::face {

class Session : public BnSession {
  public:
    Session(int32_t sensorId, int32_t userId, std::shared_ptr<ISessionCallback> callback);
    ~Session() override;

    /** Called by Face::createSession() right after construction. */
    binder_status_t linkToDeath(AIBinder* client);

    /** True after close() or after the framework process died. */
    bool isClosed() const;

    int32_t userId() const { return mUserId; }

    // ---- ISession ----
    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus getEnrollmentConfig(EnrollmentType type,
                                           std::vector<EnrollmentStageConfig>* out) override;
    ndk::ScopedAStatus enroll(
            const keymaster::HardwareAuthToken& hat, EnrollmentType type,
            const std::vector<Feature>& features,
            const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(
            int64_t operationId,
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& ids) override;
    ndk::ScopedAStatus getFeatures() override;
    ndk::ScopedAStatus setFeature(const keymaster::HardwareAuthToken& hat, Feature feature,
                                  bool enabled) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId, const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(
            const keymaster::HardwareAuthToken& hat, EnrollmentType type,
            const std::vector<Feature>& features,
            const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
            const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus onContextChanged(const common::OperationContext& context) override;
    ndk::ScopedAStatus enrollWithOptions(
            const FaceEnrollOptions& options,
            std::shared_ptr<common::ICancellationSignal>* out) override;

    // ---- cancellation support ----
    void cancelOperation();

  private:
    enum class Op { NONE, ENROLL, AUTH, OTHER };

    bool beginOp(Op op);
    void endOp();
    void sendError(Error error, int32_t vendor = 0);
    void closeInternal(bool notifyFramework);
    std::shared_ptr<common::ICancellationSignal> makeCancel();
    ndk::ScopedAStatus startEnroll(bool hasPreviewSurface,
                                   std::shared_ptr<common::ICancellationSignal>* out);
    void setPreviewWindow(ANativeWindow* window);
    void clearPreviewWindow();

    /** Run the capture/ArcSoft loop on a worker so binder calls can return. */
    void launchCapture(Op op);
    void runCapture(Op op);
    void stopWorker();

    static void onClientDied(void* cookie);

    const int32_t mSensorId;
    const int32_t mUserId;
    std::shared_ptr<ISessionCallback> mCallback;

    mutable std::mutex mLock;
    bool mClosed = false;
    Op mOp = Op::NONE;
    std::vector<int64_t> mChallenges;
    ANativeWindow* mPreviewSurface = nullptr;
    ndk::SpAIBinder mClientBinder;
    AIBinder_DeathRecipient* mDeathRecipient = nullptr;

    std::atomic<bool> mCancelRequested{false};
    std::mutex mWorkerLock;
    std::thread mWorker;
};

}  // namespace aidl::android::hardware::biometrics::face
