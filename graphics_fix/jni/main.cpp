#include <jni.h>
#include <android/log.h>
#include <ucontext.h>
#include <pthread.h>
   
#include "main.h"
#include "game/game.h"
#include "game/RW/RenderWare.h"
#include "game/RwHelper.h"
#include "net/netgame.h"
#include "chatwindow.h"
#include "playertags.h"
#include "keyboard.h"
#include "CSettings.h"
#include "java_systems/HUD.h"
#include "CLoader.h"
#include "game/ModelIdLimit.h"
#include "graphics/postfx/EglPostFX.h"
#include "loader/AmlBootstrap.h"
#include "crashlytics.h"
 
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "str_obfuscator.hpp"

uintptr_t g_libGTASA = 0;
uintptr_t g_libSAMP = 0;
char* g_pszStorage = nullptr;
char* g_pszRootStorage = nullptr;

// FIX (lokasi mods): root folder modloader SELALU di TESTLIT, BUKAN di path
// scoped Android/data/<package>/... (g_pszStorage). Diisi di
// InitializeHolyModloader() lalu dipakai ulang oleh RegisterCustomSkin/
// RegisterCustomVehicle/RegisterCustomObject agar konsisten satu sumber.
std::string g_strModloaderRoot;

// VARIABEL GLOBAL UNTUK IP DAN PORT DARI LAUNCHER
char g_szHost[256] = "127.0.0.1";
int g_iPort = 7777;

#include "CLocalisation.h"
#include "java_systems/HUD.h"
#include "java_systems/Inventory.h"
#include "util/CStackTrace.h"

void CrashLog(const char* fmt, ...);
bool g_bIsTestMode = false;
CNetGame *pNetGame = nullptr;
 
CGUI *pGUI = nullptr;

void InstallSpecialHooks();
void InitRenderWareFunctions();
void ApplyInGamePatches();
void ApplyPatches_level0();

extern uint16_t g_usLastProcessedModelIndexAutomobile;
extern int g_iLastProcessedModelIndexAutoEnt;

extern int g_iLastProcessedSkinCollision;
extern int g_iLastProcessedEntityCollision;

extern char g_bufRenderQueueCommand[200];
extern uintptr_t g_dwRenderQueueOffset;
extern char lastFile[123];
extern int g_iLastRenderedObject;
extern int lastNvEvent;
extern CVector lastPos;
char g_iLastBlock[123];
char streamimgState[123];

void PrintBuildCrashInfo()
{
	std::time_t currentTime = std::time(nullptr);
	std::tm* timeInfo = std::localtime(&currentTime);

	CrashLog("Crash time: %d:%d:%d %d:%d:%d", timeInfo->tm_mday, timeInfo->tm_mon, timeInfo->tm_year, timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
	CrashLog("Build times: %s %s. Type: %s", __TIME__, __DATE__, (VER_x32 ? "x32" : "x64"));
	CrashLog("Last processed auto and entity: %d %d", g_usLastProcessedModelIndexAutomobile, g_iLastProcessedModelIndexAutoEnt);
	CrashLog("Last processed skin and entity: %d %d", g_iLastProcessedSkinCollision, g_iLastProcessedEntityCollision);
	CrashLog("Last rendered object: %d", g_iLastRenderedObject);
	CrashLog("Last texture: %s", g_iLastBlock);
	CrashLog("Last file: %s", lastFile);
	CrashLog("Last pos: %.2f, %.2f, %.2f", lastPos.x, lastPos.y, lastPos.z);
	CrashLog("Last nvEvent: %d", lastNvEvent);
    CrashLog("%s", streamimgState);
}

#include <sstream>
#include "vendor/bass/bass.h"
#include "util/CJavaWrapper.h"
#include "CDebugInfo.h"
#include "voice/Plugin.h"

void InitInMenu()
{
    CGame::Init();
    CGame::InitInMenu();

	pGUI = new CGUI();
	CKeyBoard::init();
	CPlayerTags::Init();
}

#include <unistd.h> // system api
#include <sys/mman.h>
#include <cassert> // assert()
#include <dlfcn.h> // dlopen
#include <signal.h> // signal handling
#include <fcntl.h>   // open() for crash-safe logging
#include "voice/Playback.h"

void InitInGame()
{
	static bool bGameInited = false;
	static bool bNetworkInited = false;
	if (!bGameInited)
	{
		CGame::InitInGame();
		CGame::SetMaxStats();

        Voice::Playback::Init(); // bass init

		g_pJavaWrapper->hideLoadingScreen();

		CHUD::toggleServerLogo(true);
		bGameInited = true;

		return;
	}

	if (!bNetworkInited)
	{
        // IP dan Port sekarang memanggil variabel dinamis yang dikirim dari Launcher
        pNetGame = new CNetGame(
                g_szHost,
                g_iPort,
                CSettings::Get().szNickName,
                CSettings::Get().szPassword
        );

		bNetworkInited = true;
		Log("InitInGame() end - Connecting to %s:%d", g_szHost, g_iPort);
		return;
	}
}

#include "CDebugInfo.h"
#include "java_systems/SnapShotsWrapper.h"

void MainLoop()
{
	if(CGame::bIsGameExiting)return;

	InitInGame();

	if(pNetGame) pNetGame->Process();
}

struct sigaction act_old{};
struct sigaction act1_old{};
struct sigaction act2_old{};
struct sigaction act3_old{};

static volatile sig_atomic_t g_inCrashHandler = 0;

static size_t appendHex(char* out, size_t pos, size_t cap, uintptr_t value) {
    static const char hex[] = "0123456789abcdef";
    if(pos >= cap) return pos;
    char tmp[2 * sizeof(uintptr_t)];
    size_t n = 0;
    do {
        tmp[n++] = hex[value & 0xF];
        value >>= 4;
    } while(value && n < sizeof(tmp));
    while(n && pos < cap) out[pos++] = tmp[--n];
    return pos;
}

static size_t appendStr(char* out, size_t pos, size_t cap, const char* str) {
    while(str && *str && pos < cap) out[pos++] = *str++;
    return pos;
}

static int g_crashFd = -1;

static void emitCrashLine(const char* line, size_t len) {
    if(g_crashFd >= 0) (void)write(g_crashFd, line, len);
    char copy[256];
    const size_t n = len < sizeof(copy) - 1 ? len : sizeof(copy) - 1;
    for(size_t i = 0; i < n; ++i) copy[i] = line[i];
    copy[n] = 0;
    if(n && copy[n - 1] == '\n') copy[n - 1] = 0;
    __android_log_write(ANDROID_LOG_FATAL, "HolySAMP", copy);
}

// "<tag> 0x<abs> libname.so+0x<offset>". dladdr is not formally
// async-signal-safe, but it only reads the loader's module list; it is what
// most in-process crash reporters use to turn a PC into module+offset, and
// that offset is what is needed to find the crashing function.
static void emitAddress(const char* tag, uintptr_t addr) {
    char buf[256];
    size_t p = 0;
    p = appendStr(buf, p, sizeof(buf), tag);
    p = appendStr(buf, p, sizeof(buf), " 0x");
    p = appendHex(buf, p, sizeof(buf), addr);

    Dl_info info{};
    if(addr && dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_fname) {
        const char* name = info.dli_fname;
        for(const char* c = info.dli_fname; *c; ++c) if(*c == '/') name = c + 1;
        p = appendStr(buf, p, sizeof(buf), " ");
        p = appendStr(buf, p, sizeof(buf), name);
        p = appendStr(buf, p, sizeof(buf), "+0x");
        p = appendHex(buf, p, sizeof(buf), addr - reinterpret_cast<uintptr_t>(info.dli_fbase));
        if(info.dli_sname) {
            p = appendStr(buf, p, sizeof(buf), " (");
            p = appendStr(buf, p, sizeof(buf), info.dli_sname);
            p = appendStr(buf, p, sizeof(buf), ")");
        }
    }
    p = appendStr(buf, p, sizeof(buf), "\n");
    emitCrashLine(buf, p);
}

static void writeCrashReport(int signum, siginfo_t* info, void* contextPtr) {
    // The old report was only "signal + fault address", which does not say
    // WHERE the game crashed. This adds pc, lr and a frame-pointer backtrace,
    // each as module+offset (libGTASA.so / libsamp.so), in crash_native.txt
    // and in logcat (tag HolySAMP).
    g_crashFd = open("/sdcard/TESTLIT/crash_native.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);

    char buf[192];
    size_t p = 0;
    p = appendStr(buf, p, sizeof(buf), "HolySAMP fatal signal=");
    p = appendHex(buf, p, sizeof(buf), (uintptr_t)signum);
    p = appendStr(buf, p, sizeof(buf), " fault=0x");
    p = appendHex(buf, p, sizeof(buf), info ? (uintptr_t)info->si_addr : 0);
    p = appendStr(buf, p, sizeof(buf), "\n");
    emitCrashLine(buf, p);

#if defined(__aarch64__)
    if(contextPtr) {
        const auto* uc = static_cast<const ucontext_t*>(contextPtr);
        const auto& mc = uc->uc_mcontext;
        emitAddress("pc", mc.pc);
        emitAddress("lr", mc.regs[30]);

        // Walk the frame records (x29 chain). libGTASA and this client keep
        // frame pointers; stop at anything that does not look like a stack.
        uintptr_t fp = mc.regs[29];
        const uintptr_t sp = mc.sp;
        for(int i = 0; i < 24; ++i) {
            if(fp < sp || (fp & 0xF) || fp - sp > 8u * 1024u * 1024u) break;
            const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
            const uintptr_t next = frame[0];
            const uintptr_t ret = frame[1];
            if(!ret) break;
            char tag[8] = { '#', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0 };
            emitAddress(tag, ret);
            if(next <= fp) break;
            fp = next;
        }
    }
#else
    (void)contextPtr;
#endif

    if(g_crashFd >= 0) {
        close(g_crashFd);
        g_crashFd = -1;
    }
}

static struct sigaction* OldActionFor(int signum) {
    switch(signum) {
        case SIGSEGV: return &act_old;
        case SIGABRT: return &act1_old;
        case SIGFPE:  return &act2_old;
        case SIGBUS:  return &act3_old;
        default:      return nullptr;
    }
}

static void fatalSignalHandler(int signum, siginfo_t* info, void* contextPtr) {
    if(g_inCrashHandler) {
        signal(signum, SIG_DFL);
        kill(getpid(), signum);
        return;
    }
    g_inCrashHandler = 1;
    writeCrashReport(signum, info, contextPtr);

    // Put the previous handler (ART / debuggerd) back and return: the faulting
    // instruction runs again and the system writes its normal tombstone with a
    // full symbolised backtrace to logcat (tag DEBUG). The old code killed the
    // process with SIG_DFL, so no backtrace was ever produced.
    if(struct sigaction* old = OldActionFor(signum)) {
        sigaction(signum, old, nullptr);
    } else {
        signal(signum, SIG_DFL);
    }
    if(signum != SIGSEGV && signum != SIGBUS && signum != SIGFPE) {
        // Not re-raised by returning (abort() re-raises by itself, but a
        // plain kill() would not).
        raise(signum);
    }
}

static void InstallCrashHandlers() {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = fatalSignalHandler;
    act.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &act, &act_old);
    sigaction(SIGABRT, &act, &act1_old);
    sigaction(SIGFPE,  &act, &act2_old);
    sigaction(SIGBUS,  &act, &act3_old);
}

extern "C"
{
	JavaVM* javaVM = nullptr;
	JavaVM* alcGetJavaVM(void) {
		return javaVM;
	}
}

// =========================================================================
// ==================== HOLY MODLOADER SYSTEM START ========================
// =========================================================================
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

// MENGGUNAKAN SHADOWHOOK SEBAGAI PENGGANTI DOBBY
#include "shadowhook.h"

#define STB_IMAGE_IMPLEMENTATION
#include "vendor/imgui/stb_image.h"

#include "modloader/ModLoader.h"
#include "modloader/HookScope.h" // ML_HOOK_SCOPE(); SHADOWHOOK_STACK_SCOPE() tidak boleh dipakai lagi
#include "vicecity/ViceCity.h"   // map Vice City: tekstur model-modelnya ikut lewat hook RwTextureRead

// Note: struct RwTexture dan RwRaster TIDAK diredifinisikan di sini 
// karena sudah ada di game/RW/RenderWare.h

// Konstanta raster dipakai dari game/RW/raster.h. Dulu di sini ada tiga #define, dan salah satunya
// (rwRASTERFORMAT8888 0x0100) sebenarnya nilai rwRASTERFORMAT1555: raster PNG jadi 16-bit, buffer lock-nya
// hanya width*height*2 byte, lalu diisi width*height*4 byte.
static_assert(rwRASTERTYPETEXTURE == 0x04 && rwRASTERFORMAT8888 == 0x0500 && rwRASTERLOCKWRITE == 0x01,
              "konstanta raster harus yang dari game/RW/raster.h");

// Pointer Fungsi Asli untuk Hooking
RwTexture* (*orig_RwTextureRead)(const char* name, const char* maskName);

// Fungsi asli game (tidak di-hook): dipakai Custom DL Loader untuk membuka file model
void* (*orig_RwStreamOpen)(int type, int accessType, const void* custom);

// Pointer Internal untuk memanggil fungsi Engine (Diberi prefix 'p' agar tidak bentrok)
RwTexture* (*pRwTextureCreate)(RwRaster* raster);
RwRaster* (*pRwRasterCreate)(int width, int height, int depth, int flags);
uint8_t* (*pRwRasterLock)(RwRaster* raster, uint8_t level, int lockMode);
RwRaster* (*pRwRasterUnlock)(RwRaster* raster); // Return type harus RwRaster* sesuai header

// Pointer untuk RenderWare Stream Murni (Digunakan di Custom DL Loader)
bool (*pRwStreamFindChunk)(void* stream, uint32_t type, uint32_t* length, uint32_t* version);
void* (*pRpClumpStreamRead)(void* stream);

// Tekstur 32-bit dari piksel RGBA (hasil stbi_load dengan 4 kanal). nullptr kalau gagal; raster yang sudah
// dibuat tidak pernah tertinggal.
static RwTexture* CreateTextureFromRgba(const char* name, const uint8_t* rgba, int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    // RwRasterDestroy: pointer milik klien (game/RW/RenderWare.cpp). Tanpa itu raster yang gagal tidak
    // bisa dibuang, jadi tidak dibuat sama sekali.
    if (!pRwRasterCreate || !pRwRasterLock || !pRwRasterUnlock || !pRwTextureCreate || !RwRasterDestroy) return nullptr;

    RwRaster* raster = pRwRasterCreate(width, height, 32, (int)rwRASTERTYPETEXTURE | (int)rwRASTERFORMAT8888);
    if (raster == nullptr) return nullptr;

    // Game menentukan sendiri kedalaman dan lebar baris raster. Piksel hanya disalin kalau raster itu
    // benar-benar 32-bit dan barisnya muat, baris demi baris menurut stride milik raster.
    const size_t rowBytes = (size_t)width * 4u;
    bool copied = false;
    if (raster->depth == 32 && raster->width == width && raster->height == height) {
        uint8_t* pixels = pRwRasterLock(raster, 0, rwRASTERLOCKWRITE);
        if (pixels != nullptr) {
            if (raster->stride > 0 && (size_t)raster->stride >= rowBytes) {
                for (int y = 0; y < height; y++) {
                    memcpy(pixels + (size_t)y * (size_t)raster->stride, rgba + (size_t)y * rowBytes, rowBytes);
                }
                copied = true;
            }
            pRwRasterUnlock(raster);
        }
    }

    RwTexture* texture = copied ? pRwTextureCreate(raster) : nullptr;
    if (texture == nullptr) {
        RwRasterDestroy(raster);
        return nullptr;
    }
    strncpy(texture->name, name, 31);
    texture->name[31] = '\0';
    return texture;
}

// Hook Tekstur. Urutan: <nama>.png dari folder mod, lalu TXD lepas (format PC) untuk TXD yang
// sedang dibaca game, baru fungsi asli (texdb). Penggantian file biasa dan isi IMG tidak lagi
// lewat hook di sini: lihat NvFOpen (game/hooks.cpp), CStreaming::InitImageList dan modloader/.
RwTexture* hook_RwTextureRead(const char* name, const char* maskName) {
    // BUKAN SHADOWHOOK_STACK_SCOPE(): di mode UNIQUE pustaka libshadowhook.so 1.0.9 proyek ini memanggil
    // abort() dari shadowhook_pop_stack(). ML_HOOK_SCOPE() hanya melakukannya di mode SHARED.
    ML_HOOK_SCOPE();
    if(!orig_RwTextureRead) return nullptr;
    if (name == nullptr) return orig_RwTextureRead(name, maskName);

    if (const char* pngPath = ml::FindPng(name)) {
        int width = 0, height = 0, channels = 0;
        uint8_t* imgData = stbi_load(pngPath, &width, &height, &channels, 4);

        if (imgData != nullptr) {
            RwTexture* fromPng = CreateTextureFromRgba(name, imgData, width, height);
            stbi_image_free(imgData);
            if (fromPng != nullptr) {
                __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "Texture Load PNG (Replaced): %s -> %s", name, pngPath);
                return fromPng;
            }
            __android_log_print(ANDROID_LOG_WARN, "HolyModloader", "PNG tidak bisa dijadikan tekstur: %s", pngPath);
        }
    }

    if (RwTexture* fromTxd = ml::FindTexture(name)) return fromTxd;

    // Map Vice City: tekstur model yang sedang dibaca game, dari file .txd (format PC) di TESTLIT/vice_city.
    // Tekstur yang tidak ada di sana tetap dicari game di texdb-nya sendiri.
    if (RwTexture* fromViceCity = vc::FindTexture(name)) return fromViceCity;

    return orig_RwTextureRead(name, maskName);
}

void InitializeHolyModloader() {
    __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "Memulai Modloader 64-Bit System via ShadowHook...");

    // Folder mod SELALU di TESTLIT (bukan Android/data/<package>/...). Pemindaian, modloader.ini,
    // prioritas dan laporan modloader_status.txt ada di modloader/; hasilnya dipakai juga oleh
    // NvFOpen dan CStreaming, jadi di sini cukup memastikan sudah dipindai.
    ml::Initialize();
    if (ml::Root()[0] == '\0') {
        __android_log_print(ANDROID_LOG_ERROR, "HolyModloader", "GAGAL MEMBUAT FOLDER MODLOADER");
    } else {
        __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "Folder modloader siap di: %s", ml::Root());
    }

    if(g_libGTASA == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "HolyModloader", "g_libGTASA masih 0. Hooking dibatalkan!");
        ml::SetTextureHookInstalled(false);
        vc::SetTextureHookInstalled(false);
        return;
    }

    // Offset GTA SA 2.10 64-Bit (arm64-v8a)
    uintptr_t addr_RwStream       = g_libGTASA + 0x280e44;
    uintptr_t addr_RwTextureRead  = g_libGTASA + 0x2742c8;
    uintptr_t addr_RwTextureCreate= g_libGTASA + 0x273f74;
    uintptr_t addr_RwRasterCreate = g_libGTASA + 0x272e34;
    uintptr_t addr_RwRasterLock   = g_libGTASA + 0x272f18;
    uintptr_t addr_RwRasterUnlock = g_libGTASA + 0x272954;

    // Offset RenderWare Stream Murni (Untuk Custom DL): alamat RwStreamFindChunk dan RpClumpStreamRead
    // yang diekspor libGTASA.so 2.10 arm64. Nilai lama (0x272444, 0x28639c) jatuh di tengah
    // RwImageGammaCorrect dan TextureDatabaseRuntime::SetAsRendered.
    uintptr_t addr_RwStreamFindChunk = g_libGTASA + 0x27c9f4;
    uintptr_t addr_RpClumpStreamRead = g_libGTASA + 0x2bba10;

    // Assign ke pointer internal (pRw...) dengan cast yang benar
    pRwTextureCreate = (RwTexture* (*)(RwRaster*))addr_RwTextureCreate;
    pRwRasterCreate  = (RwRaster* (*)(int, int, int, int))addr_RwRasterCreate;
    pRwRasterLock    = (uint8_t* (*)(RwRaster*, uint8_t, int))addr_RwRasterLock;
    pRwRasterUnlock  = (RwRaster* (*)(RwRaster*))addr_RwRasterUnlock;

    pRwStreamFindChunk = (bool (*)(void*, uint32_t, uint32_t*, uint32_t*))addr_RwStreamFindChunk;
    pRpClumpStreamRead = (void* (*)(void*))addr_RpClumpStreamRead;
    orig_RwStreamOpen  = (void* (*)(int, int, const void*))addr_RwStream;

    // Custom DL tidak memakai hook apa pun, jadi tidak bergantung pada ShadowHook: begitu semua pointer
    // di atas terisi, root-nya diumumkan. Tanpa folder mod root ini kosong dan Custom DL tetap mati.
    g_strModloaderRoot = ml::Root(); // dipakai ulang oleh RegisterCustomSkin/Vehicle/Object

    // Inisialisasi ShadowHook dengan mode SHARED. Kalau ShadowHook sudah di-init lebih dulu (dari Java,
    // sebelum JNI_OnLoad memasang hook klien), panggilan ini tidak mengubah mode yang sudah aktif.
    const int shInit = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (shInit != 0) {
        // The bundled ShadowHook library exports shadowhook_get_errno(),
        // not shadowhook_get_init_errno(). Keep the error path compatible
        // with the actual prebuilt libshadowhook.so shipped by this project.
        const int shErr = shadowhook_get_errno();
        __android_log_print(ANDROID_LOG_ERROR, "HolyModloader",
            "ShadowHook init failed: init=%d errno=%d / %s",
            shInit, shErr, shadowhook_to_errmsg(shErr));
        ml::SetTextureHookInstalled(false);
        vc::SetTextureHookInstalled(false);
        return;
    }
    // Mode yang benar-benar aktif; proxy di modloader menyesuaikan diri (modloader/HookScope.h).
    __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "ShadowHook mode aktif: %s",
        shadowhook_get_mode() == SHADOWHOOK_MODE_SHARED ? "SHARED" : "UNIQUE");

    // RwTextureRead hanya di-hook kalau folder mod memang berisi PNG atau TXD lepas yang aktif, atau kalau
    // map Vice City terpasang (tekstur modelnya dan minimap.txd dibaca lewat hook yang sama).
    const bool viceCityWantsHook = vc::WantsTextureHook();
    if (!ml::WantsTextureHook() && !viceCityWantsHook) {
        __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "Tidak ada PNG/TXD lepas yang aktif: RwTextureRead tidak di-hook.");
        vc::SetTextureHookInstalled(false);
        return;
    }

    // Satu-satunya hook yang tersisa di sini. Kalau gagal, PNG dan TXD lepas mati;
    // penggantian file biasa dan isi IMG tetap jalan karena tidak bergantung pada hook ini.
    void* stub = shadowhook_hook_func_addr((void*)addr_RwTextureRead, (void*)hook_RwTextureRead, (void**)&orig_RwTextureRead);
    if(stub && orig_RwTextureRead) {
        __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "Hook RwTextureRead berhasil dipasang.");
    } else {
        __android_log_print(ANDROID_LOG_ERROR, "HolyModloader", "Hook RwTextureRead gagal dipasang.");
        if(stub) shadowhook_unhook(stub);
        orig_RwTextureRead = nullptr;
    }
    ml::SetTextureHookInstalled(orig_RwTextureRead != nullptr);
    vc::SetTextureHookInstalled(orig_RwTextureRead != nullptr);
}
// =========================================================================
// ===================== HOLY MODLOADER SYSTEM END =========================
// =========================================================================

#include "util/patch.h"
#include "CLoader.h"

extern "C"
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	javaVM = vm;

	g_libGTASA = CUtil::FindLib("libGTASA.so");

	if(g_libGTASA == 0)
	{
		Log("ERROR: libGTASA.so address not found!");
		return 0;
	}
	Log("libGTASA.so image base address: 0x%X", g_libGTASA);

	g_libSAMP = CUtil::FindLib("libmultiplayer.so");
	if(g_libSAMP == 0)
	{
		Log("ERROR: libsamp.so address not found!");
		return 0;
	}
	Log("libsamp.so image base address: 0x%X", g_libSAMP);

    // anti-cheat by plaka penka xD
    CRYPTEDSTRING(libstr, "libgvraudio.so");
	CRYPTEDSTRING(testtest1111, "libradio.so");
    if(dlopen(libstr.decode().c_str(), RTLD_LAZY) || dlopen(testtest1111.decode().c_str(), RTLD_LAZY))
    {
        exit(0);
    }

	CLoader::initJavaClasses(vm);

	CHook::InitHookStuff();

	// Before any hook: shadowhook copies the first instructions of hooked
	// functions, and the game must never run with the old ID layout.
	ModelIdLimit::Apply();

	InstallSpecialHooks();
	InitRenderWareFunctions();

	ApplyPatches_level0();

	// Original AML core (optional, only for AML mods): invoke its JNI entry
	// once, after game hooks are ready. Graphics do NOT depend on it.
	AmlBootstrap::Initialize(vm, reserved);

	InstallCrashHandlers();

	// Pasang hook API EGL/GL dari JNI_OnLoad dengan ShadowHook milik client
	// (sudah di-init oleh GTASA.java sebelum libmultiplayer dimuat), TANPA AML.
	// FBO/shader dibuat nanti pada render thread GTA. Tidak mengubah offset RenderWare.
	if (!EglPostFX::InstallHooks()) Log("Graphics hooks not installed (see EglPostFX log)");

	return JNI_VERSION_1_6;
}

void Log(const char *fmt, ...)
{
	static char buffer[512] {};

	memset(buffer, 0, sizeof(buffer));

	va_list arg;
	va_start(arg, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, arg);
	va_end(arg);

	firebase::crashlytics::Log(buffer);
	__android_log_write(ANDROID_LOG_INFO, "AXL", buffer);

#if USE_FILE_LOG
	//if(pDebug) pDebug->AddMessage(buffer);
	static FILE* flLog = nullptr;

	if(flLog == nullptr && g_pszStorage != nullptr)
	{
		sprintf(buffer, "%slog.txt", g_pszStorage);
		flLog = fopen(buffer, "ab");
	}

	if(flLog == nullptr) return;
	fprintf(flLog, "%s\n", buffer);
	fflush(flLog);
#endif
}

void CrashLog(const char* fmt, ...)
{
	static char buffer[512] {};
	memset(buffer, 0, sizeof(buffer));

	va_list arg;
	va_start(arg, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, arg);
	va_end(arg);

#ifdef NDEBUG
	firebase::crashlytics::Log(buffer);
#endif
	__android_log_write(ANDROID_LOG_FATAL, "AXL", buffer);

#if USE_FILE_LOG
	static FILE* flLog = nullptr;

	if (flLog == nullptr && g_pszStorage != nullptr)
	{
		sprintf(buffer, "%scrash_log.txt", g_pszStorage);
		flLog = fopen(buffer, "ab");
	}

	if (flLog == nullptr) return;
	fprintf(flLog, "%s\n", buffer);
	fflush(flLog);
#endif
}

uint32_t GetTickCount()
{
    return CTimer::m_snTimeInMillisecondsNonClipped;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_initSAMP(JNIEnv *env, jobject thiz, jfloat maxFps, jstring directory) {
    // Retry only when JNI_OnLoad ran before Application existed. The bootstrap
    // itself never invokes an already-entered AML JNI entry a second time.
    AmlBootstrap::Initialize(javaVM, nullptr);
    EglPostFX::InstallHooks(); // no-op after the first success; no AML needed
    Log("Initializing SAMP..");

    const char *dirChars = env->GetStringUTFChars(directory, nullptr);
    if (g_pszRootStorage != nullptr) {
        delete[] g_pszRootStorage;
    }
    g_pszRootStorage = new char[strlen(dirChars) + 1];
    strcpy(g_pszRootStorage, dirChars);

    // KARENA g_pszStorage DI-SET DI SINI, KITA INISIALISASI MODLOADER DI SINI AGAR AMAN
    // Isi dulu, baru umumkan: thread lain (NvFOpen, Log) membaca g_pszStorage tanpa kunci.
    char* storagePath = new char[strlen(dirChars) + 20];
    sprintf(storagePath, "%sSAMP/", dirChars);
    g_pszStorage = storagePath;
    
    // --> Panggil Modloader SEKARANG setelah g_pszStorage punya nilai
    InitializeHolyModloader();

    env->ReleaseStringUTFChars(directory, dirChars);

	CSettings::maxFps = (int)maxFps;
    g_pJavaWrapper = new CJavaWrapper(env, thiz);
}

// =========================================================================
// BRIDGE BARU UNTUK MENERIMA IP DAN PORT DARI LAUNCHER (KOTLIN)
// =========================================================================
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_initServer(JNIEnv *env, jobject thiz, jstring ip, jint port) {
    if(!env || !ip) return;
    const char *ipChars = env->GetStringUTFChars(ip, nullptr);
    if(!ipChars) return;
    
    // Copy IP ke variabel global C++
    strncpy(g_szHost, ipChars, sizeof(g_szHost) - 1);
    g_szHost[sizeof(g_szHost) - 1] = '\0'; // Pastikan aman (null terminated)
    
    // Set Port
    g_iPort = port;
    
    env->ReleaseStringUTFChars(ip, ipChars);
    Log("Server Address Updated via Launcher: %s:%d", g_szHost, g_iPort);
}

extern bool ProcessLocalCommands(const char str[]);

extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_00024Companion_sendCommand(JNIEnv *env, jobject clazz, jstring command) {
    if(!env || !command) return;
    const char *_command = env->GetStringUTFChars(command, nullptr);
    if(!_command) return;

    if(!ProcessLocalCommands(_command) && pNetGame)
        pNetGame->SendChatCommand(_command);

    env->ReleaseStringUTFChars(command, _command);
}

// =========================================================================
// CUSTOM DL LOADER (SKIN, VEHICLE, OBJECT) - BYPASS TXD / USE PNG
// =========================================================================
#include "game/Models/ModelInfo.h"

void RegisterCustomSkin(int newSkinId, const char* skinName, int parentSkinId) {
    // Sama seperti RegisterCustomObject: nama dan ID datang dari Java/server, jadi diperiksa dulu.
    // CModelInfo::SetModelInfo menulis ke array tanpa cek batas, dan slot yang sudah terisi tidak ditimpa.
    if (!skinName || !*skinName) return;
    if (newSkinId < 0 || newSkinId >= CModelInfo::NUM_MODEL_INFOS) return;
    if (CModelInfo::GetModelInfo(newSkinId) != nullptr) return;

    char dffPath[256];
    // FIX (lokasi mods): pakai root modloader TESTLIT (g_strModloaderRoot),
    // bukan g_pszStorage, supaya konsisten dengan InitializeHolyModloader().
    if (!g_strModloaderRoot.empty()) {
        snprintf(dffPath, sizeof(dffPath), "%s%s.dff", g_strModloaderRoot.c_str(), skinName);
    } else return;

    auto* customPed = (CPedModelInfo*)CModelInfo::AddPedModel(newSkinId);
    if(customPed != nullptr) {
        // Induk hanya dipakai kalau ID-nya sah dan memang model ped: datanya dibaca sebagai CPedModelInfo.
        CPedModelInfo* parentPed = nullptr;
        if (parentSkinId >= 0 && parentSkinId < CModelInfo::NUM_MODEL_INFOS) {
            CBaseModelInfo* parentInfo = CModelInfo::GetModelInfo(parentSkinId);
            if (parentInfo && parentInfo->GetModelType() == MODEL_INFO_PED) parentPed = (CPedModelInfo*)parentInfo;
        }
        if (parentPed != nullptr) {
            customPed->m_nAnimType = parentPed->m_nAnimType;
            customPed->m_nPedType  = parentPed->m_nPedType;
            customPed->m_nStatType = parentPed->m_nStatType;
        }

        customPed->SetTexDictionary(skinName, "models");

        // TXD lepas "<skinName>.txd" di folder mod dipakai untuk tekstur model ini (PNG tetap menang).
        ml::ScopedTxdContext txdContext(skinName);
        void* stream = orig_RwStreamOpen(2, 1, dffPath); 
        if (stream) {
            if (pRwStreamFindChunk(stream, 16, nullptr, nullptr)) { 
                void* clump = pRpClumpStreamRead(stream);
                if (clump) {
                    customPed->m_pRwObject = (RwObject*)clump;
                    __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "SUKSES: Skin ID %d (%s) Dirakit dg PNG!", newSkinId, skinName);
                }
            }
            RwStreamClose((RwStream*)stream, nullptr);
        }
    }
}

void RegisterCustomVehicle(int newVehId, const char* modelName, int parentVehId) {
    // Pemeriksaan yang sama dengan RegisterCustomSkin dan RegisterCustomObject.
    if (!modelName || !*modelName) return;
    if (newVehId < 0 || newVehId >= CModelInfo::NUM_MODEL_INFOS) return;
    if (CModelInfo::GetModelInfo(newVehId) != nullptr) return;

    char dffPath[256];
    // FIX (lokasi mods): pakai root modloader TESTLIT (g_strModloaderRoot),
    // bukan g_pszStorage, supaya konsisten dengan InitializeHolyModloader().
    if (!g_strModloaderRoot.empty()) {
        snprintf(dffPath, sizeof(dffPath), "%s%s.dff", g_strModloaderRoot.c_str(), modelName);
    } else return;

    auto* customVeh = (CVehicleModelInfo*)CModelInfo::AddVehicleModel(newVehId);
    if (customVeh != nullptr) {
        // Induk hanya dipakai kalau ID-nya sah dan memang model kendaraan.
        CVehicleModelInfo* parentVeh = nullptr;
        if (parentVehId >= 0 && parentVehId < CModelInfo::NUM_MODEL_INFOS) {
            CBaseModelInfo* parentInfo = CModelInfo::GetModelInfo(parentVehId);
            if (parentInfo && parentInfo->GetModelType() == MODEL_INFO_VEHICLE) parentVeh = (CVehicleModelInfo*)parentInfo;
        }
        if (parentVeh != nullptr) {
            customVeh->m_nHandlingId = parentVeh->m_nHandlingId;
            customVeh->m_nVehicleClass = parentVeh->m_nVehicleClass;
            customVeh->m_fWheelSizeFront = parentVeh->m_fWheelSizeFront;
            customVeh->m_fWheelSizeRear = parentVeh->m_fWheelSizeRear;
            customVeh->m_nVehicleType = parentVeh->m_nVehicleType;
            customVeh->m_nNumDoors = parentVeh->m_nNumDoors;
        }

        customVeh->SetTexDictionary(modelName, "models");

        // TXD lepas "<modelName>.txd" di folder mod dipakai untuk tekstur model ini (PNG tetap menang).
        ml::ScopedTxdContext txdContext(modelName);
        void* stream = orig_RwStreamOpen(2, 1, dffPath);
        if (stream) {
            if (pRwStreamFindChunk(stream, 16, nullptr, nullptr)) {
                void* clump = pRpClumpStreamRead(stream);
                if (clump) {
                    customVeh->m_pRwObject = (RwObject*)clump;
                    __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "SUKSES: Mobil ID %d (%s) Dirakit dg PNG!", newVehId, modelName);
                }
            }
            RwStreamClose((RwStream*)stream, nullptr);
        }
    }
}

void RegisterCustomObject(int newObjId, const char* modelName, int parentObjId) {
    if (!modelName || !*modelName) return;
    if (newObjId < 0 || newObjId >= CModelInfo::NUM_MODEL_INFOS) return;
    if (CModelInfo::GetModelInfo(newObjId) != nullptr) return;

    char dffPath[256];
    if (!g_strModloaderRoot.empty()) {
        snprintf(dffPath, sizeof(dffPath), "%s%s.dff", g_strModloaderRoot.c_str(), modelName);
    } else return;

    // CAtomicModelInfo expects an RpAtomic prototype, not an RpClump pointer.
    // Keeping the clump root alive is important because the atomic belongs to
    // that clump and its frame/geometry links remain valid for GTA's clone path.
    static std::unordered_map<int, RpClump*> s_customObjectRoots;

    auto* customObj = CModelInfo::AddAtomicModel(newObjId);
    if (!customObj) return;

    CBaseModelInfo* parentInfo = nullptr;
    if (parentObjId >= 0 && parentObjId < CModelInfo::NUM_MODEL_INFOS) {
        parentInfo = CModelInfo::GetModelInfo(parentObjId);
    }
    if (parentInfo) {
        customObj->m_fDrawDistance = parentInfo->m_fDrawDistance;
        customObj->m_nFlags = parentInfo->m_nFlags;
        customObj->m_pColModel = parentInfo->m_pColModel;
        customObj->bDoWeOwnTheColModel = false;
    }

    customObj->SetTexDictionary(modelName, "models");
    customObj->SetModelName(modelName);

    // TXD lepas "<modelName>.txd" di folder mod dipakai untuk tekstur model ini (PNG tetap menang).
    ml::ScopedTxdContext txdContext(modelName);
    void* stream = orig_RwStreamOpen(2, 1, dffPath);
    if (!stream) return;

    RpClump* clump = nullptr;
    if (pRwStreamFindChunk(stream, 16, nullptr, nullptr)) {
        clump = reinterpret_cast<RpClump*>(pRpClumpStreamRead(stream));
    }
    RwStreamClose((RwStream*)stream, nullptr);

    if (!clump) return;

    RpAtomic* atomic = GetFirstAtomic(clump);
    if (!atomic) {
        RpClumpDestroy(clump);
        return;
    }

    s_customObjectRoots[newObjId] = clump;
    customObj->m_pRwAtomic = atomic;

    __android_log_print(ANDROID_LOG_INFO, "HolyModloader",
        "SUKSES: Objek ID %d (%s) -> atomic=%p clump=%p",
        newObjId, modelName, (void*)atomic, (void*)clump);
}

// =========================================================================
// JNI BRIDGE (PENGHUBUNG JAVA KE C++)
// =========================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_registerCustomSkin(JNIEnv *env, jobject thiz, jint skinId, jstring skinName, jint parentId) {
    const char *c_skinName = env->GetStringUTFChars(skinName, nullptr);
    std::string strSkinName(c_skinName);
    env->ReleaseStringUTFChars(skinName, c_skinName);
    CGame::PostToMainThread([skinId, strSkinName, parentId]() { RegisterCustomSkin(skinId, strSkinName.c_str(), parentId); });
}

extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_registerCustomVehicle(JNIEnv *env, jobject thiz, jint vehId, jstring modelName, jint parentId) {
    const char *c_modelName = env->GetStringUTFChars(modelName, nullptr);
    std::string strModelName(c_modelName);
    env->ReleaseStringUTFChars(modelName, c_modelName);
    CGame::PostToMainThread([vehId, strModelName, parentId]() { RegisterCustomVehicle(vehId, strModelName.c_str(), parentId); });
}

extern "C" JNIEXPORT void JNICALL
Java_com_holy_game_core_Samp_registerCustomObject(JNIEnv *env, jobject thiz, jint objId, jstring modelName, jint parentId) {
    const char *c_modelName = env->GetStringUTFChars(modelName, nullptr);
    std::string strModelName(c_modelName);
    env->ReleaseStringUTFChars(modelName, c_modelName);
    CGame::PostToMainThread([objId, strModelName, parentId]() { RegisterCustomObject(objId, strModelName.c_str(), parentId); });
}
