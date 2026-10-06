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

PRODUCT_NAME := lineage_eqe
PRODUCT_SYSTEM_NAME := eqe_g
PRODUCT_DEVICE := eqe
PRODUCT_MANUFACTURER := motorola
PRODUCT_BRAND := motorola
PRODUCT_MODEL := motorola edge 50 pro

PRODUCT_GMS_CLIENTID_BASE := android-motorola

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="eqe_g-user 16 W1UMS36H.52-73-8-7 0507ff-869c5 release-keys MW-324" \
    BuildFingerprint=motorola/eqe_g/eqe:16/W1UMS36H.52-73-8-7/0507ff-869c5:user/release-keys \
    DeviceProduct=eqe_g

# Flags
TARGET_INCLUDES_DolbyVision := true
