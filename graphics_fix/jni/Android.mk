LOCAL_PATH := $(call my-dir)


# ==========================================
# 0. AndroidModLoader (AML)
# ==========================================
# libAML.so adalah core AML ASLI (prebuilt, AML Core 1.3.1). Jangan diganti
# dengan hasil build AML/src/*.cpp: isi folder itu hanya shim lama, bukan
# source AndroidModLoader.
#
# PENTING: modul ini TIDAK BOLEH dicantumkan di LOCAL_SHARED_LIBRARIES milik
# `multiplayer`.
#   - libAML.so membawa libc++ statis sendiri (libc++ NDK lama) dan mengekspor
#     +-1570 simbol std::__ndk1 + __cxa_*/operator new, ditambah curl, wolfSSL,
#     zlib dan GlossHook (total +-5500 simbol).
#   - Kalau ia jadi DT_NEEDED libmultiplayer.so, urutannya di depan
#     libc++_shared.so. Linker Android mencari simbol menurut urutan itu, jadi
#     libmultiplayer.so (dan libc++_shared.so sendiri) terikat ke potongan
#     libc++ lama di dalam libAML.so untuk simbol yang ada di sana, dan ke
#     libc++_shared.so untuk sisanya: dua runtime C++ tercampur dalam satu
#     proses.
#   - Tanpa libAML.so di APK, libmultiplayer.so jadi gagal dimuat sama sekali.
#
# Cara yang benar sama seperti AML upstream (System.loadLibrary("AML")):
# libAML.so dimuat sebagai library tersendiri lewat dlopen() di
# loader/AmlBootstrap.cpp. Tidak ada kode client yang menautkan simbol AML.
#
# Modul prebuilt di bawah tetap ada supaya ndk-build menyalin libAML.so ke
# libs/arm64-v8a/: tanpa APP_MODULES, ndk-build meng-install semua modul
# shared (termasuk PREBUILT_SHARED_LIBRARY) yang dideklarasikan di Android.mk
# ini walaupun tidak direferensikan modul lain.
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)

include $(CLEAR_VARS)
LOCAL_MODULE := AML
LOCAL_SRC_FILES := AML/prebuilt/arm64-v8a/libAML.so
include $(PREBUILT_SHARED_LIBRARY)

# APP_MODULES membatasi modul yang di-install; AML harus ikut disebut di sana.
ifneq ($(strip $(APP_MODULES) $(NDK_APP_MODULES)),)
ifeq ($(filter AML,$(APP_MODULES) $(NDK_APP_MODULES)),)
$(warning AML tidak ada di APP_MODULES: libAML.so TIDAK disalin ke libs/arm64-v8a. Tambahkan AML ke APP_MODULES.)
endif
endif

endif

# ==========================================
# 1. PREBUILT LIBRARIES (Dependensi)
# ==========================================

# --- Prebuilt: Opus (Static) ---
include $(CLEAR_VARS)
LOCAL_MODULE := opus_static
# Menggunakan file biner Opus yang sudah jadi (tanpa build ulang)
LOCAL_SRC_FILES := $(LOCAL_PATH)/vendor/opus/libopus.a
include $(PREBUILT_STATIC_LIBRARY)

# --- Prebuilt: Bass (Shared) ---
include $(CLEAR_VARS)
LOCAL_MODULE := bass_shared
# Disinkronkan dengan CMake: menggunakan path relatif dari source dir
LOCAL_SRC_FILES := $(LOCAL_PATH)/vendor/bass/libs/$(TARGET_ARCH_ABI)/libbass.so
include $(PREBUILT_SHARED_LIBRARY)

# --- Prebuilt: Shadowhook (Shared) ---
include $(CLEAR_VARS)
LOCAL_MODULE := shadowhook
# Disinkronkan dengan CMake: menggunakan path relatif dari source dir
LOCAL_SRC_FILES := $(LOCAL_PATH)/vendor/shadowhook/libs/$(TARGET_ARCH_ABI)/libshadowhook.so
include $(PREBUILT_SHARED_LIBRARY)

# ==========================================
# 2. MAIN MODULE: multiplayer
# ==========================================
include $(CLEAR_VARS)
LOCAL_MODULE := postfx_gloss
LOCAL_SRC_FILES := AML/third_party/gloss/arm64-v8a/libGlossHook.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := multiplayer

# --- Flags & C++ Standard ---
LOCAL_CPP_FEATURES := exceptions rtti
# Disinkronkan dengan CMake: menambahkan -fstack-protector-strong, menghapus -g
LOCAL_CPPFLAGS := -std=c++20 -s -w -O3 -fexceptions -fstack-protector-strong -pthread

# --- ABI Specific Definitions & Architecture Flags ---
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
    LOCAL_CPPFLAGS += -DVER_x32=true
    # Flag arsitektur CMake untuk 32-bit
    LOCAL_CPPFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=softfp
else ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
    LOCAL_CPPFLAGS += -DVER_x32=false
    # Flag arsitektur CMake untuk 64-bit
    LOCAL_CPPFLAGS += -march=armv8-a -mtune=cortex-a78
    # Linker flag CMake untuk 64-bit (max-page-size=65536)
    LOCAL_LDFLAGS += -Wl,-z,max-page-size=65536
endif

# --- Include Directories ---
# Disinkronkan dengan include_directories() di CMakeLists.txt
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/vendor/imgui \
    $(LOCAL_PATH)/vendor \
    $(LOCAL_PATH)/game/RW \
    $(LOCAL_PATH)/game \
    $(LOCAL_PATH)/java_systems \
    $(LOCAL_PATH)/game/Core \
    $(LOCAL_PATH)/vendor/shadowhook/include \
    $(LOCAL_PATH)/vendor/opus

# --- Source Files ---
# Sistem auto-detect mendeteksi semua file .cpp, .c, .cc.
# Pengecualian folder /opus/ tetap dipertahankan untuk optimasi kompilasi.
MY_SRC_FILES := $(shell find $(LOCAL_PATH) -type f \( -name "*.cpp" -o -name "*.c" -o -name "*.cc" \) | grep -v "/opus/" | grep -v "/AML/" | grep -v "/modloader/tests/" | grep -v "/vicecity/tests/")
LOCAL_SRC_FILES := $(MY_SRC_FILES:$(LOCAL_PATH)/%=%)

# --- System Libraries ---
# Disinkronkan dengan target_link_libraries() di CMake (hanya log dan GLESv3)
LOCAL_LDLIBS := -llog -lGLESv3 -lEGL -ldl
LOCAL_LDFLAGS += -Wl,--exclude-libs,libGlossHook.a:libpostfx_gloss.a

# --- Link to Prebuilt Libraries ---
LOCAL_STATIC_LIBRARIES := opus_static postfx_gloss
# AML sengaja tidak ada di sini (lihat bagian 0): dimuat lewat dlopen saat runtime.
LOCAL_SHARED_LIBRARIES := bass_shared shadowhook

include $(BUILD_SHARED_LIBRARY)
