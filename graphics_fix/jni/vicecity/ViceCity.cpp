// Binds the Vice City map to the game (GTA:SA Android 2.10 arm64). Everything that can be tested without the
// game lives in VcMap, VcStreamer, VcTextures, VcCollision and VcConfig; this file looks the game's functions
// up, registers the models, creates the objects and answers the hooks.
#include "ViceCity.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../main.h"
#include "../game/game.h"
#include "../game/Camera.h"
#include "../game/Clock.h"
#include "../game/Models/ModelInfo.h"
#include "../game/Pools.h"
#include "../game/Streaming.h"
#include "../game/TxdStore.h"
#include "../game/scripting.h"
#include "../net/netgame.h"
#include "../util/patch.h"
#include "../modloader/HookScope.h"
#include "../modloader/ModLoader.h"
#include "VcArchive.h"
#include "VcCollision.h"
#include "VcConfig.h"
#include "VcMap.h"
#include "VcMapData.h"
#include "VcStreamer.h"
#include "VcTextures.h"

namespace vc {

#if VER_x32

// The map needs the 64-bit game: every address and layout below belongs to libGTASA.so 2.10 arm64.
void Initialize() {}
bool Present() { return false; }
void InstallHooks() {}
FILE* OpenGameFile(const char*) { return nullptr; }
bool WantsTextureHook() { return false; }
void SetTextureHookInstalled(bool) {}
RwTexture* FindTexture(const char*) { return nullptr; }
void Tick() {}
void OnMemoryPressure() {}
int ResolveServerModel(int32_t sampModelId) { return sampModelId < 0 ? -1 : sampModelId; }
bool OnServerObject(int32_t, const CVector&) { return false; }
void OnNetworkReset() {}
RwTexture* FindSprite(const char*, const char*) { return nullptr; }

#else   // !VER_x32

namespace {

// Always under TESTLIT, next to the game data and the modloader folder.
const char* const kRootCandidates[] = {
    "/storage/emulated/0/TESTLIT/vice_city/",
    "/storage/emulated/0/TESTLIT/Vice_City/",
    "/storage/emulated/0/TESTLIT/VICE_CITY/",
    "/storage/emulated/0/TESTLIT/vicecity/",
    "/sdcard/TESTLIT/vice_city/",
};

constexpr int kLowestModelId = 5000;          // below: peds, vehicles, weapons and ids the game treats by number
// Ids the client hands out on its own account: the range game/ModelIdLimit adds above the game's 20000. Below
// it a free id may be one a server means by number (SA-MP's own objects go up to 19999).
constexpr int kFirstExtraModelId = CModelInfo::NUM_MODEL_INFOS - LimitAdjuster::EXTRA_MODEL_IDS;
constexpr int kLastArchiveIndex = 7;          // CStreaming::LoadCdDirectory() reads the first eight archives
constexpr int32_t kMinimapSampId = -1500;     // vc_minimap.pwn: AddSimpleModel(-1, 19379, -1500, "blank.dff", "minimap.txd")
constexpr uint16_t kFirstLocalObjectId = 0xFF00;   // CObjectPool ids of map objects that need a material
constexpr uint32_t kNoHandle = 0xFFFFFFFFu;

constexpr size_t kObjectReserve = 400;        // pool places left to the server and the game itself
constexpr size_t kEntryNodeReserve = 3000;
constexpr size_t kPtrNodeReserve = 6000;
constexpr float kSectorSize = 50.0f;
constexpr float kRequestDistance = 90.0f;     // models of objects this near are asked for before they come into view
constexpr uint64_t kNearPassMs = 300;
constexpr uint64_t kTextureGcMs = 2000;
constexpr uint64_t kPressureIdleMs = 3000;
constexpr uint64_t kStatusMs = 5000;
constexpr float kCameraApart = 60.0f;         // the camera is a focus point of its own when this far from the player
constexpr size_t kMissingIdsListed = 24;      // stock model ids named in the status file

// CPhysical::Add() of the Android game links an object into the world only while every sector its bounding
// box touches has an index of at most 120, which ends at world coordinate 3050; Vice City lies at x 4171 ..
// 6963. The four checks are `add w8, <sector>, #120` / `cmp w8, #240`; the compare is widened to 496, which
// moves the limit to 15850. Offsets from the start of the function, original word, new word.
constexpr uint32_t kWorldLimitOffsets[] = {0x60, 0x78, 0x94, 0xAC};
constexpr uint32_t kWorldLimitOld = 0x7103C11F;   // cmp w8, #0xF0
constexpr uint32_t kWorldLimitNew = 0x7107C11F;   // cmp w8, #0x1F0

// CTimeModelInfo: CAtomicModelInfo (0x48 bytes) followed by CTimeInfo {uint8 on, uint8 off, int16 other}.
constexpr size_t kTimeModelBytes = 0x50;
constexpr size_t kTimeInfoOffset = 0x48;
constexpr int kModelInfoInitSlot = 7;         // CBaseModelInfo::Init in the vtable

static_assert(sizeof(CBaseModelInfo) == 0x48, "CBaseModelInfo layout");
static_assert(sizeof(CColModel) == 0x38, "CColModel layout");

// ----------------------------------------------------------------------------------------------------------
// The game

struct GameFunctions {
    void* (*ColModelNew)(size_t size) = nullptr;                                    // CColModel::operator new: null when the pool is full
    void (*ColModelConstruct)(void* col) = nullptr;                                 // CColModel::CColModel
    void (*SetColModel)(void* modelInfo, void* col, bool ownsIt) = nullptr;         // CBaseModelInfo::SetColModel
    void (*LoadCollisionVer3)(uint8_t* info, uint32_t bytes, void* col, const char* name) = nullptr;
    void (*SetAtomicModelInfoFlags)(void* modelInfo, uint32_t ideFlags) = nullptr;
    void (*BaseModelInfoConstruct)(void* modelInfo) = nullptr;                      // CBaseModelInfo::CBaseModelInfo
    uintptr_t timeModelVtable = 0;                                                  // vtable for CTimeModelInfo
    uint8_t* physicalAdd = nullptr;                                                 // CPhysical::Add
    const char* currentTxdName = nullptr;                                           // CTxdStore::ms_curName

    RwStream* (*StreamOpen)(int type, int access, const void* data) = nullptr;
    int (*StreamClose)(RwStream* stream, void* data) = nullptr;
    void* (*TexDictionaryCreate)() = nullptr;
    RwTexture* (*TexDictionaryAddTexture)(void* dictionary, RwTexture* texture) = nullptr;
    RwTexture* (*TexDictionaryFindNamedTexture)(void* dictionary, const char* name) = nullptr;
    int (*TexDictionaryDestroy)(void* dictionary) = nullptr;
    const void* (*TexDictionaryForAllTextures)(const void* dictionary, RwTexture* (*callback)(RwTexture*, void*),
                                               void* data) = nullptr;
    RwTexture* (*TextureGtaStreamRead)(RwStream* stream) = nullptr;
    RwTexture* (*RemoveIfRefCountIsGreaterThanOne)(RwTexture* texture, void* data) = nullptr;

    bool texturesBound = false;
    bool worldBound = false;
    std::string missing;   // first symbol that was not found
};

GameFunctions g_game;

void BindGame() {
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = dlopen("libGTASA.so", RTLD_LAZY);
        bool ok = true;
        const auto bind = [&](auto& target, const char* symbol) {
            void* address = lib ? dlsym(lib, symbol) : nullptr;
            if (!address) {
                ok = false;
                if (g_game.missing.empty()) g_game.missing = symbol;
            }
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(address);
        };
        bind(g_game.StreamOpen, "_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
        bind(g_game.StreamClose, "_Z13RwStreamCloseP8RwStreamPv");
        bind(g_game.TexDictionaryCreate, "_Z21RwTexDictionaryCreatev");
        bind(g_game.TexDictionaryAddTexture, "_Z25RwTexDictionaryAddTextureP15RwTexDictionaryP9RwTexture");
        bind(g_game.TexDictionaryFindNamedTexture, "_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc");
        bind(g_game.TexDictionaryDestroy, "_Z22RwTexDictionaryDestroyP15RwTexDictionary");
        bind(g_game.TexDictionaryForAllTextures,
             "_Z29RwTexDictionaryForAllTexturesPK15RwTexDictionaryPFP9RwTextureS3_PvES4_");
        bind(g_game.TextureGtaStreamRead, "_Z22RwTextureGtaStreamReadP8RwStream");
        bind(g_game.RemoveIfRefCountIsGreaterThanOne, "_Z32RemoveIfRefCountIsGreaterThanOneP9RwTexturePv");
        g_game.texturesBound = ok;

        ok = true;
        bind(g_game.ColModelNew, "_ZN9CColModelnwEm");
        bind(g_game.ColModelConstruct, "_ZN9CColModelC1Ev");
        bind(g_game.SetColModel, "_ZN14CBaseModelInfo11SetColModelEP9CColModelb");
        bind(g_game.LoadCollisionVer3, "_ZN11CFileLoader22LoadCollisionModelVer3EPhjR9CColModelPKc");
        bind(g_game.SetAtomicModelInfoFlags, "_Z23SetAtomicModelInfoFlagsP16CAtomicModelInfoj");
        bind(g_game.BaseModelInfoConstruct, "_ZN14CBaseModelInfoC2Ev");
        bind(g_game.timeModelVtable, "_ZTV14CTimeModelInfo");
        bind(g_game.physicalAdd, "_ZN9CPhysical3AddEv");
        bind(g_game.currentTxdName, "_ZN9CTxdStore10ms_curNameE");
        g_game.worldBound = ok;
    });
}

uint64_t MonotonicUs() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

uint64_t NowMs() { return MonotonicUs() / 1000; }

// ----------------------------------------------------------------------------------------------------------
// Textures: TextureApi over the game's RenderWare

void* ApiOpenMemoryStream(const uint8_t* data, uint32_t size) {
    RwMemory memory;   // copied by RwStreamOpen
    memory.start = const_cast<RwUInt8*>(data);
    memory.length = size;
    return g_game.StreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
}

void ApiCloseStream(void* stream) { g_game.StreamClose(static_cast<RwStream*>(stream), nullptr); }

void* ApiReadTexture(void* stream) { return g_game.TextureGtaStreamRead(static_cast<RwStream*>(stream)); }

void ApiAddTextureRef(void* texture) { ++static_cast<RwTexture*>(texture)->refCount; }

void* ApiCreateDictionary() { return g_game.TexDictionaryCreate(); }

void ApiAddTexture(void* dictionary, void* texture) {
    g_game.TexDictionaryAddTexture(dictionary, static_cast<RwTexture*>(texture));
}

void* ApiFindTexture(void* dictionary, const char* name) {
    return g_game.TexDictionaryFindNamedTexture(dictionary, name);
}

// The order CTxdStore::RemoveTxd() uses: textures that materials still hold leave the dictionary first and
// live on; the rest are destroyed with it.
void ApiDestroyDictionary(void* dictionary) {
    g_game.TexDictionaryForAllTextures(dictionary, g_game.RemoveIfRefCountIsGreaterThanOne, nullptr);
    g_game.TexDictionaryDestroy(dictionary);
}

// ----------------------------------------------------------------------------------------------------------
// State

enum class ColState : uint8_t { Unknown, Loaded, None };

struct ModelRuntime {
    bool registered = false;      // has a model info, a collision model and an entry in the archive
    ColState collision = ColState::Unknown;
};

struct State {
    // --- found by Initialize() ---
    std::string root;             // data folder with a trailing '/', "" when none could be found or made
    std::string modelDir;         // where the .dff/.txd files are
    Config config;
    std::vector<std::string> configNotes;
    FolderIndex files;
    bool present = false;         // the folder holds models and Mode is not off
    std::atomic<bool> textureHook{true};
    int minimapHandle = -1;       // texture cache handle of minimap.txd

    // --- registration ---
    bool registerTried = false;
    std::atomic<bool> ready{false};        // the models are in the game
    std::string failure;                   // why they are not
    Map map;
    std::vector<ModelRuntime> models;
    std::unordered_map<int, uint32_t> modelByGameId;   // model id in the game -> index of the registered model
    std::unique_ptr<uint8_t[]> timeModels; // CTimeModelInfo objects
    std::unique_ptr<Archive> archive;      // what the game streams the models from
    std::atomic<const Archive*> archiveLive{nullptr};
    int archiveIndex = -1;
    int firstId = -1, lastId = -1;
    size_t registeredModels = 0;
    size_t streamableModels = 0;
    bool worldPatched = false;
    std::string worldPatchWhy;
    bool hookInstalled = false;
    std::vector<std::string> notes;        // for the status file

    // --- textures ---
    std::unique_ptr<TextureCache> textures;
    std::unordered_map<std::string, int> txdByName;   // TXD slot name -> texture cache handle
    std::vector<int> txdOfHandle;                     // texture cache handle -> index into map.txd(), -1 for others
    std::atomic<bool> texturesLive{false};
    std::unordered_map<std::string, RwTexture*> sprites;
    uint64_t lastTextureGc = 0;
    uint64_t lastPressure = 0;

    // --- the map in the world ---
    bool placementsReady = false;          // PreparePlacements() has run (first frame in the game)
    Streamer streamer;
    std::vector<uint32_t> handles;         // per placement: script handle of the game object, kNoHandle when none
    std::vector<int32_t> placementModel;   // per placement: model id in the game, -1 when it cannot be placed
    std::vector<int16_t> materialOf;       // per placement: index into kMaterials, -1 for most
    float bounds[4] = {};                  // min x, min y, max x, max y of everything that can be placed
    float maxRange = 0.0f;                 // farthest any object is created from a focus point
    std::atomic<bool> serverUsesMap{false};
    uint64_t lastNearPass = 0;
    uint64_t collisionsLoaded = 0;
    uint64_t collisionFailures = 0;
    size_t stockMissing = 0;               // placements left out because their stock model cannot be used
    std::vector<int> stockMissingIds;      // the first of those models, for the status file

    // --- status file ---
    std::mutex statusMutex;
    std::atomic<bool> statusDirty{false};
    uint64_t lastStatus = 0;
};

State& S() {
    static State* state = new State();   // never destroyed: the game may still call in while the process exits
    return *state;
}

void Note(const std::string& text) {
    State& s = S();
    if (s.notes.size() < 40) s.notes.push_back(text);
    Log("ViceCity: %s", text.c_str());
}

bool LocalMapWanted() {
    State& s = S();
    switch (s.config.mode) {
        case Mode::Always: return true;
        case Mode::Auto: return s.serverUsesMap.load(std::memory_order_relaxed);
        default: return false;
    }
}

// ----------------------------------------------------------------------------------------------------------
// Status file

std::string BuildStatus() {
    State& s = S();
    std::string out = "Vice City - status\n";
    char line[512];
    const auto add = [&](const char* fmt, auto... args) {
        snprintf(line, sizeof(line), fmt, args...);
        out += line;
        out += '\n';
    };
    add("Folder data    : %s", s.root.empty() ? "(tidak ada)" : s.root.c_str());
    if (s.root.empty()) {
        out += "Folder TESTLIT/vice_city/ tidak ada dan tidak bisa dibuat.\n";
        return out;
    }
    add("Pengaturan     : Mode=%s DrawDistance=%.2f MaxObjects=%d BudgetMs=%d TxdIdleSeconds=%d FirstModelId=%d "
        "SpecialFlags=%d WorldPatch=%d",
        ModeName(s.config.mode), static_cast<double>(s.config.drawDistance), s.config.maxObjects, s.config.budgetMs,
        s.config.txdIdleSeconds, s.config.firstModelId, s.config.specialFlags ? 1 : 0, s.config.worldPatch ? 1 : 0);
    for (const std::string& n : s.configNotes) add("  vice_city.ini : %s", n.c_str());

    if (s.modelDir.empty()) {
        out += "File model     : belum ada. Salin isi folder models/vice_city dari repo samp-vice-city (semua .dff dan\n"
               "                 .txd) ke folder data di atas, dan models/minimap.txd ke folder yang sama.\n";
        return out;
    }
    add("File model     : %s (%zu .dff, %zu .txd)", s.modelDir.c_str(), s.files.CountWithExtension(".dff"),
        s.files.CountWithExtension(".txd"));
    add("Minimap        : %s", s.minimapHandle >= 0 ? "minimap.txd ada" : "minimap.txd tidak ada (sprite mdl-1500 kosong)");
    if (s.config.mode == Mode::Off) {
        out += "Map dimatikan (Mode=off).\n";
        return out;
    }
    if (!s.registerTried) {
        add("Pendaftaran    : %s", s.hookInstalled ? "menunggu game memuat data" : "GAGAL: hook CStreaming::Init2 tidak terpasang");
        return out;
    }
    if (!s.ready.load()) {
        add("Pendaftaran    : GAGAL: %s", s.failure.empty() ? "sebab tidak diketahui" : s.failure.c_str());
    } else {
        add("Model          : %zu dari %zu terdaftar (ID %d .. %d), %zu dikenali game dari arsip %s (slot %d)",
            s.registeredModels, kModelCount, s.firstId, s.lastId, s.streamableModels, kArchiveName, s.archiveIndex);
        add("Batas dunia    : %s", s.worldPatched ? "patch CPhysical::Add terpasang (objek boleh sampai x/y 15850)"
                                                  : s.worldPatchWhy.c_str());
        add("Tekstur        : hook RwTextureRead %s", s.textureHook.load() ? "aktif" : "TIDAK terpasang: model tampil tanpa tekstur");
        if (s.textures) {
            const TextureCache::Stats t = s.textures->stats();
            add("                 %zu file TXD terdaftar, %zu di memori (%.1f MB), %llu kali dimuat, %zu gagal, %zu belum lengkap",
                t.files, t.loaded, static_cast<double>(t.bytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(t.loads), t.failed, t.partial);
        }
        const char* local = "tidak (Mode=server): hanya objek kiriman server";
        if (s.config.mode == Mode::Always) local = "ya (Mode=always)";
        else if (s.config.mode == Mode::Auto) {
            local = s.serverUsesMap.load() ? "ya (server ini memakai map Vice City)"
                                           : "menunggu: server belum membuat objek map (filterscript vice_city_037)";
        }
        add("Map lokal      : %s", local);
        add("Objek          : %zu ada, %zu diminati, %zu antre, %zu tidak muat (MaxObjects); dibuat %llu, dihapus %llu",
            s.streamer.activeCount(), s.streamer.wantedCount(), s.streamer.pendingCount(), s.streamer.droppedCount(),
            static_cast<unsigned long long>(s.streamer.createdTotal()),
            static_cast<unsigned long long>(s.streamer.destroyedTotal()));
        add("Collision      : %llu model dimuat, %llu gagal", static_cast<unsigned long long>(s.collisionsLoaded),
            static_cast<unsigned long long>(s.collisionFailures));
        if (s.stockMissing > 0) {
            std::string ids;
            for (const int id : s.stockMissingIds) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            add("Model bawaan   : %zu penempatan dilewati karena model San Andreas/SA-MP-nya tidak ada di data game "
                "(atau tanpa collision): %s%s",
                s.stockMissing, ids.c_str(), s.stockMissingIds.size() >= kMissingIdsListed ? ", ..." : "");
        }
    }
    const std::vector<std::string> missing = s.map.MissingFiles(30);
    if (!missing.empty()) {
        add("File yang kurang (%zu model tidak bisa dipakai):", kModelCount - s.map.usableModels());
        for (const std::string& m : missing) add("  %s", m.c_str());
        if (kModelCount - s.map.usableModels() > missing.size()) out += "  ...\n";
    }
    if (s.textures) {
        const std::vector<std::string> failures = s.textures->Failures(20);
        if (!failures.empty()) out += "TXD yang gagal dibaca:\n";
        for (const std::string& f : failures) add("  %s", f.c_str());
    }
    if (!s.notes.empty()) out += "Catatan:\n";
    for (const std::string& n : s.notes) add("  %s", n.c_str());
    return out;
}

void WriteStatus() {
    State& s = S();
    if (s.root.empty()) return;
    std::lock_guard<std::mutex> lock(s.statusMutex);
    // Marked before the text is put together: what changes meanwhile is written the next time, and a folder
    // that cannot be written to is tried again after kStatusMs, not on every frame.
    s.lastStatus = NowMs();
    s.statusDirty.store(false, std::memory_order_relaxed);
    bool written = false;
    try {
        const std::string text = BuildStatus();
        const std::string path = s.root + "vice_city_status.txt";
        const std::string temp = path + ".tmp";
        const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            size_t done = 0;
            bool ok = true;
            while (ok && done < text.size()) {
                const ssize_t w = write(fd, text.data() + done, text.size() - done);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) ok = false;
                else done += static_cast<size_t>(w);
            }
            close(fd);
            written = ok && rename(temp.c_str(), path.c_str()) == 0;
            if (!written) unlink(temp.c_str());
        }
    } catch (...) {   // no memory for the text
    }
    if (!written) s.statusDirty.store(true, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------------------------------------
// Initialize: the data folder

bool IsDirectory(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string ReadSmallFile(const std::string& path) {
    std::string out;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    char buffer[4096];
    while (out.size() < 256 * 1024) {
        const ssize_t r = read(fd, buffer, sizeof(buffer));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        out.append(buffer, static_cast<size_t>(r));
    }
    close(fd);
    return out;
}

void EnsureTextureCache() {
    State& s = S();
    if (s.textures) return;
    BindGame();
    TextureApi api;
    if (g_game.texturesBound) {
        api.OpenMemoryStream = &ApiOpenMemoryStream;
        api.CloseStream = &ApiCloseStream;
        api.ReadTexture = &ApiReadTexture;
        api.AddTextureRef = &ApiAddTextureRef;
        api.CreateDictionary = &ApiCreateDictionary;
        api.AddTexture = &ApiAddTexture;
        api.FindTexture = &ApiFindTexture;
        api.DestroyDictionary = &ApiDestroyDictionary;
    }
    s.textures.reset(new TextureCache(api));   // without the game's functions it answers "not found" to everything
}

void DoInitialize() {
    State& s = S();
    for (const char* candidate : kRootCandidates) {
        if (IsDirectory(candidate)) {
            s.root = candidate;
            break;
        }
    }
    if (s.root.empty() && mkdir(kRootCandidates[0], 0775) == 0) s.root = kRootCandidates[0];
    if (s.root.empty()) return;

    const std::string iniPath = s.root + "vice_city.ini";
    struct stat st {};
    if (stat(iniPath.c_str(), &st) != 0) {
        // First start: leave the settings where they can be found.
        const std::string text = DefaultConfigText();
        const int fd = open(iniPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0) {
            (void)!write(fd, text.data(), text.size());
            close(fd);
        }
    } else {
        ParseConfig(ReadSmallFile(iniPath), s.config, s.configNotes);
    }

    s.modelDir = FindModelFolder(s.root);
    if (!s.modelDir.empty()) s.files.Scan(s.modelDir);
    s.present = !s.modelDir.empty() && s.config.mode != Mode::Off;

    // The minimap of vc_minimap.pwn: next to the models, or where the repository keeps it.
    if (s.config.mode != Mode::Off) {
        static const char* const kPlaces[] = {"", "models/", "../"};
        for (const char* place : kPlaces) {
            const std::string path = s.root + place + "minimap.txd";
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                EnsureTextureCache();
                s.minimapHandle = s.textures->Add(path, true);
                s.txdOfHandle.push_back(-1);
                break;
            }
        }
        if (s.minimapHandle < 0 && !s.modelDir.empty()) {
            if (const FoundFile* f = s.files.Find("minimap.txd")) {
                EnsureTextureCache();
                s.minimapHandle = s.textures->Add(f->path, true);
                s.txdOfHandle.push_back(-1);
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "ViceCity", "folder data: %s, model: %s, mode %s", s.root.c_str(),
                        s.modelDir.empty() ? "(belum ada)" : s.modelDir.c_str(), ModeName(s.config.mode));
}

// ----------------------------------------------------------------------------------------------------------
// Registration: model infos, TXD slots, collision models, the archive

void SetColBounds(CColModel* col, const float box[6]) {
    // CColModel starts with its bounds: box min, box max, sphere centre, sphere radius.
    float* f = reinterpret_cast<float*>(col);
    float radiusSq = 0.0f;
    for (int i = 0; i < 3; ++i) {
        f[i] = box[i];
        f[3 + i] = box[3 + i];
        f[6 + i] = (box[i] + box[3 + i]) * 0.5f;
        const float half = (box[3 + i] - box[i]) * 0.5f;
        radiusSq += half * half;
    }
    f[9] = std::sqrt(radiusSq);
}

size_t FreeColModels() {
    const PoolAllocator::Pool* pool = CPools::ms_pColModelPool;
    if (!pool || !pool->flags) return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < pool->count; ++i) n += (pool->flags[i] & 0x80) ? 1 : 0;
    return n;
}

// Model ids nobody uses, in ascending order, as many as `need` at most. Without FirstModelId: the highest
// ones of the extra range, away from the ids other custom models are usually given (20000 upwards).
std::vector<int> FreeModelIds(size_t need, int first) {
    std::vector<int> ids;
    const int total = CModelInfo::NUM_MODEL_INFOS;
    if (first > 0) {
        for (int id = std::max(first, kLowestModelId); id < total && ids.size() < need; ++id) {
            if (!CModelInfo::ms_modelInfoPtrs[id]) ids.push_back(id);
        }
    } else {
        for (int id = total - 1; id >= kFirstExtraModelId && ids.size() < need; --id) {
            if (!CModelInfo::ms_modelInfoPtrs[id]) ids.push_back(id);
        }
        std::reverse(ids.begin(), ids.end());
    }
    return ids;
}

bool PatchWorldLimit(std::string& why) {
    uint8_t* fn = g_game.physicalAdd;
    if (!fn) {
        why = "simbol CPhysical::Add tidak ditemukan";
        return false;
    }
    for (const uint32_t offset : kWorldLimitOffsets) {
        uint32_t word;
        memcpy(&word, fn + offset, sizeof(word));
        if (word != kWorldLimitOld && word != kWorldLimitNew) {
            why = "instruksi CPhysical::Add tidak seperti libGTASA.so 2.10 arm64: patch tidak dipasang";
            return false;
        }
    }
    for (const uint32_t offset : kWorldLimitOffsets) {
        uint32_t word = kWorldLimitNew;
        CHook::WriteMemory(reinterpret_cast<uintptr_t>(fn + offset), &word, sizeof(word));
    }
    return true;
}

// Registers everything with the game. Called once, right before the game reads its archive directories:
// every IDE file has been read by then, so the ids that are still free are known. Returns false, with
// s.failure set, when the map stays off.
bool RegisterWithGame() {
    State& s = S();
    BindGame();
    if (!g_game.worldBound) {
        s.failure = "simbol " + g_game.missing + " tidak ditemukan di libGTASA.so (bukan versi 2.10 arm64?)";
        return false;
    }

    s.map.Build(s.files);
    s.models.assign(kModelCount, ModelRuntime());
    if (s.map.usableModels() == 0) {
        s.failure = "tidak ada model yang file .dff dan .txd-nya lengkap";
        return false;
    }

    // --- room in the game's tables
    const size_t usable = s.map.usableModels();
    size_t timed = 0, txdNeeded = 0;
    for (const MapModel& m : s.map.models()) timed += (m.usable && (m.def->timeOn || m.def->timeOff)) ? 1 : 0;
    for (const MapTxd& t : s.map.txd()) txdNeeded += t.users > 0 ? 1 : 0;

    const std::vector<int> ids = FreeModelIds(usable, s.config.firstModelId);
    if (ids.empty()) {
        s.failure = "tidak ada ID model kosong di " + std::to_string(kFirstExtraModelId) + " .. " +
                    std::to_string(CModelInfo::NUM_MODEL_INFOS - 1) + " (lihat FirstModelId di vice_city.ini)";
        return false;
    }
    if (ids.size() < usable) {
        Note("ID model kosong hanya " + std::to_string(ids.size()) + " dari " + std::to_string(usable) +
             " yang dibutuhkan: sisanya tidak didaftarkan (lihat FirstModelId di vice_city.ini)");
    }
    if (CModelInfo::ms_atomicModelInfoStore.m_nCount + (usable - timed) >
        static_cast<size_t>(LimitAdjuster::ATOMIC_MODEL_INFOS)) {
        s.failure = "tempat model info (ATOMIC_MODEL_INFOS di game/LimitAdjuster.h) tidak cukup";
        return false;
    }
    if (!CTxdStore::ms_pTxdPool || static_cast<size_t>(CTxdStore::ms_pTxdPool->GetNoOfFreeSpaces()) < txdNeeded + 16) {
        s.failure = "pool TXD game tidak cukup untuk " + std::to_string(txdNeeded) + " TXD tambahan";
        return false;
    }
    if (FreeColModels() < usable + 64) {
        s.failure = "pool collision model (COL_MODEL_POOL_SIZE di game/LimitAdjuster.h) tidak cukup";
        return false;
    }
    int archiveIndex = -1;
    for (int i = 0; i <= kLastArchiveIndex; ++i) {
        if (!CStreaming::ms_files[i].m_szName[0]) {
            archiveIndex = i;
            break;
        }
    }
    if (archiveIndex < 0) {
        s.failure = "delapan slot arsip IMG game sudah terpakai (kurangi baris IMG di gta.dat)";
        return false;
    }

    s.map.AssignIds(ids);

    // --- TXD slots, under names of their own: a name of the map may also be the name of a stock TXD
    EnsureTextureCache();
    for (size_t j = 0; j < s.map.txd().size(); ++j) {
        MapTxd& t = s.map.txd()[j];
        if (t.users == 0 || !t.file) continue;
        bool free = false;
        for (char c = 't'; c <= 'z' && !free; ++c) {
            t.name[2] = c;
            free = CTxdStore::FindTxdSlot(t.name) == -1;
        }
        if (!free) continue;   // its models are left out below
        t.slot = CTxdStore::AddTxdSlot(t.name, kTextureDatabase, false);
        const int handle = s.textures->Add(t.file->path);
        if (static_cast<size_t>(handle) >= s.txdOfHandle.size()) s.txdOfHandle.resize(static_cast<size_t>(handle) + 1, -1);
        s.txdOfHandle[static_cast<size_t>(handle)] = static_cast<int>(j);
        s.txdByName.emplace(t.name, handle);
    }

    // --- model infos. The name has to be unique in the game too: its archive loader finds models by name.
    std::unordered_set<uint32_t> keys;
    keys.reserve(static_cast<size_t>(CModelInfo::NUM_MODEL_INFOS));
    for (int id = 0; id < CModelInfo::NUM_MODEL_INFOS; ++id) {
        if (const CBaseModelInfo* mi = CModelInfo::ms_modelInfoPtrs[id]) keys.insert(mi->m_nKey);
    }
    s.timeModels.reset(new uint8_t[std::max<size_t>(timed, 1) * kTimeModelBytes]());
    size_t timeUsed = 0;
    s.firstId = s.lastId = -1;
    for (size_t i = 0; i < s.map.models().size(); ++i) {
        MapModel& m = s.map.models()[i];
        if (m.gameId < 0) continue;
        const MapTxd& t = s.map.txd()[static_cast<size_t>(m.txd)];
        if (t.slot < 0 || CModelInfo::ms_modelInfoPtrs[m.gameId]) {
            m.gameId = -1;
            continue;
        }
        bool unique = false;
        for (char c = 'm'; c <= 'z' && !unique; ++c) {
            m.name[2] = c;
            unique = keys.insert(CKeyGen::GetUppercaseKey(m.name)).second;
        }
        if (!unique) {
            m.gameId = -1;
            continue;
        }
        void* col = g_game.ColModelNew(sizeof(CColModel));
        if (!col) {
            m.gameId = -1;
            continue;
        }
        g_game.ColModelConstruct(col);
        SetColBounds(static_cast<CColModel*>(col), m.def->box);

        CBaseModelInfo* mi;
        if (m.def->timeOn || m.def->timeOff) {
            uint8_t* raw = s.timeModels.get() + timeUsed++ * kTimeModelBytes;
            g_game.BaseModelInfoConstruct(raw);
            CHook::SetVTable(raw, g_game.timeModelVtable + 0x10);
            mi = reinterpret_cast<CBaseModelInfo*>(raw);
            CHook::CallVTableFunctionByNum<void>(mi, kModelInfoInitSlot);
            // No day/night partner: with one, CBaseModelInfo::SetColModel() hands the collision model on to it.
            const int16_t noOther = -1;
            raw[kTimeInfoOffset] = m.def->timeOn;
            raw[kTimeInfoOffset + 1] = m.def->timeOff;
            memcpy(raw + kTimeInfoOffset + 2, &noOther, sizeof(noOther));
            CModelInfo::SetModelInfo(m.gameId, mi);
        } else {
            mi = CModelInfo::AddAtomicModel(m.gameId);
        }
        mi->m_nKey = CKeyGen::GetUppercaseKey(m.name);
        strncpy(mi->m_modelName, m.name, sizeof(mi->m_modelName) - 1);
        mi->m_modelName[sizeof(mi->m_modelName) - 1] = '\0';
        mi->m_nTxdIndex = static_cast<int16_t>(t.slot);
        mi->m_fDrawDistance = m.def->drawDistance * s.config.drawDistance;
        g_game.SetAtomicModelInfoFlags(mi, EffectiveIdeFlags(*m.def, s.config.specialFlags));
        g_game.SetColModel(mi, col, true);

        s.models[i].registered = true;
        s.modelByGameId.emplace(m.gameId, static_cast<uint32_t>(i));
        if (s.firstId < 0) s.firstId = m.gameId;
        s.lastId = m.gameId;
        ++s.registeredModels;
    }
    if (s.registeredModels == 0) {
        s.failure = "tidak ada model yang bisa didaftarkan";
        return false;
    }

    // --- the archive, and the game's own way of finding models in it
    const auto abandon = [&](const std::string& why) {
        // The model infos stay behind without files; nothing refers to them any more.
        for (ModelRuntime& r : s.models) r.registered = false;
        s.registeredModels = 0;
        s.failure = why;
        return false;
    };
    // Every model that still has a game id here is registered, and the other way round.
    std::string archiveWhy;
    s.archive.reset(new Archive());
    if (!s.archive->Build(s.map.models(), archiveWhy) || !s.archive->SelfTest(archiveWhy)) return abandon(archiveWhy);
    if (!ml::LookUpModelsByName()) {
        return abandon("hook CModelInfoAccelerator::GetEntry gagal dipasang");
    }
    s.archiveLive.store(s.archive.get(), std::memory_order_release);
    const int index = CStreaming::AddImageToList(kArchiveName, true);
    if (index != archiveIndex || CStreaming::ms_files[index].m_StreamHandle == 0) {
        // CdStreamOpen() answers 0 when the file cannot be opened, and 0 is the handle of the first archive.
        if (index >= 0 && index < static_cast<int>(TOTAL_IMG_ARCHIVES)) {
            CStreaming::ms_files[index].m_szName[0] = '\0';
            CStreaming::ms_files[index].m_StreamHandle = 0;
        }
        s.archiveLive.store(nullptr, std::memory_order_release);
        return abandon("arsip model tidak bisa didaftarkan ke game");
    }
    s.archiveIndex = index;

    if (s.config.worldPatch) s.worldPatched = PatchWorldLimit(s.worldPatchWhy);
    else s.worldPatchWhy = "dimatikan (WorldPatch=0): objek di x/y >= 3050 tidak akan muncul";
    return true;
}

// After the game has read the archive directories: which models did it find?
void AfterArchivesRead() {
    State& s = S();
    size_t found = 0;
    for (size_t i = 0; i < s.models.size(); ++i) {
        if (!s.models[i].registered) continue;
        const int id = s.map.models()[i].gameId;
        if (CStreaming::GetInfo(id).GetCdSize() != 0) ++found;
        else s.models[i].registered = false;   // never asked for: the game would wait for it forever
    }
    s.streamableModels = found;
    if (found == 0) {
        s.failure = "game tidak menemukan satu pun model di arsip " + std::string(kArchiveName);
        return;
    }

    s.texturesLive.store(true, std::memory_order_release);
    s.ready.store(true, std::memory_order_release);
    Log("ViceCity: %zu model terdaftar (ID %d..%d), %zu dikenali game, batas dunia %s", s.registeredModels, s.firstId,
        s.lastId, found, s.worldPatched ? "dipatch" : "TIDAK dipatch");
}

// What can be placed, and where. Runs on the first frame in the game and not at registration: the stock
// models some placements use get their collision models (and with them their size) only after the game has
// read its collision files, which comes after CStreaming::Init2().
void PreparePlacements() {
    State& s = S();
    s.handles.assign(kPlacementCount, kNoHandle);
    s.placementModel.assign(kPlacementCount, -1);
    s.materialOf.assign(kPlacementCount, -1);
    for (size_t k = 0; k < kMaterialCount; ++k) {
        if (kMaterials[k].placement < kPlacementCount) s.materialOf[kMaterials[k].placement] = static_cast<int16_t>(k);
    }
    StreamSettings settings;
    settings.distanceScale = s.config.drawDistance;
    settings.maxActive = static_cast<size_t>(s.config.maxObjects);
    settings.budgetUs = static_cast<uint32_t>(s.config.budgetMs) * 1000u;

    std::vector<StreamItem> items(kPlacementCount);
    bool any = false;
    float maxRange = 0.0f;
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const Placement& p = kPlacements[k];
        StreamItem& item = items[k];
        memcpy(item.pos, p.pos, sizeof(item.pos));
        const TypeInfo& type = kTypes[p.type < kTypeCount ? p.type : kTypeObject];
        item.streamDistance = type.drawDistance + 50.0f;   // as CreateVCObject() in the script
        item.priority = type.priority;
        item.usable = false;
        if (p.model < 0) {
            const int m = s.map.ModelOfPlacement(k);
            if (m >= 0 && s.models[static_cast<size_t>(m)].registered) {
                s.placementModel[k] = s.map.models()[static_cast<size_t>(m)].gameId;
                item.reach = BoxReach(kModels[m].box);
                item.usable = true;
            }
        } else if (p.model < CModelInfo::NUM_MODEL_INFOS) {
            // A stock model has to exist in this copy of the game, and with a collision model: the script
            // command that creates an object reads the model's bounds without looking whether there are any.
            CBaseModelInfo* mi = CModelInfo::ms_modelInfoPtrs[p.model];
            const ModelInfoType kind = mi ? mi->GetModelType() : MODEL_INFO_PED;
            if (mi && mi->m_pColModel && kind != MODEL_INFO_PED && kind != MODEL_INFO_VEHICLE) {
                s.placementModel[k] = p.model;
                item.reach = BoxReach(reinterpret_cast<const float*>(mi->m_pColModel));
                if (!std::isfinite(item.reach) || item.reach > 2000.0f) item.reach = 50.0f;
                item.usable = true;
            } else {
                ++s.stockMissing;
                if (s.stockMissingIds.size() < kMissingIdsListed &&
                    std::find(s.stockMissingIds.begin(), s.stockMissingIds.end(), p.model) == s.stockMissingIds.end()) {
                    s.stockMissingIds.push_back(p.model);
                }
            }
        }
        if (!item.usable) continue;
        if (!any) {
            s.bounds[0] = s.bounds[2] = p.pos[0];
            s.bounds[1] = s.bounds[3] = p.pos[1];
            any = true;
        }
        s.bounds[0] = std::min(s.bounds[0], p.pos[0]);
        s.bounds[1] = std::min(s.bounds[1], p.pos[1]);
        s.bounds[2] = std::max(s.bounds[2], p.pos[0]);
        s.bounds[3] = std::max(s.bounds[3], p.pos[1]);
        maxRange = std::max(maxRange, std::max(item.streamDistance * settings.distanceScale,
                                               item.reach + settings.criticalDistance));
    }
    s.maxRange = maxRange + settings.keepMargin;
    s.streamer.SetSettings(settings);
    s.streamer.SetItems(std::move(items));
    s.placementsReady = true;
    s.statusDirty.store(true, std::memory_order_relaxed);
}

void (*g_originalInit2)() = nullptr;

// CStreaming::Init2(): the game calls it once while it reads gta.dat, after the last IDE file and before the
// first IPL file. It initialises the streaming tables and reads the directory of every registered archive.
void Init2Hook() {
    ML_HOOK_SCOPE();   // not SHADOWHOOK_STACK_SCOPE(): see modloader/HookScope.h
    State& s = S();
    bool registered = false;
    try {
        Initialize();
        if (s.present && !s.registerTried) {
            s.registerTried = true;
            registered = RegisterWithGame();
            if (!registered) Log("ViceCity: map tidak didaftarkan: %s", s.failure.c_str());
        }
    } catch (...) {
        registered = false;
        s.failure = "memori tidak cukup saat mendaftarkan model";
    }
    if (g_originalInit2) g_originalInit2();
    try {
        if (registered) AfterArchivesRead();
        if (s.present) WriteStatus();
    } catch (...) {
        s.failure = "memori tidak cukup saat menyiapkan map";
    }
}

// ----------------------------------------------------------------------------------------------------------
// Collision: read from the DFF the first time an object of the model is about to be created

void EnsureCollision(size_t modelIndex) {
    State& s = S();
    ModelRuntime& r = s.models[modelIndex];
    if (r.collision != ColState::Unknown) return;
    r.collision = ColState::None;   // whatever happens below is not tried again
    const MapModel& m = s.map.models()[modelIndex];
    CBaseModelInfo* mi = CModelInfo::ms_modelInfoPtrs[m.gameId];
    if (!mi || !mi->m_pColModel || !m.dff) return;

    Collision col;
    const ColResult result = ReadCollisionFile(m.dff->path.c_str(), col);
    if (result == ColResult::Loadable) {
        CColModel* cm = mi->m_pColModel;
        // The loader copies what it needs: `col` may go away afterwards. It takes buffer and size from behind
        // the 32-byte file header.
        g_game.LoadCollisionVer3(col.file.data() + kColFileHeader, static_cast<uint32_t>(col.file.size() - kColFileHeader),
                                 cm, m.name);
        // It also takes the bounds of the file: put back the box that covers the geometry as well.
        SetColBounds(cm, m.def->box);
        cm->m_nColSlot = 0;   // the collision file that is always loaded: the game never unloads these
        r.collision = ColState::Loaded;
        ++s.collisionsLoaded;
    } else if (result == ColResult::Bad || result == ColResult::Unsupported) {
        ++s.collisionFailures;
        if (s.collisionFailures <= 10) Note(std::string(m.def->dff) + ": collision tidak dipakai (" + col.detail + ")");
    }
    s.statusDirty.store(true, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------------------------------------
// The objects

class WorldHost final : public StreamHost {
public:
    void BeginTick() { m_counted = false; }

    Result Create(uint32_t index) override {
        State& s = S();
        const Placement& p = kPlacements[index];
        const int model = s.placementModel[index];
        if (model < 0 || !CModelInfo::ms_modelInfoPtrs[model] || !CModelInfo::ms_modelInfoPtrs[model]->m_pColModel) {
            return Result::Never;
        }
        float reach = 1.0f;
        if (p.model < 0) {
            const int m = s.map.ModelOfPlacement(index);
            EnsureCollision(static_cast<size_t>(m));
            reach = BoxReach(kModels[m].box);
        } else {
            reach = BoxReach(reinterpret_cast<const float*>(CModelInfo::ms_modelInfoPtrs[model]->m_pColModel));
            if (!std::isfinite(reach) || reach > 2000.0f) reach = 50.0f;
        }
        if (!HasRoom(reach)) return Result::Later;

        const int16_t material = s.materialOf[index];
        if (material >= 0) {
            // An object with SetObjectMaterial: the client's own object class knows how to do that.
            const MaterialDef& def = kMaterials[material];
            const uint16_t id = static_cast<uint16_t>(kFirstLocalObjectId + material);
            if (!CObjectPool::New(id, model, CVector(p.pos[0], p.pos[1], p.pos[2]), CVector(p.rot[0], p.rot[1], p.rot[2]),
                                  kTypes[p.type].drawDistance)) {
                return Result::Later;
            }
            if (CObjectSamp* object = CObjectPool::GetAt(id)) {
                object->SetMaterial(def.model, def.index, def.txd, def.texture, def.color);
            }
            Took(reach);
            return Result::Done;
        }

        // The way the client creates the objects a server sends (CObjectSamp), without what a plain map
        // object does not need. The handle variable is as wide as the script interpreter writes it.
        uintptr_t handle = ~static_cast<uintptr_t>(0);
        ScriptCommand(&create_object, model, static_cast<double>(p.pos[0]), static_cast<double>(p.pos[1]),
                      static_cast<double>(p.pos[2]), &handle);
        if (handle == ~static_cast<uintptr_t>(0)) return Result::Later;
        // create_object lifts the object by the height of its model; this puts it where the map has it.
        ScriptCommand(&put_object_at, static_cast<int>(handle), static_cast<double>(p.pos[0]),
                      static_cast<double>(p.pos[1]), static_cast<double>(p.pos[2]));
        CPhysical* entity = GamePool_Object_GetAt(static_cast<int>(handle));
        if (!entity) return Result::Later;
        if (p.rot[0] != 0.0f || p.rot[1] != 0.0f || p.rot[2] != 0.0f) {
            entity->Remove();
            entity->SetOrientation(DegreesToRadians(p.rot[0]), DegreesToRadians(p.rot[1]), DegreesToRadians(p.rot[2]));
            entity->UpdateRW();
            entity->UpdateRwFrame();
            entity->Add();
        }
        s.handles[index] = static_cast<uint32_t>(handle);
        Took(reach);
        return Result::Done;
    }

    void Destroy(uint32_t index) override {
        State& s = S();
        const int16_t material = s.materialOf[index];
        if (material >= 0) {
            CObjectPool::Delete(static_cast<uint16_t>(kFirstLocalObjectId + material));
            return;
        }
        const uint32_t handle = s.handles[index];
        s.handles[index] = kNoHandle;
        if (handle == kNoHandle || !GamePool_Object_GetAt(static_cast<int>(handle))) return;   // the game removed it already
        ScriptCommand(&destroy_object, static_cast<int>(handle));
    }

    uint64_t NowUs() override { return MonotonicUs(); }

private:
    // Sectors an object of this reach is linked into, at most (a node of each kind per sector).
    static size_t Nodes(float reach) {
        const size_t side = static_cast<size_t>(std::ceil(2.0f * reach / kSectorSize)) + 1;
        return side * side;
    }

    // CObject::Create() and CPhysical::Add() do not look whether their pools have room.
    bool HasRoom(float reach) {
        if (!m_counted) {
            m_counted = true;
            m_objects = CPools::ms_pObjectPool ? static_cast<size_t>(CPools::ms_pObjectPool->GetNoOfFreeSpaces()) : 0;
            m_entryNodes = CPools::ms_pEntryInfoNodePool
                               ? static_cast<size_t>(CPools::ms_pEntryInfoNodePool->GetNoOfFreeSpaces()) : 0;
            m_ptrNodes = CPools::ms_pPtrNodeDoubleLinkPool
                             ? static_cast<size_t>(CPools::ms_pPtrNodeDoubleLinkPool->GetNoOfFreeSpaces()) : 0;
        }
        const size_t nodes = Nodes(reach);
        return m_objects > kObjectReserve && m_entryNodes > kEntryNodeReserve + nodes &&
               m_ptrNodes > kPtrNodeReserve + nodes;
    }

    void Took(float reach) {
        const size_t nodes = Nodes(reach);
        if (m_objects > 0) --m_objects;
        m_entryNodes -= std::min(m_entryNodes, nodes);
        m_ptrNodes -= std::min(m_ptrNodes, nodes);
    }

    bool m_counted = false;
    size_t m_objects = 0, m_entryNodes = 0, m_ptrNodes = 0;
};

WorldHost& Host() {
    static WorldHost* host = new WorldHost();
    return *host;
}

// Models of the objects right around a focus point are asked for before they come into view. The game does
// this itself (CStreaming::AddModelsToRequestList), but only inside its own map: out here its sector loop
// has nothing to walk. The same pass notices objects the game has removed behind the client's back.
void NearPass(const StreamFocus* focus, size_t focusCount) {
    State& s = S();
    const float limitSq = kRequestDistance * kRequestDistance;
    for (size_t k = 0; k < kPlacementCount; ++k) {
        if (!s.streamer.IsActive(static_cast<uint32_t>(k))) continue;
        const Placement& p = kPlacements[k];
        bool near = false;
        for (size_t f = 0; f < focusCount && !near; ++f) {
            const float dx = p.pos[0] - focus[f].pos[0], dy = p.pos[1] - focus[f].pos[1], dz = p.pos[2] - focus[f].pos[2];
            near = dx * dx + dy * dy + dz * dz <= limitSq;
        }
        if (!near) continue;
        if (s.materialOf[k] < 0 &&
            (s.handles[k] == kNoHandle || !GamePool_Object_GetAt(static_cast<int>(s.handles[k])))) {
            s.handles[k] = kNoHandle;
            s.streamer.Lost(static_cast<uint32_t>(k));
            continue;
        }
        const int model = s.placementModel[k];
        if (model < 0) continue;
        CStreamingInfo& info = CStreaming::GetInfo(model);
        if (info.m_nLoadState != LOADSTATE_NOT_LOADED || info.GetCdSize() == 0) continue;
        if (p.model < 0) {
            const ModelDef& def = kModels[s.map.ModelOfPlacement(k)];
            if ((def.timeOn || def.timeOff) && !CClock::GetIsTimeInRange(def.timeOn, def.timeOff)) continue;
        }
        CStreaming::RequestModel(model, 0);
    }
}

bool IsFinite(const CVector& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

// ----------------------------------------------------------------------------------------------------------
// Public

void Initialize() {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            try {
                DoInitialize();
            } catch (...) {
                S().present = false;
            }
            WriteStatus();
        });
    } catch (...) {   // std::call_once itself
    }
}

bool Present() {
    Initialize();
    return S().present;
}

void InstallHooks() {
    State& s = S();
    void* stub = shadowhook_hook_sym_name("libGTASA.so", "_ZN10CStreaming5Init2Ev", reinterpret_cast<void*>(&Init2Hook),
                                          reinterpret_cast<void**>(&g_originalInit2));
    s.hookInstalled = stub != nullptr && g_originalInit2 != nullptr;
    if (!s.hookInstalled) Log("ViceCity: hook CStreaming::Init2 gagal dipasang: map tidak aktif");
}

FILE* OpenGameFile(const char* gamePath) {
    if (!gamePath) return nullptr;
    const Archive* archive = S().archiveLive.load(std::memory_order_acquire);
    if (!archive || !IsArchivePath(gamePath)) return nullptr;
    return archive->Open();
}

bool WantsTextureHook() {
    Initialize();
    State& s = S();
    return s.present || s.minimapHandle >= 0;
}

void SetTextureHookInstalled(bool installed) {
    State& s = S();
    s.textureHook.store(installed, std::memory_order_relaxed);
    s.statusDirty.store(true, std::memory_order_relaxed);
}

RwTexture* FindTexture(const char* name) {
    State& s = S();
    if (!name || !s.texturesLive.load(std::memory_order_acquire)) return nullptr;
    // The TXD the game has current while it reads a model. The slots of the map all start with "vc".
    const char* current = g_game.currentTxdName;
    if (!current || current[0] != 'v' || current[1] != 'c') return nullptr;
    char key[16];
    const size_t length = strnlen(current, sizeof(key));
    if (length >= sizeof(key)) return nullptr;
    memcpy(key, current, length);
    key[length] = '\0';
    try {
        const auto it = s.txdByName.find(key);
        if (it == s.txdByName.end()) return nullptr;
        return static_cast<RwTexture*>(s.textures->Find(it->second, name, NowMs(), true));
    } catch (...) {
        return nullptr;
    }
}

void OnMemoryPressure() {
    State& s = S();
    if (!s.ready.load(std::memory_order_acquire) || !s.textures) return;
    const uint64_t now = NowMs();
    if (now - s.lastPressure < 1000) return;
    s.lastPressure = now;
    try {
        s.textures->Collect(now, kPressureIdleMs, [&s](int handle) {
            const int j = static_cast<size_t>(handle) < s.txdOfHandle.size() ? s.txdOfHandle[static_cast<size_t>(handle)] : -1;
            if (j < 0) return true;
            const TxdDef* def = CTxdStore::ms_pTxdPool->GetAt(s.map.txd()[static_cast<size_t>(j)].slot);
            return def && def->m_wRefsCount > 0;
        });
    } catch (...) {
    }
}

void Tick() {
    State& s = S();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        const uint64_t now = NowMs();

        // Texture files nobody uses any more: no loaded model takes its textures from the TXD slot.
        if (s.textures && now - s.lastTextureGc >= kTextureGcMs) {
            s.lastTextureGc = now;
            const size_t released = s.textures->Collect(
                now, static_cast<uint64_t>(s.config.txdIdleSeconds) * 1000, [&s](int handle) {
                    const int j =
                        static_cast<size_t>(handle) < s.txdOfHandle.size() ? s.txdOfHandle[static_cast<size_t>(handle)] : -1;
                    if (j < 0) return true;
                    const TxdDef* def = CTxdStore::ms_pTxdPool->GetAt(s.map.txd()[static_cast<size_t>(j)].slot);
                    return def && def->m_wRefsCount > 0;
                });
            if (released > 0) s.statusDirty.store(true, std::memory_order_relaxed);
        }

        if (!s.placementsReady) PreparePlacements();

        // Where the player is, and the camera when it is somewhere else (spectating, class selection).
        StreamFocus focus[2];
        size_t focusCount = 0;
        if (LocalMapWanted()) {
            CPedSamp* player = CLocalPlayer::GetPlayerPed();
            if (player && player->m_pPed) {
                const CVector& p = player->m_pPed->GetPosition();
                if (IsFinite(p)) focus[focusCount++] = {{p.x, p.y, p.z}};
            }
            const CVector& camera = CCamera::Get().GetPosition();
            if (IsFinite(camera)) {
                bool apart = focusCount == 0;
                if (!apart) {
                    const float dx = camera.x - focus[0].pos[0], dy = camera.y - focus[0].pos[1], dz = camera.z - focus[0].pos[2];
                    apart = dx * dx + dy * dy + dz * dz > kCameraApart * kCameraApart;
                }
                if (apart) focus[focusCount++] = {{camera.x, camera.y, camera.z}};
            }
            // Far from the map there is nothing to decide.
            size_t kept = 0;
            for (size_t f = 0; f < focusCount; ++f) {
                const float x = focus[f].pos[0], y = focus[f].pos[1];
                if (x >= s.bounds[0] - s.maxRange && x <= s.bounds[2] + s.maxRange && y >= s.bounds[1] - s.maxRange &&
                    y <= s.bounds[3] + s.maxRange) {
                    focus[kept++] = focus[f];
                }
            }
            focusCount = kept;
        }

        if (focusCount == 0 && s.streamer.activeCount() == 0 && s.streamer.pendingCount() == 0) {
            if (s.statusDirty.load(std::memory_order_relaxed) && now - s.lastStatus >= kStatusMs) WriteStatus();
            return;
        }

        const uint64_t createdBefore = s.streamer.createdTotal(), destroyedBefore = s.streamer.destroyedTotal();
        WorldHost& host = Host();
        host.BeginTick();
        s.streamer.Tick(focus, focusCount, host);
        if (focusCount > 0 && now - s.lastNearPass >= kNearPassMs) {
            s.lastNearPass = now;
            NearPass(focus, focusCount);
        }
        if (s.streamer.createdTotal() != createdBefore || s.streamer.destroyedTotal() != destroyedBefore) {
            s.statusDirty.store(true, std::memory_order_relaxed);
        }
        if (s.statusDirty.load(std::memory_order_relaxed) && now - s.lastStatus >= kStatusMs) WriteStatus();
    } catch (...) {   // called from the game's own loop: nothing may leave as an exception
    }
}

int ResolveServerModel(int32_t sampModelId) {
    State& s = S();
    if (!s.ready.load(std::memory_order_acquire)) return sampModelId < 0 ? -1 : sampModelId;
    try {
        if (sampModelId >= 0) {
            // The ids the map's models have inside the game are this client's own business. A server that
            // sends such a number means a model of its own (one that could not be registered because the
            // map sits on its id): better no object than a building of the map in its place.
            return s.modelByGameId.count(sampModelId) ? -1 : sampModelId;
        }
        const int m = s.map.ModelBySampId(sampModelId);
        if (m < 0 || !s.models[static_cast<size_t>(m)].registered) return -1;
        EnsureCollision(static_cast<size_t>(m));
        return s.map.models()[static_cast<size_t>(m)].gameId;
    } catch (...) {
        return -1;
    }
}

bool OnServerObject(int32_t sampModelId, const CVector& pos) {
    State& s = S();
    if (!s.ready.load(std::memory_order_acquire)) return false;
    try {
        // The server shows that it uses the map by creating an object of it: one that repeats a placement of
        // the map (same model, same place - also a stock San Andreas model, which every client knows), any
        // object with one of the map's own models, or the stand-in for one of those.
        const float p[3] = {pos.x, pos.y, pos.z};
        const int placement = s.map.FindPlacement(sampModelId, p);
        bool usesMap = placement >= 0 || (sampModelId < 0 && s.map.ModelBySampId(sampModelId) >= 0);
        // The client places this one itself: the server's copy is left out.
        bool placedHere = placement >= 0 && s.placementsReady && s.placementModel[static_cast<size_t>(placement)] >= 0;
        // open.mp sends an object with a custom model to a 0.3.7 client as a question mark in the same place.
        // Where the map has a model of its own, that is what it stands for, and it is never shown: not next
        // to the real model, and not instead of one whose files are missing either.
        const bool standIn = placement < 0 && sampModelId == kStandInModel && s.map.FindStandIn(p) >= 0;
        if (standIn) usesMap = placedHere = true;

        if (usesMap && !s.serverUsesMap.exchange(true)) {
            Log("ViceCity: server memakai map Vice City; map dipasang client (Mode=%s)", ModeName(s.config.mode));
            s.statusDirty.store(true, std::memory_order_relaxed);
        }
        return placedHere && LocalMapWanted();
    } catch (...) {
        return false;
    }
}

void OnNetworkReset() {
    State& s = S();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        s.serverUsesMap.store(false, std::memory_order_relaxed);
        if (s.placementsReady) {
            // The objects with a material live in the client's object pool, which the network code empties
            // itself: whether it already has or not, they are gone after this.
            for (size_t k = 0; k < kMaterialCount; ++k) {
                CObjectPool::Delete(static_cast<uint16_t>(kFirstLocalObjectId + k));
                s.streamer.Lost(kMaterials[k].placement);
            }
            if (s.config.mode != Mode::Always) s.streamer.Clear(Host());
        }
        s.statusDirty.store(true, std::memory_order_relaxed);
    } catch (...) {
    }
}

RwTexture* FindSprite(const char* txdName, const char* textureName) {
    if (!txdName || !textureName || !*textureName) return nullptr;
    if ((txdName[0] != 'm' && txdName[0] != 'M') || (txdName[1] != 'd' && txdName[1] != 'D') ||
        (txdName[2] != 'l' && txdName[2] != 'L')) {
        return nullptr;
    }
    char* end = nullptr;
    const long id = strtol(txdName + 3, &end, 10);
    if (end == txdName + 3 || *end != '\0') return nullptr;
    Initialize();
    State& s = S();
    if (!s.textures) return nullptr;
    try {
        int handle = -1;
        if (id == kMinimapSampId) {
            handle = s.minimapHandle;
        } else if (s.ready.load(std::memory_order_acquire)) {
            const int m = s.map.ModelBySampId(static_cast<int32_t>(id));
            if (m >= 0 && s.models[static_cast<size_t>(m)].registered) {
                const auto it = s.txdByName.find(s.map.txd()[static_cast<size_t>(s.map.models()[static_cast<size_t>(m)].txd)].name);
                if (it != s.txdByName.end()) handle = it->second;
            }
        }
        if (handle < 0) return nullptr;
        // A text draw keeps the plain pointer and never gives it back: one reference per texture, kept for good.
        const std::string key = std::to_string(handle) + ":" + textureName;
        const auto cached = s.sprites.find(key);
        if (cached != s.sprites.end()) return cached->second;
        RwTexture* texture = static_cast<RwTexture*>(s.textures->Find(handle, textureName, NowMs(), true));
        if (texture) s.sprites.emplace(key, texture);
        return texture;
    } catch (...) {
        return nullptr;
    }
}

#endif   // !VER_x32

}  // namespace vc
