// EAGLE graphics engine - JNI control surface.
// Java side: com.holy.game.core.GraphicsNative (docs/java/GraphicsNative.java).
// Every call only records a request; the game thread applies it on the next
// frame. No rendering work is done on the Java thread.

#include "GraphicsEngine.h"

#include <jni.h>
#include <string>

using gfx::GraphicsEngine;

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

JNIEXPORT jstring JNICALL Java_com_holy_game_core_GraphicsNative_nativeGetStatus(JNIEnv* env, jclass) {
    if (!env) return nullptr;
    const std::string status = GraphicsEngine::Get().StatusString();
    return env->NewStringUTF(status.c_str());
}

} // extern "C"
