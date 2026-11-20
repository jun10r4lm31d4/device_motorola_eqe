/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <android/native_window.h>

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>

namespace motorola::face::camera {

/**
 * Minimal native camera2 client used by the face HAL.
 *
 * It uses the NDK camera2 vendor variant (libcamera2ndk_vendor), which talks to
 * cameraserver through android.frameworks.cameraservice.service.ICameraService.
 * The HIDL camera provider is deliberately not used: neither the provider AIDL
 * interfaces nor libvendorcamera2 are available to this HAL.
 *
 * Frames are produced by an AImageReader configured with
 * AIMAGE_FORMAT_YUV_420_888 at 640x480 (the same settings as the stock
 * MotoFaceUnlockArcSoft APK) and are converted to packed NV21 before being
 * handed to ArcSoft.
 */
class CameraCapture {
  public:
    struct Frame {
        std::vector<uint8_t> nv21;
        int32_t width = 0;
        int32_t height = 0;
    };

    CameraCapture() = default;
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    /** Preview surface obtained from the framework's HAL token (may be null). */
    void setPreviewSurface(ANativeWindow* surface);

    /** Enumerate cameras and start a repeating request on the front camera. */
    bool start();

    /** Copy the newest frame into |out|. Blocks at most |timeoutMs|. */
    bool waitForFrame(int timeoutMs, Frame* out);

    /** Stop the stream and release every camera resource. */
    void stop();

  private:
    bool openCamera(size_t index);
    void closeCamera();
    bool configureStream();

    static void onDeviceDisconnected(void* context, ACameraDevice* device);
    static void onDeviceError(void* context, ACameraDevice* device, int error);
    static void onSessionClosed(void* context, ACameraCaptureSession* session);
    static void onSessionReady(void* context, ACameraCaptureSession* session);
    static void onSessionFailed(void* context, ACameraCaptureSession* session);
    static void onImageAvailable(void* context, AImageReader* reader);
    void publishFrame(const Frame& frame);

    mutable std::mutex mLock;
    std::condition_variable mCv;

    ACameraManager* mManager = nullptr;
    ACameraDevice* mDevice = nullptr;
    ACameraCaptureSession* mSession = nullptr;
    ACaptureRequest* mRequest = nullptr;
    ACaptureSessionOutputContainer* mOutputs = nullptr;
    ACaptureSessionOutput* mOutput = nullptr;
    ACameraOutputTarget* mTarget = nullptr;
    ACaptureSessionOutput* mPreviewOutput = nullptr;
    ACameraOutputTarget* mPreviewTarget = nullptr;
    ANativeWindow* mPreviewSurface = nullptr;
    AImageReader* mReader = nullptr;
    ANativeWindow* mWindow = nullptr;

    std::vector<std::string> mCameraIds;
    size_t mCameraIndex = 0;
    Frame mLatest;
    bool mHasFrame = false;
    bool mStreaming = false;
    bool mStopped = false;
};

}  // namespace motorola::face::camera
