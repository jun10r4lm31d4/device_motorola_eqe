KSU_PATH := device/motorola/eqe/build/tasks

$(TARGET_RAMDISK_OUT)/kernelsu.ko: $(KSU_PATH)/android13-5.15_kernelsu.ko \
                                   $(KSU_PATH)/ksuinit \
                                   $(INTERNAL_RAMDISK_FILES)
	$(hide) mkdir -p $(TARGET_RAMDISK_OUT)
	$(hide) if [ -f "$(TARGET_RAMDISK_OUT)/init" ] && [ ! -f "$(TARGET_RAMDISK_OUT)/init.real" ]; then \
		mv $(TARGET_RAMDISK_OUT)/init $(TARGET_RAMDISK_OUT)/init.real; \
		chmod 755 $(TARGET_RAMDISK_OUT)/init.real; \
	fi
	$(hide) cp $(KSU_PATH)/android13-5.15_kernelsu.ko $(TARGET_RAMDISK_OUT)/kernelsu.ko
	$(hide) cp $(KSU_PATH)/ksuinit $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 755 $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 644 $(TARGET_RAMDISK_OUT)/kernelsu.ko

$(INSTALLED_RAMDISK_TARGET): $(TARGET_RAMDISK_OUT)/kernelsu.ko

.PHONY: ksu_bacon
ksu_bacon: ksu_init_inject bacon
