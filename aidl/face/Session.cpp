#define LOG_TAG "face-moto-session"

#include "Session.h"

#include <android-base/logging.h>
#include <android/native_window.h>

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

#include "ArcSoftFace.h"
#include "CameraCapture.h"

using motorola::face::arcsoft::ArcSoftEngine;
using motorola::face::camera::CameraCapture;

namespace aidl::android::hardware::biometrics::face {

namespace {

/* Error codes from ArcConstant.smali in the stock APK. */
constexpr int kArcOk = 0;
constexpr int kArcNoFace = 0x7002;
constexpr int kArcRecognizeFail = 0x7006;

constexpr auto kEnrollTimeout = std::chrono::milliseconds(25000);
constexpr auto kAuthTimeout = std::chrono::milliseconds(10000);
constexpr int kFramePollMs = 100;
constexpr int kFrameCallbackIntervalMs = 250;

AcquiredInfo acquiredInfoForArcError(int rc) {
    switch (rc) {
        case 0x7001: return AcquiredInfo::NOT_DETECTED;  // MULTIFACE
        case kArcNoFace: return AcquiredInfo::NOT_DETECTED;
        case 0x7003: return AcquiredInfo::VENDOR;  // NOT_ALIVE
        case 0x7004: return AcquiredInfo::TOO_DIFFERENT;      // ABNORMAL_FACE
        case 0x7005: return AcquiredInfo::INSUFFICIENT;       // FEATURE_DETECTFAIL
        case kArcRecognizeFail: return AcquiredInfo::TOO_DIFFERENT;
        case 0x7008: return AcquiredInfo::POOR_GAZE;  // NOT_FRONTAL_FACE
        case 0x7009: return AcquiredInfo::TOO_DARK;
        case 0x700a: return AcquiredInfo::FACE_TOO_HIGH;  // EYE_CLOSED
        case 0x700b: return AcquiredInfo::TOO_CLOSE;
        case 0x700c: return AcquiredInfo::INSUFFICIENT;  // PART_FACE
        case 0x700d: return AcquiredInfo::GOOD;  // MG_UNLOCK_KEEP: frame accepted
        case 0x700e: return AcquiredInfo::MOUTH_COVERING_DETECTED;
        case 0x700f: return AcquiredInfo::FACE_OBSCURED;  // EYES_SHELTERED
        case 0x7010: return AcquiredInfo::INSUFFICIENT;  // REGISTER_FEATURE_ERROR
        case 0x7011: return AcquiredInfo::UNKNOWN;       // NO_DEVICE_UNIQUE_TAG
        case 0x7012: return AcquiredInfo::FACE_OBSCURED;  // NOSE_SHELTERED
        case 0x7013: return AcquiredInfo::INSUFFICIENT;  // FACE_BLUR: keep capturing
        default: return AcquiredInfo::UNKNOWN;
    }
}

int64_t randomChallenge() {
    uint64_t value = 0;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::read(fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) value = 0;
        ::close(fd);
    }
    if (value == 0) {
        value = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    return static_cast<int64_t>(value == 0 ? 1 : value);
}

/* Cancellation signal that keeps the session alive for as long as the
 * framework holds the object (the binder reference keeps the C++ object
 * alive through SharedRefBase). */
class SessionCancel : public common::BnCancellationSignal {
  public:
    explicit SessionCancel(Session* session) : mSession(session), mBinder(session->asBinder()) {}

    ndk::ScopedAStatus cancel() override {
        mSession->cancelOperation();
        return ndk::ScopedAStatus::ok();
    }

  private:
    Session* mSession;
    ndk::SpAIBinder mBinder;
};

}  // namespace

Session::Session(int32_t sensorId, int32_t userId, std::shared_ptr<ISessionCallback> callback)
    : mSensorId(sensorId), mUserId(userId), mCallback(std::move(callback)) {
    LOG(INFO) << "session created: sensor=" << mSensorId << " user=" << mUserId;
    mDeathRecipient = AIBinder_DeathRecipient_new(&Session::onClientDied);
}

Session::~Session() {
    closeInternal(false);
    if (mPreviewSurface != nullptr) {
        ANativeWindow_release(mPreviewSurface);
        mPreviewSurface = nullptr;
    }
    if (mDeathRecipient != nullptr) {
        AIBinder_DeathRecipient_delete(mDeathRecipient);
        mDeathRecipient = nullptr;
    }
}

binder_status_t Session::linkToDeath(AIBinder* client) {
    if (client == nullptr || mDeathRecipient == nullptr) return STATUS_BAD_VALUE;
    std::lock_guard<std::mutex> guard(mLock);
    if (mClosed) return STATUS_DEAD_OBJECT;
    /* SpAIBinder adopts a reference; take our own before wrapping it. */
    AIBinder_incStrong(client);
    mClientBinder = ndk::SpAIBinder(client);
    return AIBinder_linkToDeath(client, mDeathRecipient, this);
}

bool Session::isClosed() const {
    std::lock_guard<std::mutex> guard(mLock);
    return mClosed;
}

void Session::onClientDied(void* cookie) {
    auto* session = static_cast<Session*>(cookie);
    if (session == nullptr) return;
    LOG(INFO) << "client died, closing session";
    session->closeInternal(false);
}

void Session::closeInternal(bool notifyFramework) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> guard(mLock);
        if (mClosed) return;
        mClosed = true;
        mOp = Op::NONE;
        mChallenges.clear();
        notify = notifyFramework;
    }
    /* The callback proxy may already be gone when the client dies; keep it
     * alive while unlinking. SpAIBinder has no move semantics, so copy the
     * pointer with an explicit reference instead of moving the member. */
    AIBinder* client = nullptr;
    {
        std::lock_guard<std::mutex> guard(mLock);
        client = mClientBinder.get();
        if (client != nullptr) AIBinder_incStrong(client);
        mClientBinder.set(nullptr);
    }
    if (client != nullptr) {
        if (mDeathRecipient != nullptr) {
            AIBinder_unlinkToDeath(client, mDeathRecipient, this);
        }
        AIBinder_decStrong(client);
    }
    mCancelRequested = true;
    stopWorker();
    if (notify) mCallback->onSessionClosed();
}

bool Session::beginOp(Op op) {
    bool busy = false;
    Op current = Op::NONE;
    {
        std::lock_guard<std::mutex> guard(mLock);
        if (mClosed) return false;
        if (mOp != Op::NONE) {
            busy = true;
            current = mOp;
        } else {
            mOp = op;
            mCancelRequested = false;
        }
    }
    if (busy) {
        LOG(WARNING) << "operation requested while " << static_cast<int>(current)
                     << " is active";
        /* Do not call sendError(): it clears mOp, which would cancel the
         * operation that is already running. */
        mCallback->onError(Error::UNABLE_TO_PROCESS, 0);
        return false;
    }
    if (!ArcSoftEngine::Get().setUser(mUserId)) {
        sendError(Error::HW_UNAVAILABLE);
        return false;
    }
    return true;
}

void Session::endOp() {
    std::lock_guard<std::mutex> guard(mLock);
    mOp = Op::NONE;
}

void Session::sendError(Error error, int32_t vendor) {
    {
        std::lock_guard<std::mutex> guard(mLock);
        mOp = Op::NONE;
        if (mClosed) return;
    }
    mCallback->onError(error, vendor);
}

void Session::cancelOperation() {
    mCancelRequested = true;
    bool active;
    {
        std::lock_guard<std::mutex> guard(mLock);
        active = !mClosed && mOp != Op::NONE;
        mOp = Op::NONE;
    }
    LOG(INFO) << "cancel requested, active=" << active;
    if (!active) return;
    mCallback->onError(Error::CANCELED, 0);
}

std::shared_ptr<common::ICancellationSignal> Session::makeCancel() {
    return ndk::SharedRefBase::make<SessionCancel>(this);
}

// ---------------------------------------------------------------- ISession

ndk::ScopedAStatus Session::generateChallenge() {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();
    const int64_t challenge = randomChallenge();
    {
        std::lock_guard<std::mutex> guard(mLock);
        /* Multiple challenges may be in flight (see ISession.aidl); keep the
         * list bounded since only the most recent ones are realistically used. */
        constexpr size_t kMaxChallenges = 8;
        if (mChallenges.size() >= kMaxChallenges) mChallenges.erase(mChallenges.begin());
        mChallenges.push_back(challenge);
        mOp = Op::NONE;
    }
    mCallback->onChallengeGenerated(challenge);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();

    {
        std::lock_guard<std::mutex> guard(mLock);
        mChallenges.erase(std::remove(mChallenges.begin(), mChallenges.end(), challenge),
                          mChallenges.end());
        mOp = Op::NONE;
    }
    /* The HAL must notify even for a challenge it does not know about. */
    mCallback->onChallengeRevoked(challenge);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType /*type*/,
                                                std::vector<EnrollmentStageConfig>* out) {
    /* Optional for HALs that do not report intermediate enrollment stages. */
    if (out != nullptr) out->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(
        const keymaster::HardwareAuthToken& /*hat*/, EnrollmentType /*type*/,
        const std::vector<Feature>& /*features*/,
        const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return startEnroll(previewSurface.has_value(), out);
}

void Session::setPreviewWindow(ANativeWindow* window) {
    /* The AIDL NativeWindow owns its reference only for the duration of the
     * binder call. Keep one alive for the capture worker. */
    if (window != nullptr) ANativeWindow_acquire(window);
    std::lock_guard<std::mutex> guard(mLock);
    if (mPreviewSurface != nullptr) ANativeWindow_release(mPreviewSurface);
    mPreviewSurface = window;
}

ndk::ScopedAStatus Session::startEnroll(bool hasPreviewSurface,
                                        std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel();
    if (!beginOp(Op::ENROLL)) return ndk::ScopedAStatus::ok();

    /* SensorProps reports halControlsPreview=true: the framework must not open
     * a second preview stream on the same camera. The HAL attaches the surface
     * supplied by the framework directly to its camera session and also reports
     * every captured frame through onEnrollmentFrame() so the UI can guide the
     * user. */
    LOG(INFO) << "enroll started" << (hasPreviewSurface ? " (preview surface provided)" : "");
    launchCapture(Op::ENROLL);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(
        int64_t /*operationId*/, std::shared_ptr<common::ICancellationSignal>* out) {
    LOG(INFO) << "authenticate requested";
    *out = makeCancel();
    if (!beginOp(Op::AUTH)) return ndk::ScopedAStatus::ok();

    launchCapture(Op::AUTH);
    return ndk::ScopedAStatus::ok();
}

void Session::stopWorker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(mWorkerLock);
        if (mWorker.joinable()) worker = std::move(mWorker);
    }
    if (worker.joinable()) worker.join();
}

void Session::launchCapture(Op op) {
    stopWorker();
    std::lock_guard<std::mutex> guard(mWorkerLock);
    mWorker = std::thread([this, op] { runCapture(op); });
}

void Session::clearPreviewWindow() {
    std::lock_guard<std::mutex> guard(mLock);
    if (mPreviewSurface != nullptr) {
        ANativeWindow_release(mPreviewSurface);
        mPreviewSurface = nullptr;
    }
}

void Session::runCapture(Op op) {
    /* The preview Surface belongs to the enrollment UI and the framework destroys
     * it as soon as the enroll client finishes ("Releasing mPreviewSurface").
     * Keeping our reference and attaching that dead surface to the next camera
     * session (authenticate) makes createCaptureSession() fail until the service
     * is restarted. So: only enroll uses it, and the reference is dropped when the
     * enroll operation ends, whatever the reason (done, cancel, error, timeout).
     * Declared before |camera| so it runs after the camera session is torn down. */
    struct PreviewRelease {
        Session* session;
        bool enroll;
        ~PreviewRelease() {
            if (enroll) session->clearPreviewWindow();
        }
    } previewRelease{this, op == Op::ENROLL};

    CameraCapture camera;
    if (op == Op::ENROLL) {
        std::lock_guard<std::mutex> guard(mLock);
        camera.setPreviewSurface(mPreviewSurface);
    }
    if (!camera.start()) {
        LOG(ERROR) << "cannot open the front camera for face capture";
        /* HW_UNAVAILABLE makes the framework drop the session and immediately
         * ask for a new one, which used to race with the still-live session and
         * crash system_server. A camera open failure here is usually transient
         * (surface/session being reconfigured), so keep the session alive. */
        sendError(Error::UNABLE_TO_PROCESS);
        return;
    }

    const auto timeout = op == Op::ENROLL ? kEnrollTimeout : kAuthTimeout;
    const auto start = std::chrono::steady_clock::now();
    auto lastFrameCallback = start - std::chrono::milliseconds(kFrameCallbackIntervalMs);
    auto lastFailedAttempt = start;
    int enrolledId = 1;
    {
        const auto ids = ArcSoftEngine::Get().storedFaceIds();
        if (!ids.empty()) enrolledId = ids.front();
    }

    while (!mCancelRequested && !isClosed() &&
           std::chrono::steady_clock::now() - start < timeout) {
        CameraCapture::Frame frame;
        if (!camera.waitForFrame(kFramePollMs, &frame)) continue;

        const int rc = op == Op::ENROLL
                ? ArcSoftEngine::Get().enrollFrame(frame.nv21.data(), frame.width, frame.height)
                : ArcSoftEngine::Get().compareFrame(frame.nv21.data(), frame.width, frame.height);
        const auto now = std::chrono::steady_clock::now();

        if (rc == kArcOk) {
            if (op == Op::ENROLL) {
                const auto ids = ArcSoftEngine::Get().storedFaceIds();
                const int id = ids.empty() ? enrolledId : ids.back();
                EnrollmentFrame done;
                done.stage = EnrollmentStage::ENROLLMENT_FINISHED;
                done.data.acquiredInfo = AcquiredInfo::GOOD;
                mCallback->onEnrollmentFrame(done);
                mCallback->onEnrollmentProgress(id, 0);
                LOG(INFO) << "enrollment complete, id=" << id;
            } else {
                AuthenticateSuccess result;
                result.enrollmentId = enrolledId;
                int32_t version = 0;
                if (mCallback->getInterfaceVersion(&version).isOk() && version >= 5) {
                    mCallback->onAuthenticationSucceededWithResult(result);
                } else {
                    mCallback->onAuthenticationSucceeded(enrolledId, {} /* hat */);
                }
                LOG(INFO) << "face matched, id=" << enrolledId;
            }
            camera.stop();
            endOp();
            return;
        }

        if (now - lastFrameCallback < std::chrono::milliseconds(kFrameCallbackIntervalMs)) continue;
        lastFrameCallback = now;

        const AcquiredInfo info = acquiredInfoForArcError(rc);
        if (op == Op::ENROLL) {
            EnrollmentFrame ef;
            ef.stage = EnrollmentStage::WAITING_FOR_CENTERING;
            ef.data.acquiredInfo = info;
            ef.data.vendorCode = (info == AcquiredInfo::UNKNOWN || info == AcquiredInfo::VENDOR) ? rc : 0;
            mCallback->onEnrollmentFrame(ef);
        } else {
            AuthenticationFrame af;
            af.data.acquiredInfo = info;
            af.data.vendorCode = (info == AcquiredInfo::UNKNOWN || info == AcquiredInfo::VENDOR) ? rc : 0;
            mCallback->onAuthenticationFrame(af);
            if (rc == kArcRecognizeFail && now - lastFailedAttempt >= std::chrono::milliseconds(500)) {
                lastFailedAttempt = now;
                mCallback->onAuthenticationFailed();
            }
        }
    }

    camera.stop();
    if (mCancelRequested || isClosed()) return;  // cancelOperation/close already reported it
    sendError(Error::TIMEOUT);
}

ndk::ScopedAStatus Session::detectInteraction(
        std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel();
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();
    /* ISession.aidl: sensors with supportsDetectInteraction == false must
     * answer onError(UNABLE_TO_PROCESS, 0). */
    sendError(Error::UNABLE_TO_PROCESS);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();
    auto ids = ArcSoftEngine::Get().storedFaceIds();
    endOp();
    mCallback->onEnrollmentsEnumerated(ids);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& ids) {
    if (ids.empty()) {
        mCallback->onEnrollmentsRemoved({});
        return ndk::ScopedAStatus::ok();
    }
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();

    const auto stored = ArcSoftEngine::Get().storedFaceIds();
    for (int32_t id : ids) {
        if (std::find(stored.begin(), stored.end(), id) == stored.end()) {
            LOG(WARNING) << "removeEnrollments: unknown id " << id;
            sendError(Error::UNABLE_TO_REMOVE);
            return ndk::ScopedAStatus::ok();
        }
    }

    /* maxEnrollmentsPerUser == 1 and the ArcSoft database has no per-id delete,
     * so every valid request targets the only enrollment that exists. */
    const bool ok = ArcSoftEngine::Get().removeAll();
    endOp();
    if (!ok) {
        sendError(Error::UNABLE_TO_REMOVE);
        return ndk::ScopedAStatus::ok();
    }
    mCallback->onEnrollmentsRemoved(ids);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();

    if (ArcSoftEngine::Get().storedFaceCount() == 0) {
        sendError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }

    std::vector<Feature> enabled;
    for (Feature feature : {Feature::REQUIRE_ATTENTION, Feature::REQUIRE_DIVERSE_POSES}) {
        bool on = false;
        if (ArcSoftEngine::Get().getFeature(static_cast<int32_t>(feature), &on) && on) {
            enabled.push_back(feature);
        }
    }
    endOp();
    mCallback->onFeaturesRetrieved(enabled);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const keymaster::HardwareAuthToken& /*hat*/, Feature feature,
                                       bool enabled) {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();

    if (ArcSoftEngine::Get().storedFaceCount() == 0) {
        sendError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }

    const bool ok = ArcSoftEngine::Get().setFeature(static_cast<int32_t>(feature), enabled);
    endOp();
    if (!ok) {
        sendError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    mCallback->onFeatureSet(feature);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    if (isClosed()) return ndk::ScopedAStatus::ok();
    /* ISession.aidl: sensors configured as WEAK or CONVENIENCE MUST return 0.
     * This sensor is CONVENIENCE, so no per-user authenticator id is kept. */
    mCallback->onAuthenticatorIdRetrieved(0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    if (isClosed()) return ndk::ScopedAStatus::ok();
    /* Only meaningful for STRONG sensors; acknowledge immediately. */
    mCallback->onAuthenticatorIdInvalidated(0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken& /*hat*/) {
    if (!beginOp(Op::OTHER)) return ndk::ScopedAStatus::ok();
    /* No lockout state is kept: CONVENIENCE sensors rely on the framework
     * throttling. Just acknowledge the request. */
    endOp();
    mCallback->onLockoutCleared();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    closeInternal(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t id, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(id, out);
}

ndk::ScopedAStatus Session::enrollWithContext(
        const keymaster::HardwareAuthToken& hat, EnrollmentType type,
        const std::vector<Feature>& features,
        const std::optional<::aidl::android::hardware::common::NativeHandle>& surface,
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, type, features, surface, out);
}

ndk::ScopedAStatus Session::enrollWithOptions(
        const FaceEnrollOptions& options,
        std::shared_ptr<common::ICancellationSignal>* out) {
    /* surfacePreview supersedes the deprecated nativeHandlePreview field. */
    setPreviewWindow(options.surfacePreview.has_value() ? options.surfacePreview->get() : nullptr);
    return startEnroll(options.surfacePreview.has_value(), out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
