// Binds ml::Loader to the game (GTA:SA Android 2.10 arm64). Everything that can be tested without the game
// lives in Loader.cpp, including what happens when memory runs out; this file only looks symbols up and
// forwards calls.
#include "ModLoader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include <android/log.h>
#include <dlfcn.h>

#include "../main.h"
#include "../game/Animation/AnimManager.h"
#include "../game/RW/RenderWare.h"
#include "../game/TxdStore.h"
#include "../game/constants.h"
#include "HookScope.h"
#include "Loader.h"

namespace ml {
namespace {

// Files with one of these names are never taken from the mod folder: not as ordinary files, not as IMG
// contents, not as TXD, not through the Custom DL loader. A pattern is a file name ('*' and '?' allowed),
// for example "handling.cfg" or "*.ide". The game asks for files by name, so there is nothing to gain from
// a folder in front of it: of "data/*.ide" only "*.ide" is used. modloader.ini cannot unlock them.
// Empty on purpose: every game file may be replaced.
const char* const kLockedPatterns[] = {
    nullptr,
};

// Always under TESTLIT, never the scoped Android/data folder (hidden from file managers since Android 11).
const char* const kRootCandidates[] = {
    "/storage/emulated/0/TESTLIT/modloader/",
    "/storage/emulated/0/TESTLIT/Modloader/",
    "/storage/emulated/0/TESTLIT/MODLOADER/",
    "/sdcard/TESTLIT/modloader/",
};

#if !VER_x32

struct GameFunctions {
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
    int (*FindTxdSlot)(const char* name) = nullptr;
    const char* currentTxdName = nullptr;                              // CTxdStore::ms_curName
    void* (*GetModelInfoByName)(const char* name, int* index) = nullptr;
    void* acceleratorGetEntry = nullptr;                               // CModelInfoAccelerator::GetEntry
    int* collisionCacheState = nullptr;                                // CColAccel::m_iCacheState
};

GameFunctions g_game;

void* OpenMemoryStream(const uint8_t* data, uint32_t size) {
    RwMemory memory;   // copied by RwStreamOpen
    memory.start = const_cast<RwUInt8*>(data);
    memory.length = size;
    return g_game.StreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
}

void CloseStream(void* stream) { g_game.StreamClose(static_cast<RwStream*>(stream), nullptr); }

void* ReadTexture(void* stream) { return g_game.TextureGtaStreamRead(static_cast<RwStream*>(stream)); }

void AddTextureRef(void* texture) { ++static_cast<RwTexture*>(texture)->refCount; }

void* CreateDictionary() { return g_game.TexDictionaryCreate(); }

void AddTexture(void* dictionary, void* texture) {
    g_game.TexDictionaryAddTexture(dictionary, static_cast<RwTexture*>(texture));
}

void* FindTextureInDictionary(void* dictionary, const char* name) {
    return g_game.TexDictionaryFindNamedTexture(dictionary, name);
}

// The order CTxdStore::RemoveTxd() uses: textures that materials still hold leave the dictionary first and
// live on; the rest are destroyed with it.
void DestroyDictionary(void* dictionary) {
    g_game.TexDictionaryForAllTextures(dictionary, g_game.RemoveIfRefCountIsGreaterThanOne, nullptr);
    g_game.TexDictionaryDestroy(dictionary);
}

const char* CurrentTxdName() { return g_game.currentTxdName; }

int FindTxdSlot(const char* name) { return g_game.FindTxdSlot(name); }

bool GetTxdSlot(int slot, TxdSlotInfo& out) {
    CTxdPool* pool = CTxdStore::ms_pTxdPool;   // the client's pool; the game is patched to use it
    if (!pool || slot < 0 || slot >= pool->GetSize()) return false;
    const TxdDef* def = pool->GetAt(slot);
    if (!def) return false;
    size_t length = strnlen(def->name, sizeof(def->name));
    if (length >= sizeof(out.name)) length = sizeof(out.name) - 1;
    memcpy(out.name, def->name, length);
    out.name[length] = '\0';
    out.parent = def->m_wParentIndex;
    out.refs = def->m_wRefsCount;
    return true;
}

// Replaces CModelInfoAccelerator::GetEntry(CBaseModelInfo**, int*, char*). The original maps the DFF entries
// of the IMG directories to model ids by their position, recorded in MODELS/MINFO.BIN; an overlay changes
// the positions, so the model is looked up by name, exactly as the original does when that file is missing.
void AcceleratorGetEntryHook(void* self, void** outInfo, int* outId, char* name) {
    ML_HOOK_SCOPE();   // not SHADOWHOOK_STACK_SCOPE(): see HookScope.h
    // m_bFileFound: CModelInfoAccelerator::End() must not write a MINFO.BIN from a list this hook never fills.
    static_cast<uint8_t*>(self)[0x1F] = 1;
    *outInfo = g_game.GetModelInfoByName(name, outId);
}

bool BypassModelInfoCache() {
    static std::mutex mutex;
    static int state = 0;   // 0: not tried, 1: installed, -1: failed
    std::lock_guard<std::mutex> lock(mutex);
    if (state == 0) {
        void* original = nullptr;
        void* stub = shadowhook_hook_func_addr(g_game.acceleratorGetEntry,
                                               reinterpret_cast<void*>(&AcceleratorGetEntryHook), &original);
        state = stub ? 1 : -1;
        if (!stub) {
            const int error = shadowhook_get_errno();
            __android_log_print(ANDROID_LOG_ERROR, "HolyModloader", "hook GetEntry gagal: %d / %s", error,
                                shadowhook_to_errmsg(error));
        }
    }
    return state == 1;
}

// CColAccel::m_iCacheState, as CColAccel::startCache() leaves it in CGame::Initialise():
//   1  no usable CINFO.BIN: collision bounds and IPL data are computed from the files and recorded;
//   2  they are taken from CINFO.BIN, by position (collision slots and IPL slots in the order the archives
//      list them, entities in the order the IPL files list them).
// With 0 every reader of the cache computes from the files, as with 1. Two of the recorders (addColDef,
// setIplDef) do not look at the state and go on writing into buffers startCache() allocates either way,
// which changes nothing. No file is written: the client turns CColAccel::endCache() into a plain return
// (game/patches.cpp), so the game neither resets the state nor writes CINFO.BIN.
// The state is only read by the CColAccel functions, and startCache() does nothing else once it has set it.
bool DropCollisionCache() {
    if (*g_game.collisionCacheState == 0) return false;   // the game has not started its cache yet
    *g_game.collisionCacheState = 0;
    return true;
}

#endif   // !VER_x32

void BindGame(GameApi& api) {
#if VER_x32
    api.txdUnavailable = "tidak didukung di build 32-bit";
    api.streamUnavailable = "tidak didukung di build 32-bit";
#else
    static std::string txdWhy, streamWhy;   // GameApi keeps pointers into these
    std::string collisionCacheWhy;
    void* lib = dlopen("libGTASA.so", RTLD_LAZY);
    const auto find = [lib](const char* symbol, std::string& why) -> void* {
        void* address = lib ? dlsym(lib, symbol) : nullptr;
        if (!address && why.empty()) why = std::string("simbol ") + symbol + " tidak ditemukan di libGTASA.so";
        return address;
    };
    const auto bind = [&find](auto& target, const char* symbol, std::string& why) {
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(find(symbol, why));
    };
    bind(g_game.StreamOpen, "_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv", txdWhy);
    bind(g_game.StreamClose, "_Z13RwStreamCloseP8RwStreamPv", txdWhy);
    bind(g_game.TexDictionaryCreate, "_Z21RwTexDictionaryCreatev", txdWhy);
    bind(g_game.TexDictionaryAddTexture, "_Z25RwTexDictionaryAddTextureP15RwTexDictionaryP9RwTexture", txdWhy);
    bind(g_game.TexDictionaryFindNamedTexture, "_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc", txdWhy);
    bind(g_game.TexDictionaryDestroy, "_Z22RwTexDictionaryDestroyP15RwTexDictionary", txdWhy);
    bind(g_game.TexDictionaryForAllTextures,
         "_Z29RwTexDictionaryForAllTexturesPK15RwTexDictionaryPFP9RwTextureS3_PvES4_", txdWhy);
    bind(g_game.TextureGtaStreamRead, "_Z22RwTextureGtaStreamReadP8RwStream", txdWhy);
    bind(g_game.RemoveIfRefCountIsGreaterThanOne, "_Z32RemoveIfRefCountIsGreaterThanOneP9RwTexturePv", txdWhy);
    bind(g_game.FindTxdSlot, "_ZN9CTxdStore11FindTxdSlotEPKc", txdWhy);
    bind(g_game.currentTxdName, "_ZN9CTxdStore10ms_curNameE", txdWhy);
    bind(g_game.GetModelInfoByName, "_ZN10CModelInfo12GetModelInfoEPKcPi", streamWhy);
    bind(g_game.acceleratorGetEntry, "_ZN21CModelInfoAccelerator8GetEntryEPP14CBaseModelInfoPiPc", streamWhy);
    bind(g_game.collisionCacheState, "_ZN9CColAccel13m_iCacheStateE", collisionCacheWhy);

    if (txdWhy.empty()) {
        api.OpenMemoryStream = &OpenMemoryStream;
        api.CloseStream = &CloseStream;
        api.ReadTexture = &ReadTexture;
        api.AddTextureRef = &AddTextureRef;
        api.CreateDictionary = &CreateDictionary;
        api.AddTexture = &AddTexture;
        api.FindTexture = &FindTextureInDictionary;
        api.DestroyDictionary = &DestroyDictionary;
        api.CurrentTxdName = &CurrentTxdName;
        api.FindTxdSlot = &FindTxdSlot;
        api.GetTxdSlot = &GetTxdSlot;
    } else {
        api.txdUnavailable = txdWhy.c_str();
    }
    if (streamWhy.empty()) api.BypassModelInfoCache = &BypassModelInfoCache;
    else api.streamUnavailable = streamWhy.c_str();
    if (collisionCacheWhy.empty()) api.DropCollisionCache = &DropCollisionCache;

    // How many collision, IPL, animation and recording files the game has room for. CStreaming::LoadCdDirectory()
    // registers them without looking, so the loader counts what the archives hold and adds no new name past
    // these numbers. The first three are the client's own pools and array, which the game is patched to use
    // (game/Collision/ColStore.cpp, game/IplStore.cpp, CAnimManager::ms_aAnimBlocks); an animation file also
    // needs a streaming id (game/constants.h). The last one is the game's own table,
    // CVehicleRecording::StreamingArray: 475 entries in libGTASA.so 2.10 arm64, the same number as the id range.
    api.streamLimits.colSlots = static_cast<int>(TOTAL_COL_MODEL_IDS);
    api.streamLimits.iplSlots = static_cast<int>(TOTAL_IPL_MODEL_IDS);
    api.streamLimits.animBlocks = std::min(static_cast<int>(NUM_ANIM_BLOCKS), static_cast<int>(TOTAL_IFP_MODEL_IDS));
    api.streamLimits.recordings = static_cast<int>(TOTAL_RRR_MODEL_IDS);
#endif
}

// The loader, or nullptr when it could not be set up: without memory to scan the mod folder the client runs
// without a modloader, exactly as it did before there was one. Decided once.
Loader* Instance() noexcept {
    static Loader* loader = nullptr;
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            try {
                GameApi api;
                BindGame(api);
                const std::vector<std::string> roots(std::begin(kRootCandidates), std::end(kRootCandidates));
                std::vector<std::string> locked;
                for (const char* const* pattern = kLockedPatterns; *pattern; ++pattern) locked.emplace_back(*pattern);
                // Never destroyed: the game may still call in while the process exits.
                loader = Loader::Create(api, roots, locked).release();
            } catch (...) {
                loader = nullptr;
            }
            if (!loader) {
                __android_log_print(ANDROID_LOG_ERROR, "HolyModloader",
                                    "memori tidak cukup untuk membaca folder mod: modloader tidak aktif");
                return;
            }
            __android_log_print(ANDROID_LOG_INFO, "HolyModloader", "folder mod: %s (%s)",
                                loader->root().empty() ? "(tidak ada)" : loader->root().c_str(),
                                loader->enabled() ? "aktif" : "tidak aktif");
        });
    } catch (...) {   // std::call_once itself; nothing above throws
    }
    return loader;
}

}  // namespace

void Initialize() { Instance(); }

const char* Root() {
    Loader* loader = Instance();
    return loader ? loader->root().c_str() : "";
}

FILE* OpenGameFile(const char* gamePath, const char* (*remap)(const char* gamePath),
                   void (*buildPath)(const char* gamePath, char* out, size_t cap), char* opened, size_t openedCap) {
    if (Loader* loader = Instance()) return loader->OpenGameFile(gamePath, remap, buildPath, opened, openedCap);
    return Loader::OpenStockFile(gamePath, buildPath, opened, openedCap);
}

FILE* OpenClientFile(const char* path, const char* mode) {
    if (Loader* loader = Instance()) return loader->OpenClientFile(path, mode);
    return (path && mode) ? fopen(path, mode) : nullptr;
}

void PrepareStreaming(const char* const* archives, size_t count,
                      void (*buildPath)(const char* gamePath, char* out, size_t cap)) {
    if (Loader* loader = Instance()) loader->PrepareStreaming(archives, count, buildPath);
}

void Tick() {
    if (Loader* loader = Instance()) loader->Tick();
}

bool LookUpModelsByName() {
#if VER_x32
    return false;
#else
    Instance();   // binds the game's functions, also when the loader itself could not be set up
    if (!g_game.acceleratorGetEntry || !g_game.GetModelInfoByName) return false;
    return BypassModelInfoCache();
#endif
}

void OnMemoryPressure() {
    if (Loader* loader = Instance()) loader->OnMemoryPressure();
}

const char* FindPng(const char* textureName) {
    Loader* loader = Instance();
    return loader ? loader->FindPng(textureName) : nullptr;
}

RwTexture* FindTexture(const char* name) {
    Loader* loader = Instance();
    return loader ? static_cast<RwTexture*>(loader->FindTexture(name)) : nullptr;
}

bool WantsTextureHook() {
    Loader* loader = Instance();
    return loader && loader->wantsTextureHook();
}

void SetTextureHookInstalled(bool installed) {
    if (Loader* loader = Instance()) loader->SetTextureHookInstalled(installed);
}

// Without memory for a copy of the name there is no forced context: the TXD the game has current decides.
ScopedTxdContext::ScopedTxdContext(const char* txdName) noexcept : m_previous(nullptr) {
    try {
        if (txdName) m_name = txdName;
    } catch (...) {
        m_name.clear();
    }
    m_previous = Loader::PushTxdContext(&m_name);
}

ScopedTxdContext::~ScopedTxdContext() { Loader::PopTxdContext(m_previous); }

}  // namespace ml
