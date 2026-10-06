cmake_minimum_required(VERSION 3.12)
project(multiplayer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    add_definitions(-DVER_x32=true)
elseif(${ANDROID_ABI} STREQUAL "arm64-v8a")
    add_definitions(-DVER_x32=false)
endif()

# Add source files
file(GLOB_RECURSE SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/game/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/app/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/game/**/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/game/**/**/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/game/**/**/**/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/net/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/java_systems/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/java_systems/casino/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/util/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/gui/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vendor/ini/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vendor/RakNet/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vendor/RakNet/SAMP/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vendor/imgui/*.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vendor/hash/*.cpp
        )

# Add include directories
include_directories(
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/vendor/imgui
        ${CMAKE_CURRENT_LIST_DIR}/vendor/
        ${CMAKE_CURRENT_LIST_DIR}/game/RW/
        ${CMAKE_CURRENT_LIST_DIR}/game/
        ${CMAKE_CURRENT_LIST_DIR}/java_systems
        ${CMAKE_CURRENT_LIST_DIR}/game/Core
)

list(FILTER SOURCES EXCLUDE REGEX "/AML/")
list(FILTER SOURCES EXCLUDE REGEX "/modloader/tests/")

# EAGLE graphics engine (jni/graphics). Already matched by the recursive glob
# above; listed explicitly so the module is visible and never silently dropped.
set(EAGLE_GRAPHICS_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GraphicsEngine.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GraphicsConfig.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GraphicsLog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GraphicsHooks.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GraphicsJNI.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GameRenderBridge.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/RenderQueueBridge.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GLCaps.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/GLStateBackup.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/ShaderManager.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/ShaderPatcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/FrameBuffer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/SunManager.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/TimeCycleFX.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/CascadeShadow.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/ShadowCasters.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/ShadowManager.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/DebugOverlay.cpp
        ${CMAKE_CURRENT_LIST_DIR}/graphics/Math/Matrix4.cpp
)
list(APPEND SOURCES ${EAGLE_GRAPHICS_SOURCES})
list(REMOVE_DUPLICATES SOURCES)

add_library(multiplayer SHARED ${SOURCES})

# EGL postFX uses the existing arm64 GlossHook archive, no new GTA offsets.
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "EGL postFX package targets arm64-v8a")
endif()
add_library(postfx_gloss STATIC IMPORTED)
set_target_properties(postfx_gloss PROPERTIES IMPORTED_LOCATION
    "${CMAKE_CURRENT_LIST_DIR}/AML/third_party/gloss/arm64-v8a/libGlossHook.a")
set_property(TARGET multiplayer APPEND_STRING PROPERTY LINK_FLAGS
    " -Wl,--exclude-libs,libGlossHook.a")
target_link_libraries(multiplayer
        log
        GLESv3
        EGL
        dl
        postfx_gloss
        opus
        shadowhook::shadowhook
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/bass/libs/${ANDROID_ABI}/libbass.so
)

# Set compilation flags
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s -w -fstack-protector -Wall -O3 -fexceptions") # old
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Og -fexceptions -pthread -g3 -gdwarf-5") # Debug

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s -w -O3 -fexceptions -fstack-protector-strong -pthread")

if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=cortex-a78")
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon -mfloat-abi=softfp")
endif()

if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=65536")
endif()
# ==========================================
# AndroidModLoader - ORIGINAL PREBUILT
# ==========================================
if(ANDROID_ABI STREQUAL "arm64-v8a")
    add_library(AML SHARED IMPORTED GLOBAL)
    set_target_properties(AML PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/AML/prebuilt/arm64-v8a/libAML.so")
endif()
