package com.holy.game.gui.modern

/**
 * GTA SA mobile setting IDs (MobileSettings::settings, eMobileSettings in the
 * client). Values, ranges and visibility come from the game at runtime.
 */
object GtaSettings {
    const val VISUALS = 0
    const val RESOLUTION = 1
    const val DRAW_DISTANCE = 2
    const val STEER_TYPE = 3
    const val TRAFFIC = 4
    const val SHADOWS = 5
    const val TARGETING = 6
    const val CAR_REFLECTIONS = 7
    const val ACCELEROMETER = 8
    const val TOUCH_LAYOUT = 9
    const val CAM_HEIGHT = 10
    const val SFX_VOLUME = 11
    const val MUSIC_VOLUME = 12
    const val AUTOTUNE = 13
    const val INVERT_LOOK = 15
    const val BRIGHTNESS = 16
    const val STEER_ANALOG_SCALE = 23
    const val SUBTITLES = 24
    const val VIBRATION = 29
    const val FRAME_LIMITER = 30
    const val AUTO_CLIMB = 35

    /** eMobileSettings::MS_MAX; nativeGetSettings() returns 4 ints per setting. */
    const val COUNT = 37

    // Client graphics (GraphicsSettings.h, ids from kMenuBase = 64). Same
    // 4-int layout in nativeGetSettings(); saved in TESTLIT/SAMP/settings.ini.
    const val GFX_BASE = 64
    const val GFX_PRESET = GFX_BASE + 0
    const val GFX_POSTFX = GFX_BASE + 1
    const val GFX_SUN_SHADOWS = GFX_BASE + 2
    const val GFX_SHADOW_QUALITY = GFX_BASE + 3
    const val GFX_SHADOW_DISTANCE = GFX_BASE + 4
    const val GFX_SOFT_SHADOWS = GFX_BASE + 5
    const val GFX_AO = GFX_BASE + 6
    const val GFX_REFLECTIONS = GFX_BASE + 7
    const val GFX_SUN_RAYS = GFX_BASE + 8
    const val GFX_BLOOM = GFX_BASE + 9
    const val GFX_SKY = GFX_BASE + 10
    const val GFX_WET_ROADS = GFX_BASE + 11
    const val GFX_WEATHER = GFX_BASE + 12
    const val GFX_FOG = GFX_BASE + 13
    const val GFX_GRASS = GFX_BASE + 14
    const val GFX_FXAA = GFX_BASE + 15
    const val GFX_MSAA = GFX_BASE + 16
    const val GFX_MOTION_BLUR = GFX_BASE + 17
    const val GFX_DOF = GFX_BASE + 18
    const val GFX_ANISOTROPIC = GFX_BASE + 19
    const val GFX_ADAPTIVE = GFX_BASE + 20
    const val GFX_TAA = GFX_BASE + 21
}
