// EAGLE graphics engine - JNI control surface.
// Java side: com.holy.game.core.GraphicsNative (android/java/.../GraphicsNative.java).
// Every call only records a request; the game thread applies it on the next
// frame. No rendering work is done on the Java thread.

#include "GraphicsEngine.h"
#include "GraphicsPaths.h"
#include "ShaderUniforms.h"

#include <jni.h>
#include <string>

using gfx::GraphicsEngine;
using gfx::ShaderUniforms;

namespace {
std::string JString(JNIEnv* env, jstring text) {
    if (!env || !text) return std::string();
    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (!chars) return std::string();
    std::string out(chars);
    env->ReleaseStringUTFChars(text, chars);
    return out;
}
} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeReloadGraphics(JNIEnv*, jclass) {
    GraphicsEngine::Get().RequestReloadConfig();
    GraphicsEngine::Get().RequestReloadShaders();
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeReloadConfig(JNIEnv*, jclass) {
    GraphicsEngine::Get().RequestReloadConfig();
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeReloadShaders(JNIEnv*, jclass) {
    GraphicsEngine::Get().RequestReloadShaders();
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetGraphicsQuality(JNIEnv*, jclass, jint quality) {
    GraphicsEngine::Get().SetQuality(static_cast<int>(quality));
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetGraphicsEnabled(JNIEnv*, jclass, jboolean enabled) {
    GraphicsEngine::Get().SetEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetShadowEnabled(JNIEnv*, jclass, jboolean enabled) {
    GraphicsEngine::Get().SetShadowEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetShadowDistance(JNIEnv*, jclass, jfloat metres) {
    GraphicsEngine::Get().SetShadowDistance(static_cast<float>(metres));
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetBloom(JNIEnv*, jclass, jboolean enabled,
                                                                            jfloat intensity) {
    GraphicsEngine::Get().SetBloom(enabled == JNI_TRUE, static_cast<float>(intensity));
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetDebugFlag(JNIEnv* env, jclass, jstring name,
                                                                                jboolean value) {
    if (!env || !name) return;
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (!chars) return;
    const std::string flag(chars);
    env->ReleaseStringUTFChars(name, chars);
    GraphicsEngine::Get().SetDebugFlag(flag, value == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_com_holy_game_core_GraphicsNative_nativeIsGraphicsEnabled(JNIEnv*, jclass) {
    return GraphicsEngine::Get().ConfigSnapshot().graphics.enabled ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_holy_game_core_GraphicsNative_nativeIsShadowEnabled(JNIEnv*, jclass) {
    return GraphicsEngine::Get().ConfigSnapshot().shadow.enabled ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetGraphicsQuality(JNIEnv*, jclass) {
    return static_cast<jint>(GraphicsEngine::Get().ConfigSnapshot().graphics.quality);
}

JNIEXPORT jfloat JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetShadowDistance(JNIEnv*, jclass) {
    return static_cast<jfloat>(GraphicsEngine::Get().ConfigSnapshot().shadow.distance);
}

JNIEXPORT jboolean JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetDebugFlag(JNIEnv* env, jclass, jstring name) {
    if (!env || !name) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (!chars) return JNI_FALSE;
    const std::string flag(chars);
    env->ReleaseStringUTFChars(name, chars);
    return GraphicsEngine::Get().GetDebugFlag(flag) ? JNI_TRUE : JNI_FALSE;
}

// ---- shaderUniform.ini (live shader values, applied on the next frame without a shader rebuild)

JNIEXPORT jstring JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetShaderUniforms(JNIEnv* env, jclass) {
    if (!env) return nullptr;
    const std::string list = ShaderUniforms::Get().Serialize();
    return env->NewStringUTF(list.c_str());
}

JNIEXPORT jboolean JNICALL Java_com_holy_game_core_GraphicsNative_nativeSetShaderUniform(JNIEnv* env, jclass, jstring cls,
                                                                                       jstring name, jfloat value) {
    return ShaderUniforms::Get().Set(JString(env, cls), JString(env, name), static_cast<float>(value)) ? JNI_TRUE
                                                                                                       : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_holy_game_core_GraphicsNative_nativeSaveShaderUniforms(JNIEnv*, jclass) {
    return ShaderUniforms::Get().Save(gfx::paths::kShaderUniform) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_holy_game_core_GraphicsNative_nativeResetShaderUniforms(JNIEnv*, jclass) {
    ShaderUniforms::Get().ResetToDefaults();
}

JNIEXPORT jstring JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetStatus(JNIEnv* env, jclass) {
    if (!env) return nullptr;
    const std::string status = GraphicsEngine::Get().StatusString();
    return env->NewStringUTF(status.c_str());
}

} // extern "C"
