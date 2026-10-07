#include "ModernMenu.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <string>
#include <dlfcn.h>

#include "main.h"
#include "../game/game.h"
#include "../game/common.h"
#include "../game/Core/Vector2D.h"
#include "../game/Mobile/MobileMenu/MobileMenu.h"
#include "net/netgame.h"
#include "util/patch.h"
#include "util/CJavaWrapper.h"
#include "util/util.h"
#include "net/playerpool.h"
#include "HUD.h"
#include "../game/Mobile/MobileSettings/MobileSettings.h"
#include "../game/Widgets/TouchInterface.h"
#include "../game/EntryExitManager.h"
#include "../graphics/GraphicsSettings.h"
#include "../CSettings.h"

extern void ApplyFPSPatch(uint8_t fps); // game/patches.cpp

namespace {

using ModernState = CModernMenu::eState;

std::atomic<ModernState> g_state{ ModernState::NONE };
bool g_bypassNativePause = false;

// The back press that closed a native screen can still read as "just down"
// on the next frame and would close the pause menu that just opened.
constexpr int64_t BACK_DEBOUNCE_MS = 350;
int64_t g_lastPauseShownMs = 0;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Small lock-free queue: Java (UI thread) posts, game thread drains.
// Each entry is request | (arg << 32). Large enough for a slider drag.
constexpr int REQUEST_QUEUE_SIZE = 128;
std::atomic<int64_t> g_requests[REQUEST_QUEUE_SIZE];
std::atomic<uint32_t> g_requestHead{ 0 };
std::atomic<uint32_t> g_requestTail{ 0 }; // written by the game thread, read by Java (queue full check)
// Settings are also saved shortly after the last change, not only when the
// menu closes: "Keluar dari game" or Android killing the app lost them.
int64_t g_settingsSaveAtMs = 0;

// Native settings opened from the modern menu.
bool g_settingsPending = false;   // OnSettings() not called yet
bool g_settingsSeenOpen = false;  // SettingsScreen reached the stack

// GTA touch button editor opened from the modern menu. GTA only runs its
// editor while a menu screen is open, so the same screens as the native path
// (pause -> settings -> controls) are opened hidden, one per frame.
enum class ControlsStep : uint8_t { OPEN_SETTINGS, OPEN_CONTROLS, OPEN_EDITOR, EDITING };
ControlsStep g_controlsStep = ControlsStep::OPEN_SETTINGS;
int g_controlsWaitFrames = 0;
constexpr int CONTROLS_MAX_WAIT_FRAMES = 180;  // give up if GTA never gets there

// Map zoom limits (same units as MobileMenu::NEW_MAP_SCALE, 448-high space).
constexpr float MAP_SCALE_MIN = 100.0f;
constexpr float MAP_SCALE_MAX = 1100.0f;  // Menu_MapUpdate clamps to 1100 as well
constexpr float MAP_ZOOM_STEP = 1.35f;
constexpr float MENU_SCREEN_HEIGHT = 448.0f;

// libGTASA functions, resolved once.
void (*MainMenuScreen_OnResume)(MenuScreen*) = nullptr;
void (*MainMenuScreen_OnSettings)(MenuScreen*) = nullptr;
void (*SettingsScreen_OnAdjustControls)(MenuScreen*) = nullptr;
void (*CAdjustableHUD_Toggle)() = nullptr;
void** CAdjustableHUD_m_pInstance = nullptr;
void (*MobileMenu_ProcessPending)(CMobileMenu*) = nullptr;
void (*MobileMenu_InitForPause)(CMobileMenu*) = nullptr;
void (*CRadar_SetMapCentreToPlayerCoords)() = nullptr;
void (*CRadar_TransformRadarPointToRealWorldSpace)(CVector2D&, const CVector2D&) = nullptr;
void (*PlaceRedMarker)(bool) = nullptr;
void (*Menu_ApplyAudioSettings)() = nullptr;
void (*Menu_SaveSettings)() = nullptr;
void (*CTouchInterface_SetupLayoutObjects)() = nullptr;

// Settings changed from the modern menu; written to disk when it closes.
bool g_settingsDirty = false;

template <typename T>
void Resolve(T& out, const char* sym) {
    out = reinterpret_cast<T>(CHook::lib ? dlsym(CHook::lib, sym) : nullptr);
    if (!out) Log("ModernMenu: symbol not found: %s", sym);
}

bool IsConnected() {
    return pNetGame && pNetGame->GetGameState() == eNetworkState::CONNECTED;
}

MenuScreen* TopScreen(CMobileMenu* menu) {
    if (!menu || menu->screenStack.numEntries == 0 || !menu->screenStack.dataPtr) return nullptr;
    return menu->screenStack.dataPtr[menu->screenStack.numEntries - 1];
}

bool IsNativeMenuOpen(CMobileMenu* menu) {
    return menu && (menu->screenStack.numEntries != 0 || menu->pendingScreen != nullptr);
}

// ---------------------------------------------------------------- Java calls
bool g_classLookupDone = false;

// Loads com.holy.game.gui.modern.ModernMenu through the activity's class
// loader (FindClass on the game thread only sees system classes). Returns
// false when the Java part is not in the APK; the GTA pause menu is used then.
bool EnsureJavaClass() {
    if (CModernMenu::clazz) return true;
    if (g_classLookupDone || !g_pJavaWrapper || !g_pJavaWrapper->activity) return false;
    g_classLookupDone = true;

    JNIEnv* env = CJavaWrapper::GetEnv();
    if (!env) return false;

    jobject activity = g_pJavaWrapper->activity;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader = activityClass
        ? env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = getClassLoader ? env->CallObjectMethod(activity, getClassLoader) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = loaderClass
        ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;

    jclass result = nullptr;
    if (loader && loadClass) {
        jstring name = env->NewStringUTF("com.holy.game.gui.modern.ModernMenu");
        result = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, name));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            result = nullptr;
        }
        if (name) env->DeleteLocalRef(name);
    }

    if (result) {
        CModernMenu::clazz = static_cast<jclass>(env->NewGlobalRef(result));
        env->DeleteLocalRef(result);
    } else {
        Log("ModernMenu: Java class not found, using the GTA pause menu");
    }

    if (loader) env->DeleteLocalRef(loader);
    if (loaderClass) env->DeleteLocalRef(loaderClass);
    if (activityClass) env->DeleteLocalRef(activityClass);
    return CModernMenu::clazz != nullptr;
}

JNIEnv* Env() {
    if (!g_pJavaWrapper || !CModernMenu::clazz) return nullptr;
    return CJavaWrapper::GetEnv();
}

void CallJavaVoid(const char* name) {
    JNIEnv* env = Env();
    if (!env) return;
    jmethodID method = env->GetStaticMethodID(CModernMenu::clazz, name, "()V");
    if (method) env->CallStaticVoidMethod(CModernMenu::clazz, method);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void JavaShowPause() {
    g_lastPauseShownMs = NowMs();
    JNIEnv* env = Env();
    if (!env) return;

    jmethodID method = env->GetStaticMethodID(CModernMenu::clazz, "showPause", "(Ljava/lang/String;IIII)V");
    if (!method) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    const char* name = CPlayerPool::GetLocalPlayerName();
    const std::string nameUtf = cp1251_to_utf8(name ? name : "");
    jstring jName = env->NewStringUTF(nameUtf.c_str());

    env->CallStaticVoidMethod(CModernMenu::clazz, method, jName,
                              static_cast<jint>(CPlayerPool::GetLocalPlayerID()),
                              static_cast<jint>(CPlayerPool::GetLocalPlayerScore()),
                              static_cast<jint>(CPlayerPool::GetLocalPlayerPing()),
                              static_cast<jint>(CHUD::iLocalMoney));
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jName) env->DeleteLocalRef(jName);
}

// Same test as MobileMenu::InitForPause: the pause was opened by tapping the
// radar (tap/hold time above 0.66 s) and no interior transition is running.
// GTA then opens the pause menu straight on the map.
constexpr float RADAR_TAP_TIME = 0.66f;

bool IsRadarTap() {
    if (!CTouchInterface::m_pWidgets) return false;
    const CWidgetGta* radar = CTouchInterface::m_pWidgets[WIDGET_RADAR];
    if (!radar) return false;
    const int32 enterState = CEntryExitManager::ms_exitEnterState;
    if (enterState == 1 || enterState == 2) return false;
    return radar->m_fTapHoldTime > RADAR_TAP_TIME;
}

// -------------------------------------------------------- native menu helpers
void OpenNativePause(CMobileMenu* menu) {
    if (!MobileMenu_InitForPause || IsNativeMenuOpen(menu)) return;
    g_bypassNativePause = true;
    MobileMenu_InitForPause(menu);
    g_bypassNativePause = false;
}

// Closes every native menu screen and switches back to the game, exactly like
// "Resume" in the GTA pause menu.
void CloseNativeMenu(CMobileMenu* menu) {
    if (!IsNativeMenuOpen(menu)) return;

    if (menu->pendingScreen && MobileMenu_ProcessPending) {
        MobileMenu_ProcessPending(menu);
    }

    MenuScreen* top = TopScreen(menu);
    if (top && MainMenuScreen_OnResume) {
        MainMenuScreen_OnResume(top);
    }
    menu->isMapMode = false;
}

void ScreenCentre(float& x, float& y) {
    const float w = static_cast<float>(RsGlobal->maximumWidth);
    const float h = static_cast<float>(RsGlobal->maximumHeight);
    x = (h > 0.0f ? MENU_SCREEN_HEIGHT * w / h : 640.0f) * 0.5f;
    y = MENU_SCREEN_HEIGHT * 0.5f;
}

// Zooms around the centre of the screen (the crosshair of the Java overlay).
void ZoomMap(CMobileMenu* menu, float factor) {
    if (!menu->isMapMode) return;

    const float scale = menu->NEW_MAP_SCALE;
    if (!(scale > 0.0f)) return;

    const float newScale = std::clamp(scale * factor, MAP_SCALE_MIN, MAP_SCALE_MAX);
    const float k = newScale / scale;

    float cx, cy;
    ScreenCentre(cx, cy);
    menu->MAP_OFFSET_X = cx - (cx - menu->MAP_OFFSET_X) * k;
    menu->MAP_OFFSET_Y = cy - (cy - menu->MAP_OFFSET_Y) * k;
    menu->NEW_MAP_SCALE = newScale;
}

// Places the waypoint under the crosshair, or removes the current one.
void ToggleWaypoint(CMobileMenu* menu) {
    if (!menu->isMapMode || !PlaceRedMarker || !CRadar_TransformRadarPointToRealWorldSpace) return;

    const float scale = menu->NEW_MAP_SCALE;
    if (!(scale > 0.0f)) return;

    // Same maths as CRadar::TransformScreenSpaceToRadarPoint.
    float cx, cy;
    ScreenCentre(cx, cy);
    CVector2D radar{ (cx - menu->MAP_OFFSET_X) / scale, (menu->MAP_OFFSET_Y - cy) / scale };

    CVector2D world{};
    CRadar_TransformRadarPointToRealWorldSpace(world, radar);
    menu->MAP_AREA_X = world.x;
    menu->MAP_AREA_Y = world.y;

    // true: place at MAP_AREA, or clear the waypoint if one already exists.
    PlaceRedMarker(true);
}

void SaveSettingsIfDirty() {
    // Client graphics live in the client's settings.ini (CSettings).
    if (GraphicsSettings::ConsumeDirty()) CSettings::save();
    if (!g_settingsDirty || !Menu_SaveSettings) return;
    g_settingsDirty = false;
    Menu_SaveSettings();
}

void SetState(ModernState state) {
    const ModernState old = g_state.exchange(state);
    // Same moment the GTA settings screen saves: when the menu is left.
    if (old == ModernState::PAUSE && state != ModernState::PAUSE) SaveSettingsIfDirty();
    // Back in the game: Java shows the HUD it hid for the menu.
    if (state == ModernState::NONE && old != ModernState::NONE) CallJavaVoid("onClosed");
}

// Same side effects as SelectScreen::SettingSelection::HandleInput.
void ApplySetting(int32_t id, int32_t value) {
    g_settingsSaveAtMs = NowMs() + 800;
    if (id >= GraphicsSettings::kMenuBase && id < GraphicsSettings::kMenuBase + GraphicsSettings::COUNT) {
        // Client graphics (GRAFIS tab): applied live, saved shortly after.
        GraphicsSettings::Set(id - GraphicsSettings::kMenuBase, value);
        return;
    }
    if (id < 0 || id >= MS_MAX) return;
    MobileSettings& setting = CMobileSettings::ms_MobileSettings[id];
    if (!setting.visible || setting.max <= setting.min) return;

    value = std::clamp(value, setting.min, setting.max);
    if (setting.value == value) return;
    setting.value = value;
    g_settingsDirty = true;

    switch (id) {
        case MS_TouchLayout:
            if (CTouchInterface_SetupLayoutObjects) CTouchInterface_SetupLayoutObjects();
            break;
        case MS_SFXVolume:
        case MS_MusicVolume:
            if (Menu_ApplyAudioSettings) Menu_ApplyAudioSettings();
            break;
        case MS_FrameLimiter:
            // Native DoGameState store is NOPed by ApplyFPSPatch: apply here.
            ApplyFPSPatch(static_cast<uint8_t>(std::clamp(CSettings::m_Settings.iFPS, 20, 255)));
            break;
        default:
            // Everything else is read by the game every frame.
            break;
    }
}

} // namespace

// ============================================================================

CModernMenu::eState CModernMenu::GetState() {
    return g_state.load();
}

bool CModernMenu::IsHudBlocked() {
    return false;
}

bool CModernMenu::IsBypassingNativePause() {
    return g_bypassNativePause;
}

void CModernMenu::PostRequest(int32_t request, int32_t arg) {
    const uint32_t head = g_requestHead.load();
    if (head - g_requestTail >= REQUEST_QUEUE_SIZE) return;  // full: drop (UI spam)
    const uint64_t packed = static_cast<uint32_t>(request) |
                            (static_cast<uint64_t>(static_cast<uint32_t>(arg)) << 32);
    g_requests[head % REQUEST_QUEUE_SIZE].store(static_cast<int64_t>(packed));
    g_requestHead.store(head + 1);
}

bool CModernMenu::OnInitForPause(CMobileMenu* menu) {
    if (!IsConnected() || !EnsureJavaClass()) return false;

    switch (g_state.load()) {
        case eState::PAUSE:
            if (NowMs() - g_lastPauseShownMs < BACK_DEBOUNCE_MS) return true;
            // Apply what the UI posted this frame first: once the state is no
            // longer PAUSE these SET_SETTING requests would be dropped.
            ProcessRequests(menu);
            if (g_state.load() != eState::PAUSE) return true;
            // Back pressed again: close the modern pause menu.
            SetState(eState::NONE);
            CallJavaVoid("hidePause");
            return true;

        case eState::LAYOUT_EDITOR:
            CallJavaVoid("onBackPressed");
            return true;

        case eState::MAP:
        case eState::GTA_SETTINGS:
        case eState::GTA_CONTROLS:
            // A native menu is open; InitForPause is not expected here.
            return true;

        case eState::NONE:
        default:
            break;
    }

    const bool openMap = IsRadarTap();
    SetState(eState::PAUSE);
    JavaShowPause();
    // Radar tapped: open the menu on the map tab (Java asks for the map).
    if (openMap) CallJavaVoid("openMapTab");
    return true;
}

void CModernMenu::ProcessRequests(CMobileMenu* menu) {
    while (g_requestTail != g_requestHead.load()) {
        const uint64_t packed = static_cast<uint64_t>(g_requests[g_requestTail % REQUEST_QUEUE_SIZE].load());
        const int32_t request = static_cast<int32_t>(packed & 0xFFFFFFFFu);
        const int32_t arg = static_cast<int32_t>(packed >> 32);
        ++g_requestTail;

        const auto state = g_state.load();
        switch (request) {
            case REQUEST_RESUME:
                if (state == eState::PAUSE) SetState(eState::NONE);
                break;

            case REQUEST_OPEN_MAP:
                if (state != eState::PAUSE && state != eState::NONE) break;
                OpenNativePause(menu);
                if (!IsNativeMenuOpen(menu)) {
                    // Stay in the pause menu; Java leaves the map tab.
                    SetState(eState::PAUSE);
                    CallJavaVoid("onMapFailed");
                    break;
                }
                // Straight to the full map; fade the old menu chrome out at once.
                menu->isMapMode = true;
                if (menu->pendingScreen) menu->pendingScreen->opacity = 0.0f;
                if (MenuScreen* top = TopScreen(menu)) top->opacity = 0.0f;
                SetState(eState::MAP);
                break;

            case REQUEST_CLOSE_MAP:
                if (state != eState::MAP) break;
                CloseNativeMenu(menu);
                SetState(eState::NONE);
                break;

            case REQUEST_MAP_LEAVE:
                // The map is a tab of the pause menu: close only the map.
                if (state != eState::MAP) break;
                CloseNativeMenu(menu);
                SetState(eState::PAUSE);
                break;

            case REQUEST_SET_SETTING:
                if (state != eState::PAUSE && state != eState::MAP) break;
                ApplySetting((arg >> 16) & 0xFFFF, static_cast<int16_t>(arg & 0xFFFF));
                break;

            case REQUEST_GFX_SAVE:
                // Explicit "Simpan": client settings.ini and GTA's own settings now,
                // instead of waiting until the menu is closed.
                if (state != eState::PAUSE) break;
                GraphicsSettings::ConsumeDirty();
                CSettings::save();
                if (Menu_SaveSettings) {
                    g_settingsDirty = false;
                    Menu_SaveSettings();
                }
                break;

            case REQUEST_GFX_RESET:
                if (state != eState::PAUSE) break;
                GraphicsSettings::ResetDefaults();
                g_settingsSaveAtMs = NowMs() + 800; // saved like any other change
                break;

            case REQUEST_MAP_TO_PAUSE:
                if (state != eState::MAP) break;
                CloseNativeMenu(menu);
                SetState(eState::PAUSE);
                JavaShowPause();
                break;

            case REQUEST_ZOOM_IN:
                if (state == eState::MAP) ZoomMap(menu, MAP_ZOOM_STEP);
                break;

            case REQUEST_ZOOM_OUT:
                if (state == eState::MAP) ZoomMap(menu, 1.0f / MAP_ZOOM_STEP);
                break;

            case REQUEST_CENTER_PLAYER:
                if (state == eState::MAP && menu->isMapMode && CRadar_SetMapCentreToPlayerCoords)
                    CRadar_SetMapCentreToPlayerCoords();
                break;

            case REQUEST_TOGGLE_WAYPOINT:
                if (state == eState::MAP) ToggleWaypoint(menu);
                break;

            case REQUEST_GTA_SETTINGS:
                if (state != eState::PAUSE && state != eState::NONE) break;
                OpenNativePause(menu);
                if (!IsNativeMenuOpen(menu)) {
                    SetState(eState::NONE);
                    break;
                }
                g_settingsPending = true;
                g_settingsSeenOpen = false;
                SetState(eState::GTA_SETTINGS);
                break;

            case REQUEST_GTA_CONTROLS:
                if (state != eState::PAUSE && state != eState::NONE) break;
                if (!MainMenuScreen_OnSettings || !SettingsScreen_OnAdjustControls ||
                    !CAdjustableHUD_Toggle || !CAdjustableHUD_m_pInstance) {
                    if (state == eState::PAUSE) JavaShowPause();
                    break;
                }
                OpenNativePause(menu);
                if (!IsNativeMenuOpen(menu)) {
                    SetState(eState::NONE);
                    break;
                }
                if (menu->pendingScreen) menu->pendingScreen->opacity = 0.0f;
                g_controlsStep = ControlsStep::OPEN_SETTINGS;
                g_controlsWaitFrames = 0;
                SetState(eState::GTA_CONTROLS);
                break;

            case REQUEST_LAYOUT_OPEN:
                if (state == eState::PAUSE || state == eState::NONE) SetState(eState::LAYOUT_EDITOR);
                break;

            case REQUEST_LAYOUT_CLOSE:
                if (state == eState::LAYOUT_EDITOR) SetState(eState::NONE);
                break;

            default:
                break;
        }
    }
    // Debounced save, 0.8 s after the last change.
    if (g_settingsSaveAtMs != 0 && NowMs() >= g_settingsSaveAtMs) {
        g_settingsSaveAtMs = 0;
        SaveSettingsIfDirty();
    }
}

void CModernMenu::AfterUpdate(CMobileMenu* menu) {
    switch (g_state.load()) {
        case eState::MAP:
            if (!IsNativeMenuOpen(menu)) {
                // Closed by GTA itself (death, cutscene, disconnect, ...).
                SetState(eState::NONE);
                CallJavaVoid("hideMap");
            } else if (!menu->isMapMode && !menu->pendingScreen) {
                // Back pressed on the map: GTA returns to its own pause
                // menu. Go back to the modern pause menu instead.
                CloseNativeMenu(menu);
                CallJavaVoid("hideMap");
                SetState(eState::PAUSE);
                JavaShowPause();
            }
            break;

        case eState::GTA_CONTROLS: {
            if (!CAdjustableHUD_m_pInstance) {
                CloseNativeMenu(menu);
                SetState(eState::NONE);
                break;
            }
            const bool editorOpen = *CAdjustableHUD_m_pInstance != nullptr;
            if (!IsNativeMenuOpen(menu)) {
                // Closed by GTA itself (death, disconnect, ...).
                if (editorOpen) CAdjustableHUD_Toggle();
                SetState(eState::NONE);
                break;
            }

            // A screen is ready for the next step when it is on the stack
            // and nothing is pending. Hide it at once: only the editor
            // should be seen.
            const bool ready = !menu->pendingScreen;
            MenuScreen* top = TopScreen(menu);
            if (top && g_controlsStep != ControlsStep::EDITING) top->opacity = 0.0f;

            bool backToPause = false;
            switch (g_controlsStep) {
                case ControlsStep::OPEN_SETTINGS:
                    if (ready && top && menu->screenStack.numEntries == 1) {
                        MainMenuScreen_OnSettings(top);
                        g_controlsStep = ControlsStep::OPEN_CONTROLS;
                        g_controlsWaitFrames = 0;
                    }
                    break;
                case ControlsStep::OPEN_CONTROLS:
                    if (ready && top && menu->screenStack.numEntries == 2) {
                        SettingsScreen_OnAdjustControls(top);
                        g_controlsStep = ControlsStep::OPEN_EDITOR;
                        g_controlsWaitFrames = 0;
                    }
                    break;
                case ControlsStep::OPEN_EDITOR:
                    if (ready && top && menu->screenStack.numEntries == 3) {
                        if (!editorOpen) CAdjustableHUD_Toggle();
                        g_controlsStep = ControlsStep::EDITING;
                        g_controlsWaitFrames = 0;
                    }
                    break;
                case ControlsStep::EDITING:
                    // CAdjustableHUD::Update deletes the editor itself on
                    // save, cancel or back.
                    if (!editorOpen) backToPause = true;
                    break;
            }

            if (g_controlsStep != ControlsStep::EDITING &&
                ++g_controlsWaitFrames > CONTROLS_MAX_WAIT_FRAMES) {
                Log("ModernMenu: GTA controls editor did not open, step %d",
                    static_cast<int>(g_controlsStep));
                backToPause = true;
            }

            if (backToPause) {
                if (*CAdjustableHUD_m_pInstance) CAdjustableHUD_Toggle();
                CloseNativeMenu(menu);
                SetState(eState::PAUSE);
                JavaShowPause();
            }
            break;
        }

        case eState::GTA_SETTINGS:
            if (!IsNativeMenuOpen(menu)) {
                SetState(eState::NONE);
                break;
            }
            if (g_settingsPending) {
                // Wait until the FlowScreen is on the stack, so the settings
                // screen gets it as its previous screen (Back works).
                if (!menu->pendingScreen && menu->screenStack.numEntries == 1 && MainMenuScreen_OnSettings) {
                    if (MenuScreen* top = TopScreen(menu)) {
                        MainMenuScreen_OnSettings(top);
                        g_settingsPending = false;
                    }
                }
            } else if (menu->screenStack.numEntries >= 2) {
                g_settingsSeenOpen = true;
            } else if (g_settingsSeenOpen && !menu->pendingScreen && menu->screenStack.numEntries == 1) {
                // Back from the settings: return to the modern pause menu
                // instead of the old GTA one.
                CloseNativeMenu(menu);
                SetState(eState::PAUSE);
                JavaShowPause();
            }
            break;

        default:
            break;
    }
}

// ============================================================================
//  Hooks
// ============================================================================

static bool g_gameReadySent = false;

static void (*MobileMenu_Update_orig)(CMobileMenu*) = nullptr;
static void MobileMenu_Update_hook(CMobileMenu* thiz) {
    if (!MobileMenu_Update_orig) return;
    if (!thiz) {
        MobileMenu_Update_orig(thiz);
        return;
    }

    // Once per connection: Java applies the saved HUD layout.
    if (IsConnected()) {
        if (!g_gameReadySent && EnsureJavaClass()) {
            g_gameReadySent = true;
            CallJavaVoid("onGameReady");
        }
    } else {
        g_gameReadySent = false;
    }

    CModernMenu::ProcessRequests(thiz);
    MobileMenu_Update_orig(thiz);
    CModernMenu::AfterUpdate(thiz);
}

// FlowScreen is the GTA pause screen. It is drawn over the map; with the
// modern overlay it must not be drawn at all while the map is open.
static void (*FlowScreen_Render_orig)(void*, int32_t) = nullptr;
static void FlowScreen_Render_hook(void* thiz, int32_t arg) {
    if (!FlowScreen_Render_orig) return;
    const auto state = CModernMenu::GetState();
    if (state == CModernMenu::eState::MAP || state == CModernMenu::eState::GTA_CONTROLS) return;
    FlowScreen_Render_orig(thiz, arg);
}

void CModernMenu::InjectHooks() {
    Resolve(MainMenuScreen_OnResume, "_ZN14MainMenuScreen8OnResumeEv");
    Resolve(MainMenuScreen_OnSettings, "_ZN14MainMenuScreen10OnSettingsEv");
    Resolve(SettingsScreen_OnAdjustControls, "_ZN14SettingsScreen16OnAdjustControlsEv");
    Resolve(CAdjustableHUD_Toggle, "_ZN14CAdjustableHUD6ToggleEv");
    Resolve(CAdjustableHUD_m_pInstance, "_ZN14CAdjustableHUD11m_pInstanceE");
    Resolve(MobileMenu_ProcessPending, "_ZN10MobileMenu14ProcessPendingEv");
    Resolve(MobileMenu_InitForPause, "_ZN10MobileMenu12InitForPauseEv");
    Resolve(CRadar_SetMapCentreToPlayerCoords, "_ZN6CRadar26SetMapCentreToPlayerCoordsEv");
    Resolve(CRadar_TransformRadarPointToRealWorldSpace, "_ZN6CRadar35TransformRadarPointToRealWorldSpaceER9CVector2DRKS0_");
    Resolve(PlaceRedMarker, "_Z14PlaceRedMarkerb");
    Resolve(Menu_ApplyAudioSettings, "_Z23Menu_ApplyAudioSettingsv");
    Resolve(Menu_SaveSettings, "_Z17Menu_SaveSettingsv");
    Resolve(CTouchInterface_SetupLayoutObjects, "_ZN15CTouchInterface18SetupLayoutObjectsEv");

    CHook::InlineHook("_ZN10MobileMenu6UpdateEv", &MobileMenu_Update_hook, &MobileMenu_Update_orig);
    CHook::InlineHook("_ZN10FlowScreen6RenderEi", &FlowScreen_Render_hook, &FlowScreen_Render_orig);
}

// ============================================================================
//  JNI (com.holy.game.gui.modern.ModernMenu)
// ============================================================================

extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeRequest(JNIEnv* env, jclass clazz, jint request) {
    CModernMenu::PostRequest(request);
}

extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeRequestArg(JNIEnv* env, jclass clazz, jint request, jint arg) {
    CModernMenu::PostRequest(request, arg);
}

// GTA settings for the modern settings tabs: 4 ints per setting
// (value, min, max, visible), MS_MAX settings. Plain int reads; the values
// only change on the game thread and a stale value is corrected next time.
// Ids from GraphicsSettings::kMenuBase on are the client's graphics options
// (same 4-int layout, always visible); ids in between are unused (visible 0).
extern "C" JNIEXPORT jintArray JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeGetSettings(JNIEnv* env, jclass clazz) {
    static_assert(MS_MAX <= GraphicsSettings::kMenuBase, "menu ids overlap");
    constexpr int kCount = GraphicsSettings::kMenuBase + GraphicsSettings::COUNT;
    jint data[kCount * 4] = {};
    for (int i = 0; i < MS_MAX; ++i) {
        const MobileSettings& setting = CMobileSettings::ms_MobileSettings[i];
        data[i * 4 + 0] = setting.value;
        data[i * 4 + 1] = setting.min;
        data[i * 4 + 2] = setting.max;
        data[i * 4 + 3] = setting.visible ? 1 : 0;
    }
    for (int i = 0; i < GraphicsSettings::COUNT; ++i) {
        const int base = (GraphicsSettings::kMenuBase + i) * 4;
        data[base + 0] = GraphicsSettings::Get(i);
        data[base + 1] = GraphicsSettings::Min(i);
        data[base + 2] = GraphicsSettings::Max(i);
        data[base + 3] = GraphicsSettings::Visible(i) ? 1 : 0;
    }
    jintArray array = env->NewIntArray(kCount * 4);
    if (array) env->SetIntArrayRegion(array, 0, kCount * 4, data);
    return array;
}

// Radar rectangle after the HUD layout editor moved or resized the radar.
// Same fields as HudManager.SetRadarBgPos / nativeSetRadarPos.
extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeSetRadarRect(JNIEnv* env, jclass clazz,
        jfloat x1, jfloat y1, jfloat x2, jfloat y2,
        jfloat gtaX, jfloat gtaY, jfloat gtaWidth, jfloat gtaHeight) {
    CHUD::radarBgPos1.x = x1;
    CHUD::radarBgPos1.y = y1;
    CHUD::radarBgPos2.x = x2;
    CHUD::radarBgPos2.y = y2;
    CHUD::radarPos.x = gtaX;
    CHUD::radarPos.y = gtaY;
    CHUD::radarSize.x = gtaWidth;
    CHUD::radarSize.y = gtaHeight;
}

// UI thread (Activity.onPause, "Keluar"): settings.ini is written at once,
// because the game thread no longer runs while the app is in the background
// and Android may kill the process there. GTA's own gta_sa.set stays on the
// game thread (REQUEST_GFX_SAVE / closing the menu).
extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeSaveSettingsNow(JNIEnv* env, jclass clazz) {
    CSettings::SaveNow();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_holy_game_gui_modern_ModernMenu_nativeIsConnected(JNIEnv* env, jclass clazz) {
    return IsConnected() ? JNI_TRUE : JNI_FALSE;
}
