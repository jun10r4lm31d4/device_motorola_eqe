KSU_DIR := device/motorola/eqe/prebuilt/kernelsu
KSU_KO := $(KSU_DIR)/kernelsu.ko
KSU_INIT := $(KSU_DIR)/ksuinit

$(TARGET_RAMDISK_OUT)/kernelsu.ko: $(KSU_KO) \
                                   $(KSU_INIT) \
                                   $(INTERNAL_RAMDISK_FILES)
	$(hide) mkdir -p $(TARGET_RAMDISK_OUT)
	$(hide) if [ -f "$(TARGET_RAMDISK_OUT)/init" ] && [ ! -f "$(TARGET_RAMDISK_OUT)/init.real" ]; then \
		mv $(TARGET_RAMDISK_OUT)/init $(TARGET_RAMDISK_OUT)/init.real; \
		chmod 755 $(TARGET_RAMDISK_OUT)/init.real; \
	fi
	$(hide) cp $(KSU_KO) $(TARGET_RAMDISK_OUT)/kernelsu.ko
	$(hide) cp $(KSU_INIT) $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 755 $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 644 $(TARGET_RAMDISK_OUT)/kernelsu.ko

$(INSTALLED_RAMDISK_TARGET): $(TARGET_RAMDISK_OUT)/kernelsu.ko
