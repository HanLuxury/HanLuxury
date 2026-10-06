package com.holy.game.core;

/**
 * EAGLE graphics engine - JNI control surface (native side: jni/graphics/GraphicsJNI.cpp).
 *
 * All methods are safe to call from the UI thread: the native side only stores a
 * request that the game thread applies on the next frame. Rendering never runs in Java.
 * The class must stay in package com.holy.game.core (JNI symbol names depend on it).
 */
public final class GraphicsNative {
    private GraphicsNative() {}

    /** Re-reads graphics.ini and reloads the engine's own shader programs. */
    public static native void nativeReloadGraphics();

    /** Re-reads /storage/emulated/0/TESTLIT/graphics/graphics.ini. */
    public static native void nativeReloadConfig();

    /** Development hot reload of the engine's own shaders (debug overlay, post-process later). */
    public static native void nativeReloadShaders();

    /** 0 = LOW, 1 = MEDIUM, 2 = HIGH, 3 = ULTRA. */
    public static native void nativeSetGraphicsQuality(int quality);

    public static native void nativeSetGraphicsEnabled(boolean enabled);

    public static native void nativeSetShadowEnabled(boolean enabled);

    /** Shadow distance in metres (20..600). */
    public static native void nativeSetShadowDistance(float metres);

    /** Stored now, used by the post-process phase. */
    public static native void nativeSetBloom(boolean enabled, float intensity);

    /**
     * Debug flags: "showCascade", "showShadowMap", "showSunDirection", "freezeSun",
     * "freezeShadowCamera", "perfCounters", "log", "showDepth", "showSSAO".
     */
    public static native void nativeSetDebugFlag(String name, boolean value);

    /** One-line engine status (for a debug menu or logcat). */
    public static native String nativeGetStatus();
}
