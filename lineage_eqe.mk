#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from eqe device
$(call inherit-product, device/motorola/eqe/device.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Boot animation
TARGET_SCREEN_HEIGHT := 2712
TARGET_SCREEN_WIDTH := 1220

# Characteristics
PRODUCT_CHARACTERISTICS := nosdcard

# Lineage Charger
WITH_LINEAGE_CHARGER := false

PRODUCT_NAME := lineage_eqe
PRODUCT_DEVICE := eqe
PRODUCT_MANUFACTURER := motorola
PRODUCT_BRAND := motorola
PRODUCT_MODEL := motorola edge 50 pro

PRODUCT_GMS_CLIENTID_BASE := android-motorola

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="eqe_g-user 16 W1UM36H.19-13-4 3258a8-65d2c release-keys" \
    BuildFingerprint=motorola/eqe_g/eqe:16/W1UM36H.19-13-4/3258a8-65d2c:user/release-keys \
    DeviceProduct=eqe_g
