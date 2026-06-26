KSU_BASE_URL := https://github.com/tiann/KernelSU/releases/latest/download
KSU_KO_URL := $(KSU_BASE_URL)/android13-5.15_kernelsu.ko
KSU_INIT_URL := $(KSU_BASE_URL)/ksuinit

KSU_INTERMEDIATES := $(call intermediates-dir-for,PACKAGING,kernelsu)
KSU_KO_INTERMEDIATE := $(KSU_INTERMEDIATES)/android13-5.15_kernelsu.ko
KSU_INIT_INTERMEDIATE := $(KSU_INTERMEDIATES)/ksuinit

$(shell mkdir -p $(KSU_INTERMEDIATES))
$(shell /usr/bin/curl -sLk $(KSU_INIT_URL) -o $(KSU_INIT_INTERMEDIATE))
$(shell /usr/bin/curl -sLk $(KSU_KO_URL) -o $(KSU_KO_INTERMEDIATE))

$(TARGET_RAMDISK_OUT)/kernelsu.ko: $(KSU_KO_INTERMEDIATE) \
                                   $(KSU_INIT_INTERMEDIATE) \
                                   $(INTERNAL_RAMDISK_FILES)
	$(hide) mkdir -p $(TARGET_RAMDISK_OUT)
	$(hide) if [ -f "$(TARGET_RAMDISK_OUT)/init" ] && [ ! -f "$(TARGET_RAMDISK_OUT)/init.real" ]; then \
		mv $(TARGET_RAMDISK_OUT)/init $(TARGET_RAMDISK_OUT)/init.real; \
		chmod 755 $(TARGET_RAMDISK_OUT)/init.real; \
	fi
	$(hide) cp $(KSU_KO_INTERMEDIATE) $(TARGET_RAMDISK_OUT)/kernelsu.ko
	$(hide) cp $(KSU_INIT_INTERMEDIATE) $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 755 $(TARGET_RAMDISK_OUT)/init
	$(hide) chmod 644 $(TARGET_RAMDISK_OUT)/kernelsu.ko

$(INSTALLED_RAMDISK_TARGET): $(TARGET_RAMDISK_OUT)/kernelsu.ko
