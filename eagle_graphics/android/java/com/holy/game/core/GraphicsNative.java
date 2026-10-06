package com.holy.game.core;

import android.util.Log;

import com.holy.launcher.storage.Storage;

/**
 * EAGLE graphics engine - JNI control surface (native side: jni/graphics/GraphicsJNI.cpp).
 *
 * Every native call only records a request; the game thread applies it on the next
 * frame, so all methods are safe on the UI thread. Rendering never runs in Java.
 * Keep this class in package com.holy.game.core: the JNI symbol names depend on it.
 *
 * Config.ini + Advanced.ini (/storage/emulated/0/TESTLIT/graphics/) hold the defaults.
 * Values the player changes in the "Grafis" settings tab are kept in SharedPreferences
 * and applied again on every start by {@link #applySavedSettings()}. Live shader values
 * (shaderUniform.ini) are written back to that file by {@link #nativeSaveShaderUniforms()}.
 */
public final class GraphicsNative {
    private static final String TAG = "EagleGFX";

    private static final String KEY_SAVED = "eagle_gfx_saved";
    private static final String KEY_ENABLED = "eagle_gfx_enabled";
    private static final String KEY_SHADOWS = "eagle_gfx_shadows";
    private static final String KEY_QUALITY = "eagle_gfx_quality";
    private static final String KEY_DISTANCE = "eagle_gfx_shadow_distance";

    public static final int QUALITY_LOW = 0;
    public static final int QUALITY_MEDIUM = 1;
    public static final int QUALITY_HIGH = 2;
    public static final int QUALITY_ULTRA = 3;

    private GraphicsNative() {}

    // ---- requests
    /** Re-reads the config files and reloads the engine's own shader programs. */
    public static native void nativeReloadGraphics();

    /** Re-reads Config.ini, Advanced.ini, shaderUniform.ini and data/eagle_timecyc.dat. */
    public static native void nativeReloadConfig();

    /** Development hot reload of the engine's own shaders (debug overlay, post-process later). */
    public static native void nativeReloadShaders();

    /** 0 = LOW, 1 = MEDIUM, 2 = HIGH, 3 = ULTRA (also resets the shadow preset values). */
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

    // ---- current values (pending requests included)
    public static native boolean nativeIsGraphicsEnabled();

    public static native boolean nativeIsShadowEnabled();

    public static native int nativeGetGraphicsQuality();

    public static native float nativeGetShadowDistance();

    public static native boolean nativeGetDebugFlag(String name);

    /** One-line engine status (for a debug menu or logcat). */
    public static native String nativeGetStatus();

    // ---- shaderUniform.ini: live shader values (no restart, applied on the next frame)

    /** One entry per line: class \t name \t type(bool|int|float) \t value \t min \t max \t step \t used(0|1). */
    public static native String nativeGetShaderUniforms();

    /** Returns false if the class/name is unknown. The value is clamped to min..max. */
    public static native boolean nativeSetShaderUniform(String cls, String name, float value);

    /** Writes every value back to shaderUniform.ini. */
    public static native boolean nativeSaveShaderUniforms();

    /** Values back to the defaults declared in the glShader/*.shader files (not saved). */
    public static native void nativeResetShaderUniforms();

    // ---- persistence of the player's choices

    /** Stores the current engine values as the player's settings. */
    public static void saveUserSettings() {
        try {
            Storage.setBoolean(KEY_ENABLED, nativeIsGraphicsEnabled());
            Storage.setBoolean(KEY_SHADOWS, nativeIsShadowEnabled());
            Storage.setInt(KEY_QUALITY, nativeGetGraphicsQuality());
            Storage.setInt(KEY_DISTANCE, Math.round(nativeGetShadowDistance()));
            Storage.setBoolean(KEY_SAVED, true);
        } catch (Throwable t) {
            Log.w(TAG, "saveUserSettings failed", t);
        }
    }

    /** Forgets the player's settings: Config.ini decides again from the next start. */
    public static void clearUserSettings() {
        try {
            Storage.setBoolean(KEY_SAVED, false);
        } catch (Throwable t) {
            Log.w(TAG, "clearUserSettings failed", t);
        }
    }

    /** Call once after initSAMP(): re-applies what the player chose in the settings tab. */
    public static void applySavedSettings() {
        try {
            if (!Storage.getBoolean(KEY_SAVED)) return;
            // Quality first: it loads the preset, the explicit distance overrides it.
            nativeSetGraphicsQuality(clampQuality(Storage.getInt(KEY_QUALITY)));
            int distance = Storage.getInt(KEY_DISTANCE);
            if (distance >= 20) nativeSetShadowDistance(distance);
            nativeSetGraphicsEnabled(Storage.getBoolean(KEY_ENABLED));
            nativeSetShadowEnabled(Storage.getBoolean(KEY_SHADOWS));
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "libmultiplayer.so without the EAGLE graphics engine", e);
        } catch (Throwable t) {
            Log.w(TAG, "applySavedSettings failed", t);
        }
    }

    public static int clampQuality(int quality) {
        return Math.max(QUALITY_LOW, Math.min(QUALITY_ULTRA, quality));
    }

    public static String qualityName(int quality) {
        switch (clampQuality(quality)) {
            case QUALITY_LOW: return "LOW";
            case QUALITY_MEDIUM: return "MEDIUM";
            case QUALITY_HIGH: return "HIGH";
            default: return "ULTRA";
        }
    }
}
