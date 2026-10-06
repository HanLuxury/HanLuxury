#pragma once

#include <array>

// Post-processing melalui EGL/GL API. Tidak menggunakan alamat/offset GTA.
namespace EglPostFX {
enum class LookPreset { Automatic, Day, GoldenHour, Night, Rain };

// Snapshot dari nilai client yang SUDAH tersedia (CWeather/CClock/CTimeCycle),
// diambil di game thread lalu dibawa ke render thread lewat SubmitBeforeHud.
// Tidak mencari offset, memaksa cuaca, atau mengubah jam server.
struct FrameEnvironment {
    bool valid=false;
    float hour=12.0f, rain=0.0f, wetness=0.0f, cloud=0.0f, fog=0.0f;
    float underwater=0.0f, tunnel=0.0f;
    bool sunVisible=false;
    float sunUV[2]={0.5f,0.8f}; // normalized GL coordinates, bottom-left origin.
    float sunGlare=0.0f;        // CWeather::SunGlare, continuous (0..1)
    float toSun[3]={0.0f,0.0f,1.0f}; // CTimeCycle::GetVectorToSun(), world space
    int oldWeather=-1,newWeather=-1; // eWeatherType, -1 = unknown
    float weatherBlend=0.0f;    // CWeather::InterpolationValue (old -> new)
};
struct ScreenLight {
    float uv[2] = {0.75f, 0.85f}; // UV OpenGL: (0,0) kiri bawah.
    float radius = 0.35f;
    float intensity = 0.04f;
    float color[3] = {1.0f, 0.78f, 0.50f}; // Linear RGB.
    float pulse = 0.08f; // Amplitudo animasi 0..1; 0 = konstan.
};

struct Settings {
    bool enabled = true;
    bool ssr = true;       // Hanya aktif bila capture depth + projection valid.
    bool ssao = true;
    bool sourceIsSRGB = true; // Framebuffer LINEAR sering berisi RGB gamma GTA.
    bool weatherLook = true;  // Grading/fog/langit ikut cuaca GTA secara realtime.
    bool fxaa = true;         // Anti-aliasing FXAA pada gambar dunia (sebelum HUD).
    LookPreset preset = LookPreset::Automatic;
    float exposure = 1.06f;
    float saturation = 1.02f;
    float contrast = 1.06f;
    float toneMix = 0.55f;
    float bloomStrength = 0.32f;
    float bloomThreshold = 0.70f; // Threshold dalam linear RGB (0.55 membuat langit ikut bloom saat kamera menunduk/mendongak).
    float bloomKnee = 0.20f;
    float dirtStrength = 0.08f;
    float ssrStrength = 0.32f; // Wet-weather maximum; dry reflection is reduced.
    float ssrDistance = 25.0f; // Satuan view-space, bukan meter terjamin.
    float ssrThickness = 0.30f;
    float aoStrength = 0.65f;
    float aoRadius = 1.0f;
    float clarity = 0.12f; // Sharpening ringan: nilai tinggi menambah shimmer pada tepi/dedaunan.
    float vibrance = 0.12f;
    float shadowLift = 0.008f;
    float vignette = 0.04f;
    float sunShafts = 0.24f; // Screen-space rays; dengan depth hanya piksel langit yang bersinar.
    float lensFlare = 0.045f;
    float fogStrength = 0.65f; // GLES3 + valid depth/projection only.
    float skyStrength = 0.35f; // Langit realtime: warna & glow matahari dari jam + cuaca.
    float wetStrength = 1.0f;  // Jalan basah/genangan saat hujan (CWeather::WetRoads).
    int lightCount = 0; // No fixed glowing spot pasted over the game by default.
    std::array<ScreenLight, 4> lights{};
};

// Panggil dari JNI_OnLoad sesudah shadowhook_init (main.cpp). Memakai ShadowHook
// milik client, TANPA AML. Tidak membuat resource GL di thread JNI.
bool InstallHooks();

// Thread-safe. Tidak melakukan GL call; nilai diambil pada render thread.
void SetSettings(const Settings& settings);
Settings GetSettings();
void SetFrameEnvironment(const FrameEnvironment& environment);

// Opsional untuk integrasi lain: panggil pada thread render game setelah
// eglMakeCurrent. Hook tidak memproses surface launcher/WebView secara global.
// RenderBeforeHud otomatis mendaftarkan surface game ini.
void BindCurrentGameSurface();

// Integrasi opsional pada batas scene -> HUD yang SUDAH ada di client.
// Seluruh GL call tetap berjalan pada thread pemilik current EGLContext.
// Jika projection=nullptr, kamera & depth dunia diambil dari WorldSunShadow.
// Matriks yang diberikan harus persis proyeksi GL frame ini, column-major.
// Jika context/FBO/projection tidak sesuai, SSR/SSAO dilewati, tidak ditebak.
void RenderBeforeHud(const float* projectionColumnMajor = nullptr);

// Game thread (Render2dStuff), sesudah emu_FlushAltRenderTarget dan sebelum
// HUD: SetFrameEnvironment + RenderBeforeHud dijalankan di render thread GTA,
// berurutan dengan command game. HUD/chat/CEF/dialog tidak ikut diproses.
void SubmitBeforeHud(const FrameEnvironment& environment);
} // namespace EglPostFX
