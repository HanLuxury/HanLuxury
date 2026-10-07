#pragma once

#include <jni.h>
#include <cstdint>

class CMobileMenu;

// ============================================================================
//  Modern pause menu / map / HUD layout editor (Definitive Edition style).
//  --------------------------------------------------------------------------
//  - The Android back button no longer opens the GTA FlowScreen pause menu
//    while connected to a server. MobileMenu::InitForPause shows the Java
//    pause menu (com.holy.game.gui.modern.ModernMenu) instead.
//  - The map keeps the native GTA map renderer (radar tiles, blips, gang
//    zones, GPS path, pinch zoom, waypoint). Only the old menu chrome is
//    hidden; the Java overlay draws the modern top bar and buttons.
//  - Java only posts requests (UI thread). Every GTA menu call runs on the
//    game thread inside the MobileMenu::Update hook.
// ============================================================================
class CModernMenu {
public:
    enum eRequest : int32_t {
        REQUEST_NONE            = 0,
        REQUEST_RESUME          = 1,  // Java pause menu closed
        REQUEST_OPEN_MAP        = 2,
        REQUEST_CLOSE_MAP       = 3,
        REQUEST_ZOOM_IN         = 4,
        REQUEST_ZOOM_OUT        = 5,
        REQUEST_CENTER_PLAYER   = 6,
        REQUEST_TOGGLE_WAYPOINT = 7,
        REQUEST_GTA_SETTINGS    = 8,  // native GTA settings (graphics, controls, adjust HUD)
        REQUEST_LAYOUT_OPEN     = 9,
        REQUEST_LAYOUT_CLOSE    = 10,
        REQUEST_MAP_TO_PAUSE    = 11, // leave the map, back to the pause menu
        REQUEST_GTA_CONTROLS    = 12, // GTA touch button editor (CAdjustableHUD) only
        REQUEST_MAP_LEAVE       = 13, // map tab left: close the map, pause menu stays
        REQUEST_SET_SETTING     = 14, // arg = (setting id << 16) | value
        REQUEST_GFX_SAVE        = 15, // GRAFIS "Simpan": write settings.ini + GTA settings now
        REQUEST_GFX_RESET       = 16, // GRAFIS "Reset": client graphics back to defaults
    };

    enum class eState : uint8_t {
        NONE,
        PAUSE,          // Java pause menu visible
        MAP,            // native map + Java map overlay
        GTA_SETTINGS,   // native settings screens opened from the modern menu
        LAYOUT_EDITOR,  // Java HUD layout editor
        GTA_CONTROLS,   // GTA touch button editor, opened straight from the modern menu
    };

    // com.holy.game.gui.modern.ModernMenu, loaded on first use.
    static inline jclass clazz = nullptr;

    static void InjectHooks();

    // Called from the MobileMenu::InitForPause hook. Returns true when the
    // modern menu handled the request and the native pause menu must not open.
    static bool OnInitForPause(CMobileMenu* menu);

    // The native menu opened by the modern menu itself must pass through.
    static bool IsBypassingNativePause();

    // Single producer: the Java UI thread.
    static void PostRequest(int32_t request, int32_t arg = 0);

    static eState GetState();

    // Older integrations called this from CLocalPlayer. The Java side hides
    // its HUD by itself now, so it always returns false. Defined out of line
    // so objects built against the older header still link.
    static bool IsHudBlocked();

    // Game thread, around MobileMenu::Update.
    static void ProcessRequests(CMobileMenu* menu);
    static void AfterUpdate(CMobileMenu* menu);
};
