#define LOG_TAG "face-moto"

#include "Face.h"

#include <android-base/logging.h>

#include "Session.h"

namespace aidl::android::hardware::biometrics::face {

namespace {
// Fingerprint sensors use id 0; BiometricService rejects duplicate sensor ids
// across all modalities, so give the face sensor its own id.
constexpr int32_t kFaceSensorId = 10;
// The HAL owns the camera stream used for enrollment/authentication. If the
// framework opened its own preview stream on the same (single) front sensor,
// one of the two clients would be evicted. SurfaceProps.enrollPreview* tell
// Settings which size the preview surface should have.
constexpr bool kHalControlsPreview = true;
constexpr int32_t kPreviewWidth = 640;
constexpr int32_t kPreviewHeight = 480;
}  // namespace

ndk::ScopedAStatus Face::getSensorProps(std::vector<SensorProps>* out) {
    SensorProps props;
    props.commonProps.sensorId = kFaceSensorId;
    // 2D RGB matching is spoofable: CONVENIENCE keeps it keyguard-only and
    // never bound to keystore / BiometricPrompt.
    props.commonProps.sensorStrength =
            ::aidl::android::hardware::biometrics::common::SensorStrength::WEAK;
    props.commonProps.maxEnrollmentsPerUser = 1;
    props.sensorType = FaceSensorType::RGB;
    props.halControlsPreview = kHalControlsPreview;
    props.previewDisplayId = 0;
    props.enrollPreviewWidth = kPreviewWidth;
    props.enrollPreviewHeight = kPreviewHeight;
    props.enrollTranslationX = 0.f;
    props.enrollTranslationY = 0.f;
    props.enrollPreviewScale = 1.f;
    props.supportsDetectInteraction = false;
    *out = {std::move(props)};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t sensorId, int32_t userId,
                                       const std::shared_ptr<ISessionCallback>& callback,
                                       std::shared_ptr<ISession>* out) {
    if (callback == nullptr || out == nullptr) {
        LOG(ERROR) << "createSession with null callback/output";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (sensorId != kFaceSensorId) {
        LOG(ERROR) << "createSession for unknown sensor " << sensorId;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard<std::mutex> guard(mLock);
    if (auto current = mSession.lock(); current != nullptr && !current->isClosed()) {
        if (current->userId() == userId) {
            /* The framework can ask for a session again after a transient
             * hardware error. Throwing EX_ILLEGAL_STATE here kills
             * system_server (FaceStartUserClient does not handle it), so hand
             * back the still-valid session instead. */
            LOG(INFO) << "reusing active face session for user " << userId;
            *out = std::move(current);
            return ndk::ScopedAStatus::ok();
        }
        LOG(WARNING) << "closing stale face session for user " << current->userId();
        current->close();
    }

    auto session = ndk::SharedRefBase::make<Session>(sensorId, userId, callback);
    const binder_status_t status = session->linkToDeath(callback->asBinder().get());
    if (status != STATUS_OK) LOG(WARNING) << "linkToDeath failed: " << status;

    mSession = session;

    LOG(INFO) << "createSession sensor=" << sensorId << " user=" << userId;
    *out = std::move(session);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
