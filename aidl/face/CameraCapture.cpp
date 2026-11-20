#define LOG_TAG "face-moto-camera"

#include "CameraCapture.h"

#include <android-base/logging.h>

#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace motorola::face::camera {

namespace {

// Same format/size used by com.motorola.faceunlock.camera.Camera2Helper.
constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;
constexpr int32_t kMaxImages = 4;

void copyPlane(const uint8_t* src, int32_t srcPixelStride, int32_t srcRowStride,
               uint8_t* dst, int32_t width, int32_t height) {
    if (srcPixelStride == 1) {
        for (int32_t y = 0; y < height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * width,
                   src + static_cast<size_t>(y) * srcRowStride, width);
        }
    } else {
        for (int32_t y = 0; y < height; ++y) {
            const uint8_t* row = src + static_cast<size_t>(y) * srcRowStride;
            uint8_t* out = dst + static_cast<size_t>(y) * width;
            for (int32_t x = 0; x < width; ++x) out[x] = row[x * srcPixelStride];
        }
    }
}

/** AImageReader gives semi-planar YUV420; ArcSoft wants packed NV21. */
bool convertToNv21(AImage* image, CameraCapture::Frame* out) {
    int32_t width = 0;
    int32_t height = 0;
    int32_t planes = 0;
    if (AImage_getWidth(image, &width) != AMEDIA_OK ||
        AImage_getHeight(image, &height) != AMEDIA_OK ||
        AImage_getNumberOfPlanes(image, &planes) != AMEDIA_OK || planes < 3 || width <= 0 ||
        height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
        return false;
    }

    struct Plane {
        uint8_t* data = nullptr;
        int32_t pixelStride = 0;
        int32_t rowStride = 0;
    } plane[3];
    for (int32_t i = 0; i < 3; ++i) {
        int32_t len = 0;
        if (AImage_getPlaneData(image, i, &plane[i].data, &len) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, i, &plane[i].pixelStride) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, i, &plane[i].rowStride) != AMEDIA_OK ||
            plane[i].data == nullptr || plane[i].pixelStride <= 0 || plane[i].rowStride <= 0) {
            return false;
        }
    }

    out->width = width;
    out->height = height;
    out->nv21.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2);
    uint8_t* dst = out->nv21.data();
    const size_t ySize = static_cast<size_t>(width) * static_cast<size_t>(height);
    copyPlane(plane[0].data, plane[0].pixelStride, plane[0].rowStride, dst, width, height);

    /* NV21 stores V (Cr) then U (Cb) for every 2x2 block. AImageReader plane 1
     * is U/Cb and plane 2 is V/Cr, so the two have to be swapped here. */
    uint8_t* chroma = dst + ySize;
    for (int32_t y = 0; y < height / 2; ++y) {
        const uint8_t* u = plane[1].data + static_cast<size_t>(y) * plane[1].rowStride;
        const uint8_t* v = plane[2].data + static_cast<size_t>(y) * plane[2].rowStride;
        for (int32_t x = 0; x < width / 2; ++x) {
            chroma[0] = v[x * plane[2].pixelStride];
            chroma[1] = u[x * plane[1].pixelStride];
            chroma += 2;
        }
    }
    return true;
}

}  // namespace

CameraCapture::~CameraCapture() {
    stop();
}

void CameraCapture::setPreviewSurface(ANativeWindow* surface) {
    std::lock_guard<std::mutex> guard(mLock);
    if (mPreviewSurface != nullptr) ANativeWindow_release(mPreviewSurface);
    mPreviewSurface = surface;
    if (surface != nullptr) ANativeWindow_acquire(surface);
}

void CameraCapture::onDeviceDisconnected(void* context, ACameraDevice* /*device*/) {
    auto* self = static_cast<CameraCapture*>(context);
    if (self == nullptr) return;
    LOG(WARNING) << "camera device disconnected";
    {
        std::lock_guard<std::mutex> guard(self->mLock);
        self->mStreaming = false;
    }
    self->mCv.notify_all();
}

void CameraCapture::onDeviceError(void* context, ACameraDevice* /*device*/, int error) {
    auto* self = static_cast<CameraCapture*>(context);
    if (self == nullptr) return;
    LOG(ERROR) << "camera device error " << error;
    {
        std::lock_guard<std::mutex> guard(self->mLock);
        self->mStreaming = false;
    }
    self->mCv.notify_all();
}

void CameraCapture::onSessionClosed(void* context, ACameraCaptureSession* /*session*/) {
    auto* self = static_cast<CameraCapture*>(context);
    if (self == nullptr) return;
    std::lock_guard<std::mutex> guard(self->mLock);
    self->mStreaming = false;
}

void CameraCapture::onSessionReady(void* /*context*/, ACameraCaptureSession* /*session*/) {}

void CameraCapture::onSessionFailed(void* context, ACameraCaptureSession* /*session*/) {
    auto* self = static_cast<CameraCapture*>(context);
    if (self == nullptr) return;
    LOG(ERROR) << "camera capture session configuration failed";
    {
        std::lock_guard<std::mutex> guard(self->mLock);
        self->mStreaming = false;
    }
    self->mCv.notify_all();
}

void CameraCapture::onImageAvailable(void* context, AImageReader* reader) {
    auto* self = static_cast<CameraCapture*>(context);
    if (self == nullptr || reader == nullptr) return;

    AImage* image = nullptr;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    if (status != AMEDIA_OK || image == nullptr) return;

    Frame frame;
    if (convertToNv21(image, &frame)) self->publishFrame(frame);
    AImage_delete(image);
}

void CameraCapture::publishFrame(const Frame& frame) {
    {
        std::lock_guard<std::mutex> guard(mLock);
        if (mStopped) return;
        mLatest = frame;  // copying keeps this safe if the session reads at the same time
        mHasFrame = true;
    }
    mCv.notify_one();
}

bool CameraCapture::start() {
    {
        std::lock_guard<std::mutex> guard(mLock);
        if (!mCameraIds.empty()) return openCamera(0);
    }

    mManager = ACameraManager_create();
    if (mManager == nullptr) {
        LOG(ERROR) << "ACameraManager_create failed";
        return false;
    }

    ACameraIdList* ids = nullptr;
    camera_status_t status = ACameraManager_getCameraIdList(mManager, &ids);
    if (status != ACAMERA_OK || ids == nullptr) {
        LOG(ERROR) << "getCameraIdList failed: " << status;
        return false;
    }

    std::vector<std::string> front;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* characteristics = nullptr;
        if (ACameraManager_getCameraCharacteristics(mManager, ids->cameraIds[i],
                                                    &characteristics) != ACAMERA_OK ||
            characteristics == nullptr) {
            continue;
        }
        ACameraMetadata_const_entry entry{};
        uint8_t facing = 0xff;
        if (ACameraMetadata_getConstEntry(characteristics, ACAMERA_LENS_FACING, &entry) ==
                ACAMERA_OK &&
            entry.count > 0) {
            facing = entry.data.u8[0];
        }
        ACameraMetadata_free(characteristics);
        LOG(INFO) << "camera " << ids->cameraIds[i] << " facing=" << static_cast<int>(facing);
        /* Only front cameras are usable for face unlock. Every other camera
         * on this device is rear-facing, so there is no fallback target. */
        if (facing == ACAMERA_LENS_FACING_FRONT) {
            front.emplace_back(ids->cameraIds[i]);
        }
    }
    ACameraManager_deleteCameraIdList(ids);

    {
        std::lock_guard<std::mutex> guard(mLock);
        mCameraIds = std::move(front);
        if (mCameraIds.empty()) {
            LOG(ERROR) << "no front camera found";
            return false;
        }
        return openCamera(0);
    }
}

bool CameraCapture::openCamera(size_t index) {
    if (index >= mCameraIds.size()) return false;

    closeCamera();
    mCameraIndex = index;
    const char* id = mCameraIds[index].c_str();
    LOG(INFO) << "opening camera " << id;

    ACameraDevice_StateCallbacks deviceCallbacks{};
    deviceCallbacks.context = this;
    deviceCallbacks.onDisconnected = &CameraCapture::onDeviceDisconnected;
    deviceCallbacks.onError = &CameraCapture::onDeviceError;
    camera_status_t status =
            ACameraManager_openCamera(mManager, id, &deviceCallbacks, &mDevice);
    if (status != ACAMERA_OK || mDevice == nullptr) {
        LOG(ERROR) << "openCamera(" << id << ") failed: " << status;
        return false;
    }

    if (ACameraDevice_createCaptureRequest(mDevice, TEMPLATE_PREVIEW, &mRequest) !=
                ACAMERA_OK ||
        mRequest == nullptr) {
        LOG(ERROR) << "createCaptureRequest failed";
        return false;
    }

    /* The stock app asks for continuous AF. Not all cameras expose it, so a
     * failure here is not fatal for frame capture. */
    const uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    if (ACaptureRequest_setEntry_u8(mRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode) != ACAMERA_OK) {
        LOG(WARNING) << "cannot set continuous AF";
    }

    AImageReader_ImageListener listener{};
    listener.context = this;
    listener.onImageAvailable = &CameraCapture::onImageAvailable;
    if (AImageReader_new(kWidth, kHeight, AIMAGE_FORMAT_YUV_420_888, kMaxImages, &mReader) !=
            AMEDIA_OK ||
        mReader == nullptr) {
        LOG(ERROR) << "AImageReader_new failed";
        return false;
    }
    if (AImageReader_getWindow(mReader, &mWindow) != AMEDIA_OK || mWindow == nullptr) {
        LOG(ERROR) << "AImageReader_getWindow failed";
        return false;
    }
    AImageReader_setImageListener(mReader, &listener);

    if (ACaptureSessionOutputContainer_create(&mOutputs) != ACAMERA_OK || mOutputs == nullptr ||
        ACaptureSessionOutput_create(mWindow, &mOutput) != ACAMERA_OK || mOutput == nullptr ||
        ACaptureSessionOutputContainer_add(mOutputs, mOutput) != ACAMERA_OK ||
        ACameraOutputTarget_create(mWindow, &mTarget) != ACAMERA_OK || mTarget == nullptr ||
        ACaptureRequest_addTarget(mRequest, mTarget) != ACAMERA_OK) {
        LOG(ERROR) << "failed to configure camera outputs";
        return false;
    }

    /* The framework hands us the preview Surface as a HAL token. Rendering it
     * is optional for enrollment, but without this output the enrollment area
     * stays black even though frames are reaching ArcSoft. */
    if (mPreviewSurface != nullptr) {
        ANativeWindow* previewWindow = mPreviewSurface;
        const bool ok =
            ACaptureSessionOutput_create(previewWindow, &mPreviewOutput) == ACAMERA_OK &&
            mPreviewOutput != nullptr &&
            ACameraOutputTarget_create(previewWindow, &mPreviewTarget) == ACAMERA_OK &&
            mPreviewTarget != nullptr &&
            ACaptureSessionOutputContainer_add(mOutputs, mPreviewOutput) == ACAMERA_OK &&
            ACaptureRequest_addTarget(mRequest, mPreviewTarget) == ACAMERA_OK;
        /* Keep the pointers alive until closeCamera(): an output already added
         * to the container must not be freed before the session is torn down. */
        if (ok) {
            LOG(INFO) << "preview output configured";
        } else {
            LOG(WARNING) << "preview output rejected; continuing with capture only";
        }
    }

    ACameraCaptureSession_stateCallbacks sessionCallbacks{};
    sessionCallbacks.context = this;
    sessionCallbacks.onClosed = &CameraCapture::onSessionClosed;
    sessionCallbacks.onReady = &CameraCapture::onSessionReady;
    sessionCallbacks.onActive = nullptr;
    if (ACameraDevice_createCaptureSession(mDevice, mOutputs, &sessionCallbacks, &mSession) !=
                ACAMERA_OK ||
        mSession == nullptr) {
        LOG(ERROR) << "createCaptureSession failed";
        return false;
    }

    status = ACameraCaptureSession_setRepeatingRequest(mSession, nullptr, 1, &mRequest, nullptr);
    if (status != ACAMERA_OK) {
        LOG(ERROR) << "setRepeatingRequest failed: " << status;
        return false;
    }
    mStreaming = true;
    mStopped = false;
    LOG(INFO) << "streaming from camera " << id;
    return true;
}

void CameraCapture::closeCamera() {
    if (mReader != nullptr) AImageReader_setImageListener(mReader, nullptr);
    if (mSession != nullptr) {
        if (mStreaming) ACameraCaptureSession_stopRepeating(mSession);
        ACameraCaptureSession_close(mSession);
        mSession = nullptr;
    }
    mStreaming = false;
    if (mPreviewTarget != nullptr) {
        ACameraOutputTarget_free(mPreviewTarget);
        mPreviewTarget = nullptr;
    }
    if (mPreviewOutput != nullptr) {
        ACaptureSessionOutput_free(mPreviewOutput);
        mPreviewOutput = nullptr;
    }
    if (mTarget != nullptr) {
        ACameraOutputTarget_free(mTarget);
        mTarget = nullptr;
    }
    if (mOutput != nullptr) {
        ACaptureSessionOutput_free(mOutput);
        mOutput = nullptr;
    }
    if (mOutputs != nullptr) {
        ACaptureSessionOutputContainer_free(mOutputs);
        mOutputs = nullptr;
    }
    if (mRequest != nullptr) {
        ACaptureRequest_free(mRequest);
        mRequest = nullptr;
    }
    if (mDevice != nullptr) {
        ACameraDevice_close(mDevice);
        mDevice = nullptr;
    }
    if (mReader != nullptr) {
        AImageReader_delete(mReader);
        mReader = nullptr;
    }
    mWindow = nullptr;
    mHasFrame = false;
}

bool CameraCapture::waitForFrame(int timeoutMs, Frame* out) {
    if (out == nullptr) return false;
    std::unique_lock<std::mutex> lock(mLock);
    if (mCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                    [this] { return mHasFrame || mStopped || !mStreaming; })) {
        if (mStopped || !mHasFrame) return false;
        *out = std::move(mLatest);
        mHasFrame = false;
        return true;
    }
    return false;
}

void CameraCapture::stop() {
    {
        std::lock_guard<std::mutex> guard(mLock);
        mStopped = true;
    }
    mCv.notify_all();
    closeCamera();
    std::lock_guard<std::mutex> guard(mLock);
    if (mManager != nullptr) {
        ACameraManager_delete(mManager);
        mManager = nullptr;
    }
    mCameraIds.clear();
    mCameraIndex = 0;
    if (mPreviewSurface != nullptr) {
        ANativeWindow_release(mPreviewSurface);
        mPreviewSurface = nullptr;
    }
}

}  // namespace motorola::face::camera
