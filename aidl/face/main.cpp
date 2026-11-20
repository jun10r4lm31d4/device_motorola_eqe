#define LOG_TAG "face-moto"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Face.h"

using aidl::android::hardware::biometrics::face::Face;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    auto face = ndk::SharedRefBase::make<Face>();
    const std::string instance = std::string(Face::descriptor) + "/default";
    CHECK_EQ(STATUS_OK, AServiceManager_addService(face->asBinder().get(), instance.c_str()));

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
