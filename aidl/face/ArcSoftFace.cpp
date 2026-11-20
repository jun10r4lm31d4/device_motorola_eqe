#define LOG_TAG "face-moto-arcsoft"

#include "ArcSoftFace.h"

#include <android-base/logging.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace motorola::face::arcsoft {

namespace {

/* Must stay constant: it is part of the key used to bind templates to a device
 * in the vendor algorithm. It is not secret and not user specific. */
constexpr const char* kDeviceTag = "motorola-eqe-face";
constexpr const char* kTemplateFile = "facedata.bin";
constexpr const char* kFeaturesFile = "features.bin";

constexpr int32_t kFeatureCount = 64;

/* Códigos ARC_FI_MERR_* usados aqui (ver ArcConstant.smali). */
constexpr int kArcNoFace = 0x7002;
constexpr int kArcRecognizeFail = 0x7006;
constexpr int kArcProcessing = 0x700d;
constexpr int kArcRegisterFeatureError = 0x7010;

/* A cola só conclui o enroll quando o contador de frames válidos passa de 0x14. */
constexpr int kEnrollFramesNeeded = 21;
/* Sem frames por este tempo => começou outro enroll (a cola zera o contador em
 * arcSaveFeatureStop/arcCompareStart, que o HAL nunca chamava). */
constexpr auto kEnrollGap = std::chrono::seconds(2);

/* Limites de sanidade ao ler o banco. */
constexpr int32_t kMaxTemplates = 16;
constexpr int32_t kMaxTemplateBytes = 8 * 1024 * 1024;
constexpr int32_t kEnrollmentId = 1; /* registId gravado pelo saveFeature stock */

std::string userDir(int32_t userId) {
    return "/data/vendor_de/" + std::to_string(userId) + "/facedata";
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' && i + 1 != path.size()) continue;
        if (current == "/") continue;
        if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
            PLOG(ERROR) << "mkdir " << current;
            return false;
        }
    }
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>* out, bool* exists) {
    *exists = false;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    *exists = true;
    out->clear();
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        out->insert(out->end(), buf, buf + n);
    }
    ::close(fd);
    return true;
}

bool writeAll(int fd, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ssize_t n = ::write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        PLOG(ERROR) << "open " << path;
        return false;
    }
    const bool ok = writeAll(fd, data, size);
    ::close(fd);
    return ok;
}

bool readFile(const std::string& path, void* data, size_t size) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t got = ::read(fd, data, size);
    ::close(fd);
    return got == static_cast<ssize_t>(size);
}

template <typename T>
bool getInt(const std::vector<uint8_t>& buf, size_t* off, T* v) {
    if (*off + sizeof(T) > buf.size()) return false;
    memcpy(v, buf.data() + *off, sizeof(T));
    *off += sizeof(T);
    return true;
}

/* Formato idêntico ao da libarcsoft-lib (GetRegisterData/readFeatureData):
 *   int32 count
 *   count x { int32 len; int32 id; int32 encLen; uint8 payload[encLen] }
 * Sem criptografia: encLen == len e payload == template em claro (era o que o shim
 * encrypt()/decrypt() fazia). Retorna false se o arquivo existe mas é inválido
 * (por exemplo, gravado pelo APK stock com a chave do Keystore, em que encLen != len). */
bool parseDb(const std::vector<uint8_t>& buf, std::vector<std::pair<int32_t, std::vector<uint8_t>>>* out) {
    out->clear();
    size_t off = 0;
    int32_t count = 0;
    if (!getInt(buf, &off, &count)) return false;
    if (count <= 0) return count == 0;
    if (count > kMaxTemplates) return false;
    for (int32_t i = 0; i < count; ++i) {
        int32_t len = 0, id = 0, encLen = 0;
        if (!getInt(buf, &off, &len) || !getInt(buf, &off, &id) || !getInt(buf, &off, &encLen)) {
            return false;
        }
        if (len <= 0 || len > kMaxTemplateBytes || encLen != len) return false;
        if (off + static_cast<size_t>(encLen) > buf.size()) return false;
        out->emplace_back(id, std::vector<uint8_t>(buf.begin() + off, buf.begin() + off + encLen));
        off += static_cast<size_t>(encLen);
    }
    return true;
}

template <typename T>
void putInt(std::vector<uint8_t>* buf, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf->insert(buf->end(), p, p + sizeof(T));
}

/* Gravação atômica (tmp + rename) para não deixar o banco pela metade. */
bool saveDb(const std::string& path, const std::vector<std::pair<int32_t, std::vector<uint8_t>>>& items) {
    std::vector<uint8_t> buf;
    putInt<int32_t>(&buf, static_cast<int32_t>(items.size()));
    for (const auto& it : items) {
        putInt<int32_t>(&buf, static_cast<int32_t>(it.second.size()));
        putInt<int32_t>(&buf, it.first);
        putInt<int32_t>(&buf, static_cast<int32_t>(it.second.size()));
        buf.insert(buf.end(), it.second.begin(), it.second.end());
    }
    const std::string tmp = path + ".tmp";
    if (!writeFile(tmp, buf.data(), buf.size())) {
        ::unlink(tmp.c_str());
        return false;
    }
    int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        PLOG(ERROR) << "rename " << tmp;
        ::unlink(tmp.c_str());
        return false;
    }
    ::chmod(path.c_str(), 0600);
    return true;
}

/* NV21: plane[0] = Y, plane[1] = VU intercalado logo depois do Y; pitch = largura.
 * Mesmo preenchimento de Java_..._arcCompare()/arcSaveFeature(). */
bool fillImage(ASVLOFFSCREEN* img, const uint8_t* nv21, int32_t width, int32_t height) {
    if (img == nullptr || nv21 == nullptr || width <= 0 || height <= 0) return false;
    *img = ASVLOFFSCREEN{};
    img->format = kAsvlPafNv21;
    img->width = width;
    img->height = height;
    img->plane[0] = const_cast<uint8_t*>(nv21);
    img->plane[1] = const_cast<uint8_t*>(nv21) + static_cast<size_t>(width) * static_cast<size_t>(height);
    img->pitch[0] = width;
    img->pitch[1] = width;
    return true;
}

}  // namespace

ArcSoftEngine& ArcSoftEngine::Get() {
    static ArcSoftEngine instance;
    return instance;
}

bool ArcSoftEngine::setUser(int32_t userId) {
    std::lock_guard<std::mutex> lock(mLock);
    return ensureInitLocked(userId);
}

/* Equivale ao arcInit() da cola: Init -> SetDeviceUniqueTag -> SetRegisterData -> SetProcessDataType. */
bool ArcSoftEngine::initHandleLocked() {
    MHandle handle = nullptr;
    MRESULT rc = ARC_FI_Init(&handle, 1);
    if (rc != 0 || handle == nullptr) {
        LOG(ERROR) << "ARC_FI_Init failed: 0x" << std::hex << rc;
        return false;
    }

    rc = ARC_FI_SetDeviceUniqueTag(handle, kDeviceTag, static_cast<int32_t>(strlen(kDeviceTag)));
    if (rc != 0) LOG(WARNING) << "ARC_FI_SetDeviceUniqueTag: 0x" << std::hex << rc;

    if (!mTemplates.empty()) {
        std::vector<ArcRegisterRecord> records(mTemplates.size());
        for (size_t i = 0; i < mTemplates.size(); ++i) {
            records[i] = ArcRegisterRecord{};
            records[i].data = mTemplates[i].data.data();
            records[i].size = static_cast<int32_t>(mTemplates[i].data.size());
            records[i].registId = mTemplates[i].id;
        }
        rc = ARC_FI_SetRegisterData(handle, records.data(), static_cast<int32_t>(records.size()));
        if (rc != 0) {
            /* O stock ignora esta falha e segue; o banco carregado fica inutilizável,
             * mas um novo enroll ainda é possível. */
            LOG(ERROR) << "ARC_FI_SetRegisterData failed: 0x" << std::hex << rc;
        }
    }
    ARC_FI_SetProcessDataType(handle, 1);

    mHandle = handle;
    mEnrollFrames = 0;
    return true;
}

void ArcSoftEngine::releaseHandleLocked() {
    if (mHandle != nullptr) {
        ARC_FI_Uninit(&mHandle);
        mHandle = nullptr;
    }
    mEnrollFrames = 0;
}

bool ArcSoftEngine::reloadHandleLocked() {
    releaseHandleLocked();
    mReady = initHandleLocked();
    return mReady;
}

bool ArcSoftEngine::ensureInitLocked(int32_t userId) {
    if (mReady && mUserId == userId) return true;

    if (mReady || mHandle != nullptr) {
        releaseHandleLocked();
        mReady = false;
    }
    mTemplates.clear();
    mUserId = -1;
    mDir.clear();

    if (userId < 0) {
        LOG(ERROR) << "invalid user id " << userId;
        return false;
    }

    std::string dir = userDir(userId);
    /* vold creates this directory (system:system 0700, face_vendor_data_file);
     * mkdir is only a fallback for the first boot / user 0. */
    if (!makeDirs(dir)) {
        LOG(ERROR) << "cannot prepare " << dir;
        return false;
    }

    const std::string path = dir + "/" + kTemplateFile;
    std::vector<uint8_t> raw;
    bool exists = false;
    if (!readWholeFile(path, &raw, &exists)) {
        LOG(ERROR) << "cannot read " << path;
        return false;
    }
    if (exists) {
        std::vector<std::pair<int32_t, std::vector<uint8_t>>> items;
        if (parseDb(raw, &items)) {
            for (auto& it : items) mTemplates.push_back(Template{it.first, std::move(it.second)});
        } else {
            /* Banco inválido (ex.: cifrado pelo APK). O stock também seguia com 0 faces. */
            LOG(WARNING) << path << " is not a valid plaintext database; ignoring it";
        }
    }

    if (!initHandleLocked()) return false;

    mUserId = userId;
    mDir = std::move(dir);
    mReady = true;
    LOG(INFO) << "ArcSoft ready for user " << userId << ", stored faces=" << mTemplates.size();
    return true;
}

bool ArcSoftEngine::ready() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mReady;
}

std::string ArcSoftEngine::pathLocked(const char* name) const {
    return mDir + "/" + name;
}

int32_t ArcSoftEngine::storedFaceCount() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mReady ? static_cast<int32_t>(mTemplates.size()) : 0;
}

std::vector<int32_t> ArcSoftEngine::storedFaceIds() const {
    std::lock_guard<std::mutex> lock(mLock);
    std::vector<int32_t> ids;
    if (!mReady) return ids;
    for (const auto& t : mTemplates) ids.push_back(t.id);
    return ids;
}

bool ArcSoftEngine::removeAll() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mReady) return false;

    const std::string path = pathLocked(kTemplateFile);
    const bool unlinked = (::unlink(path.c_str()) == 0 || errno == ENOENT);
    if (!unlinked) PLOG(ERROR) << "unlink " << path;
    ::unlink(pathLocked(kFeaturesFile).c_str());
    ::unlink((path + ".tmp").c_str());

    if (unlinked) mTemplates.clear();

    /* A engine mantém os templates carregados em memória: sem recarregar, a face
     * removida continuaria autenticando até o próximo boot. */
    if (!reloadHandleLocked()) LOG(ERROR) << "re-init after removal failed";

    return ::access(path.c_str(), F_OK) != 0;
}

bool ArcSoftEngine::getFeature(int32_t feature, bool* enabled) const {
    if (enabled == nullptr || feature < 0 || feature >= kFeatureCount) return false;

    std::lock_guard<std::mutex> lock(mLock);
    if (!mReady) return false;

    uint64_t mask = 0;
    (void)readFile(pathLocked(kFeaturesFile), &mask, sizeof(mask));
    *enabled = (mask & (1ULL << feature)) != 0;
    return true;
}

bool ArcSoftEngine::setFeature(int32_t feature, bool enabled) {
    if (feature < 0 || feature >= kFeatureCount) return false;

    std::lock_guard<std::mutex> lock(mLock);
    if (!mReady) return false;

    const std::string path = pathLocked(kFeaturesFile);
    uint64_t mask = 0;
    (void)readFile(path, &mask, sizeof(mask));
    if (enabled) {
        mask |= (1ULL << feature);
    } else {
        mask &= ~(1ULL << feature);
    }
    if (!writeFile(path, &mask, sizeof(mask))) {
        LOG(ERROR) << "failed to persist features";
        return false;
    }
    return true;
}

/* compare() da cola: DetectFace -> Recognize -> Reset. */
int ArcSoftEngine::compareFrame(const uint8_t* nv21, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mReady || mHandle == nullptr) return -1;

    ASVLOFFSCREEN image;
    if (!fillImage(&image, nv21, width, height)) return -1;

    mEnrollFrames = 0; /* arcCompareStart zerava o contador do enroll */

    ArcFaceInfo faces{};
    MRESULT rc = ARC_FI_DetectFace(mHandle, &image, &faces);
    if (rc != 0) return rc;
    if (faces.faceCount == 0) return kArcNoFace;

    ArcRecognizeResult result{};
    rc = ARC_FI_Recognize(mHandle, &image, &faces, &result);
    if (rc == 0 && result.matched == 0) rc = kArcRecognizeFail;
    ARC_FI_Reset(mHandle);
    return rc;
}

/* saveFeature() da cola: DetectFace -> ExtractFeaturesForRegister -> (21 frames) ->
 * SetRegisterData -> persistir -> Reset. */
int ArcSoftEngine::enrollFrame(const uint8_t* nv21, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mReady || mHandle == nullptr) return -1;

    ASVLOFFSCREEN image;
    if (!fillImage(&image, nv21, width, height)) return -1;

    const auto now = std::chrono::steady_clock::now();
    if (now - mLastEnrollFrame > kEnrollGap) {
        /* Novo enroll: zera o contador e o estado acumulado da ArcSoft. */
        mEnrollFrames = 0;
        ARC_FI_Reset(mHandle);
    }
    mLastEnrollFrame = now;

    ArcFaceInfo faces{};
    MRESULT rc = ARC_FI_DetectFace(mHandle, &image, &faces);
    if (rc != 0) return rc;
    if (faces.faceCount == 0) return kArcNoFace;

    ArcRegisterFeatures features{};
    rc = ARC_FI_ExtractFeaturesForRegister(mHandle, &image, &faces, &features);
    if (rc == 0 || rc == kArcProcessing) ++mEnrollFrames;
    if (rc != 0) {
        /* 0x700d (PROCESSING) e 0x7013 (BAD_FACE) são códigos de orientação normais:
         * não reinicialize aqui, senão os frames acumulados pela ArcSoft se perdem e o
         * enroll nunca termina. */
        LOG(INFO) << "ExtractFeaturesForRegister: 0x" << std::hex << rc << " (continuing)";
        return rc;
    }
    if (features.featureCount <= 0 || mEnrollFrames < kEnrollFramesNeeded) return kArcProcessing;

    std::vector<ArcRegisterRecord> records;
    for (int32_t i = 0; i < features.featureCount && i < 5; ++i) {
        if (features.feature[i].data == nullptr || features.feature[i].size <= 0) continue;
        ArcRegisterRecord r{};
        r.data = features.feature[i].data;
        r.size = features.feature[i].size;
        r.registId = kEnrollmentId;
        records.push_back(r);
    }
    if (records.empty()) {
        ARC_FI_Reset(mHandle);
        return kArcRegisterFeatureError;
    }

    rc = ARC_FI_SetRegisterData(mHandle, records.data(), static_cast<int32_t>(records.size()));
    if (rc != 0) {
        LOG(ERROR) << "ARC_FI_SetRegisterData failed: 0x" << std::hex << rc;
        ARC_FI_Reset(mHandle);
        return rc;
    }

    /* Copie ANTES do Reset: os ponteiros pertencem à ArcSoft. */
    std::vector<Template> fresh;
    for (const auto& r : records) {
        const auto* p = static_cast<const uint8_t*>(r.data);
        fresh.push_back(Template{r.registId, std::vector<uint8_t>(p, p + r.size)});
    }
    ARC_FI_Reset(mHandle);

    std::vector<std::pair<int32_t, std::vector<uint8_t>>> items;
    for (const auto& t : fresh) items.emplace_back(t.id, t.data);
    if (!saveDb(pathLocked(kTemplateFile), items)) {
        LOG(ERROR) << "failed to persist the face database";
        reloadHandleLocked(); /* devolve à engine o conjunto antigo, igual ao disco */
        return -1;
    }

    mTemplates = std::move(fresh);
    mEnrollFrames = 0;
    return 0;
}

}  // namespace motorola::face::arcsoft
