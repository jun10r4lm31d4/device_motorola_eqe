# device/motorola/eqe/vendorsetup.sh
KSU_KO_URL="https://github.com/KernelSU-Next/KernelSU-Next/releases/latest/download/aarch64_android13-5.15_kernelsu.ko"
KSU_INIT_URL="https://github.com/tiann/KernelSU/releases/latest/download/ksuinit-aarch64"

KSU_DIR="$(gettop)/device/motorola/eqe/prebuilt/kernelsu"
KSU_KO="$KSU_DIR/kernelsu.ko"
KSU_INIT="$KSU_DIR/ksuinit"

echo "$KSU_DIR $KSU_KO $KSU_INIT"
echo "[eqe] Downloading KernelSU prebuilts..."
mkdir -p "$KSU_DIR"
curl -sLk "$KSU_KO_URL" -o "$KSU_KO"
curl -sLk "$KSU_INIT_URL" -o "$KSU_INIT"
