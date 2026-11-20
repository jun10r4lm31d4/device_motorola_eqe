/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnFace.h>

#include <memory>
#include <mutex>

namespace aidl::android::hardware::biometrics::face {

class Session;

class Face : public BnFace {
  public:
    ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& callback,
                                     std::shared_ptr<ISession>* out) override;

  private:
    std::mutex mLock;
    std::weak_ptr<Session> mSession;
};

}  // namespace aidl::android::hardware::biometrics::face
