//
// Holy SA v5.5 - CClothesSystem
//   
// FIX 2026-09:
//   [1] dlsym-only resolution meant a stripped/renamed export silently disabled
//       the whole system (SetClothesByName returned false forever). Every symbol
//       now has a 2.10 arm64 offset fallback taken from the project offset dump.
//   [2] EnsureCJ() forced ped->SetModelIndex(0) even when model 0 was not
//       streamed in. RebuildPlayer() then walked a null RpClump -> SIGSEGV.
//       The switch now only happens once CStreaming reports the model loaded.
//   [3] BuildPlayerClothes() called CClothes::RebuildPlayer(CPlayerPed*) with a
//       plain CPed*. On a remote ped m_pPlayerData is null and the native code
//       dereferences it. Every rebuild is now gated behind a validated desc.
//   [4] g_state kept a raw CPed* that dangled across respawn / skin change.
//       Process() re-validates it against the live player ped each frame.
//   [5] std::array<PedState,1004> burned ~2.4 MB of BSS and ApplyPending copied
//       the whole struct by value. Replaced with an unordered_map.
//   [6] Catalog probing ran access() on 7 paths every frame while unloaded.
//       Now throttled behind a retry counter.
//   [7] SendCatalogToJava() emitted a schema ClothesMenu.java does not parse
//       ({"name","label","texture","model"} vs {"store","typeName","name"}).
//       This file is now the single source of truth for the wire format.
//
// PERF 2026-09 (player.img lag fix) -- inti perbaikan permintaan ini:
//   [P1] REBUILD COALESCING. Dulu setiap SetClothesByName(rebuild=true)
//        langsung memanggil CClothes::RebuildPlayer(force=true), dan
//        CJavaGui::ProcessClothesQueue() menguras 8 aksi per frame. Satu
//        scroll cepat = 8 rebuild = 8 pembacaan SINKRON player.img dalam
//        satu frame. Sekarang penulisan descriptor tetap instan, tapi
//        rebuild dijadwalkan dan hanya dieksekusi SEKALI setelah pemain
//        berhenti menekan.
//   [P2] STATE HASH DEDUPE. Tap item yang sama, atau tap baris lalu tekan
//        APPLY, menghasilkan descriptor yang identik. Hash FNV-1a atas
//        seluruh slot dibandingkan dengan hash hasil build terakhir; kalau
//        sama, rebuild dibatalkan total.
//   [P3] STREAMING GATE. Rebuild ditunda selama CStreaming masih punya
//        request tertunda, supaya pembacaan sinkron player.img tidak
//        ditumpuk di atas pembacaan asinkron yang sedang jalan (inilah
//        sumber freeze ratusan milidetik). Ada batas tunggu supaya tidak
//        pernah kelaparan.
//   [P4] ADAPTIVE BUDGET. Durasi rebuild diukur. Kalau player.img pemain
//        berat (rebuild 400 ms), periode tenang dan interval minimum ikut
//        melebar otomatis, jadi frame rate tetap terjaga.
//   [P5] STREAMING MEMORY FLOOR. CStreaming::ms_memoryAvailable tidak
//        pernah diisi (game menulisnya, tapi instruksinya di-NOP di
//        patches.cpp). Nilainya 0 membuat MakeSpaceFor() menghitung
//        `ms_memoryAvailable - n` sebagai size_t -> underflow -> loop tidak
//        pernah jalan. Diberi lantai yang waras, plus bonus sementara
//        selama wardrobe terbuka supaya geometri baju yang baru dibaca
//        tidak langsung dibuang dan harus dibaca ulang dari player.img.
//   [P6] CATALOG JSON CACHE. JSON katalog (bisa ratusan item) dulu dirakit
//        ulang tiap kali Java meminta. Sekarang dirakit sekali per muat
//        katalog dan dipakai ulang.
//

#include "ClothesSystem.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <strings.h>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <cmath>
#include <unistd.h>

#include "main.h"
#include "game/game.h"
#include "game/util.h"
#include "game/Streaming.h"
#include "game/StreamingInfo.h"
#include "game/Entity/Ped/Ped.h"
#include "game/Entity/Entity.h"
#include "game/RW/rpworld.h"
#include "game/PlayerPedData.h"
#include "util/patch.h"
#include "java_systems/JavaGui.h"
#include "java_systems/cef/CEF.h"
#include "net/playerpool.h"
#include "net/remoteplayer.h"
#include "net/localplayer.h"

uint8_t GetHairColorForPed(CPed* ped);

namespace {
    using DescInitialiseFn = void (*)(void*);
    using DescSetStringFn  = void (*)(void*, const char*, const char*, int);
    using DescSetNumericFn = void (*)(void*, uint32_t, uint32_t, int);
    using RebuildPlayerFn  = void (*)(void*, bool);
    using SetupClothesFn   = void (*)(CPed*);

    // GTA SA Mobile 2.10 arm64-v8a. Verified against SEMUA_OFFSET_2_10_BY_EREN.
    constexpr uintptr_t kOffDescInitialise   = 0x540A08; // CPedClothesDesc::Initialise()
    constexpr uintptr_t kOffDescSetString    = 0x540A88; // ::SetTextureAndModel(const char*, const char*, int)
    constexpr uintptr_t kOffDescSetNumeric   = 0x540A24; // ::SetTextureAndModel(uint, uint, int)
    constexpr uintptr_t kOffRebuildPlayer    = 0x540CDC; // CClothes::RebuildPlayer(CPlayerPed*, bool)

    constexpr int kCatalogMaxRetries = 8;

    // Synthetic catalog type used only by ClothesMenu for skin-color rows.
    // It intentionally lives outside the 18 CPedClothesDesc slots.
    constexpr uint8_t kCatalogHairColorType = 249;
    constexpr uint8_t kCatalogSkinColorType = 250;

    struct HairColorPreset {
        const char* name;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    constexpr HairColorPreset kHairColors[] = {
        {"Default",          255, 255, 255},
        {"Natural Black",     24,  18,  14},
        {"Dark Brown",        55,  31,  18},
        {"Brown",             92,  54,  28},
        {"Chestnut",         118,  63,  34},
        {"Auburn",            98,  34,  22},
        {"Red",               88,  18,  14},
        {"Blonde",           190, 155,  86},
        {"Platinum",         220, 210, 185},
        {"White",             235, 230, 220},
        {"Blue",               28,  66, 140},
        {"Purple",             82,  36, 125},
        {"Pink",              150,  48,  88},
    };
    constexpr uint8_t kHairColorCount =
        static_cast<uint8_t>(sizeof(kHairColors) / sizeof(kHairColors[0]));

    struct SkinColorPreset {
        const char* name;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    // The color is a material tint applied only while each ped atomic is
    // rendered. It therefore does not mutate shared GTA skin textures.
    // Palette is deliberately conservative so the original skin shading
    // remains visible instead of becoming a flat solid color.
    constexpr SkinColorPreset kSkinColors[] = {
        {"Default",       255, 255, 255},
        {"Porcelain",     255, 239, 226},
        {"Fair",          255, 225, 205},
        {"Light Brown",   238, 202, 170},
        {"Brown",         215, 170, 132},
        {"Deep Brown",    190, 135, 100},
        {"Dark Brown",    165, 108,  78},
        {"Warm Brown",    200, 145, 112},
    };
    constexpr uint8_t kSkinColorCount =
        static_cast<uint8_t>(sizeof(kSkinColors) / sizeof(kSkinColors[0]));

    // ---- Deferred rebuild tuning ----------------------------------------
    // Periode tenang: berapa lama setelah tap terakhir sebelum rebuild.
    constexpr uint32_t kQuietMsMin        = 140;
    constexpr uint32_t kQuietMsMax        = 650;
    // Interval minimum antar dua rebuild, apa pun yang terjadi.
    constexpr uint32_t kMinIntervalMsBase = 240;
    constexpr uint32_t kMinIntervalMsMax  = 900;
    // Batas menunggu streamer selesai sebelum rebuild dipaksa jalan.
    constexpr uint32_t kStreamWaitMaxMs   = 1200;

    // ---- Streaming budget ------------------------------------------------
    // Lantai anggaran streaming kalau game meninggalkannya di 0.
    constexpr size_t kStreamBudgetFloor = 64ull * 1024ull * 1024ull;
    // Batas atas kewarasan; di atas ini nilainya dianggap sampah.
    constexpr size_t kStreamBudgetSane  = 1024ull * 1024ull * 1024ull;
    // Bonus sementara selama wardrobe terbuka.
    constexpr size_t kStreamBudgetBonus = 24ull * 1024ull * 1024ull;

    void* g_gtasa = nullptr;
    DescInitialiseFn g_descInit  = nullptr;
    DescSetStringFn  g_setString = nullptr;
    DescSetNumericFn g_setNumeric = nullptr;
    RebuildPlayerFn  g_rebuild   = nullptr;
    SetupClothesFn   g_setup     = nullptr;

    bool g_initialized  = false;
    bool g_menuOpen     = false;
    bool g_catalogLoaded = false;
    int  g_catalogRetries = 0;
    char g_catalogPath[512]{};
    std::vector<CClothesSystem::CatalogItem> g_catalog;
    std::unordered_map<std::string, uint8_t> g_itemShop;

    // PERF-6: JSON katalog dirakit sekali, bukan tiap permintaan Java.
    std::string g_catalogJson;
    bool        g_catalogJsonValid = false;

    int  g_lastSelectedIndex = -1;
    bool g_pendingModelSwitch = false;

    struct PedState {
        CPed* ped = nullptr;
        std::array<CClothesSystem::ClothesItem, CClothesSystem::MAX_CLOTHES> items{};
        uint8_t skinColor = 0;
        uint8_t hairColor = 0;
        bool valid = false;
    };

    PedState g_state{};
    // FIX-5: sparse instead of 1004 fixed slots (~2.4 MB of BSS).
    std::unordered_map<uint16_t, PedState> g_pending;
    // CEF runs on the Android/WebView thread. Never touch GTA peds or
    // streaming from that thread: queue state/catalog requests and consume
    // them from Process(), which already runs on the game thread every frame.
    std::mutex g_cefMutex;
    std::deque<std::string> g_cefStateQueue;
    bool g_cefCatalogRequest = false;
    constexpr size_t kCefStateQueueMax = 24;

    // Render-time skin tint bindings. The maps intentionally use raw pointers
    // only as short-lived runtime handles; callbacks are installed again
    // whenever CClothes rebuilds the player clump.
    std::unordered_map<RpClump*, CPed*> g_skinPedByClump;
    std::unordered_map<CPed*, RpClump*> g_skinClumpByPed;
    std::unordered_map<RpAtomic*, RpAtomicCallBackRender> g_skinOriginalRender;

    struct SkinAtomicMaterials {
        std::array<RpMaterial*, 16> materials{};
        uint8_t count = 0;
        std::array<RpMaterial*, 16> hairMaterials{};
        uint8_t hairCount = 0;
    };
    std::unordered_map<RpAtomic*, SkinAtomicMaterials> g_skinMaterialsByAtomic;
    std::unordered_map<CPed*, uint8_t> g_skinColorByPed;
    std::unordered_map<CPed*, uint8_t> g_hairColorByPed;
    uint8_t g_localSkinColor = 0;
    uint8_t g_localHairColor = 0;

    // ---- Scheduler state -------------------------------------------------
    bool     g_rebuildPending   = false;
    bool     g_rebuildUrgent    = false;
    uint64_t g_rebuildRequestMs = 0;   // kapan permintaan terakhir masuk
    uint64_t g_rebuildLastEndMs = 0;   // kapan rebuild terakhir selesai
    uint32_t g_lastCostMs       = 0;   // durasi rebuild terakhir
    uint64_t g_builtHash        = 0;   // hash state yang benar-benar sudah dibangun
    bool     g_builtHashValid   = false;
    bool     g_busyNotified     = false;

    // ---- Streaming budget state -----------------------------------------
    size_t g_savedBudget   = 0;
    bool   g_budgetBoosted = false;

    uint64_t NowMs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // ------------------------------------------------------------------
    //  Symbol resolution
    // ------------------------------------------------------------------
    // NOTE: never route this through CHook::getSym(): that helper calls
    // exit(0) when a symbol is missing, which would kill the game on any
    // device whose libGTASA build renamed one of these exports.
    template <typename T>
    T ResolveSym(const char* name, uintptr_t offset64) {
        void* addr = nullptr;

        if (g_gtasa && name)
            addr = dlsym(g_gtasa, name);

        if (!addr && offset64 && !VER_x32 && g_libGTASA)
            addr = reinterpret_cast<void*>(g_libGTASA + offset64);

        return reinterpret_cast<T>(addr);
    }

    bool ValidType(uint8_t type) {
        return type < CClothesSystem::MAX_CLOTHES;
    }

    bool IsTattooType(uint8_t type) {
        return type >= CClothesSystem::CLOTHES_TATTOO_LEFT_UPPER_ARM &&
               type <= CClothesSystem::CLOTHES_TATTOO_LOWER_BACK;
    }

    bool IsCJ(const CPed* ped) {
        return ped && ped->m_nModelIndex == 0;
    }

    void CopyText(char* dst, size_t size, const char* src) {
        if (!dst || size == 0) return;
        std::snprintf(dst, size, "%s", src ? src : "");
    }

    CPed* LivePlayerPed() {
        CPed* ped = GamePool_FindPlayerPed();
        if (ped) return ped;

        CPedSamp* player = CGame::FindPlayerPed();
        return player ? player->m_pPed : nullptr;
    }

    // FIX-3: CClothes::RebuildPlayer and CPedClothesDesc both dereference
    // m_pPlayerData->m_pPedClothesDesc without a null check. Only CPlayerPed
    // owns that block; a remote CPed does not.
    bool HasDesc(CPed* ped, void** outDesc) {
        if (!ped || !ped->m_pPlayerData || !ped->m_pPlayerData->m_pPedClothesDesc)
            return false;
        if (outDesc)
            *outDesc = reinterpret_cast<void*>(ped->m_pPlayerData->m_pPedClothesDesc);
        return true;
    }

    // GTA owns the skin hierarchy during the car enter/leave task.
    bool IsVehicleTransition(CPed* ped) {
        return ped && (!ped->m_pIntelligence || ped->IsEnteringCar() || ped->IsExitingVehicle() ||
                       (ped->pVehicle && !ped->IsInVehicle()));
    }

    void UninstallColorRenderer(CPed* ped);

    // FIX-2: never force a model swap onto a ped whose clump is not resident.
    // Returns false (and schedules a retry) instead of crashing.
    bool EnsureCJ(CPed* ped) {
#if defined(EAGLE_MODULAR_CHARACTERS)
        // The modular build never changes a player's carrier through CJ.
        return false;
#endif
        if (!ped) return false;
        if (IsCJ(ped)) {
            g_pendingModelSwitch = false;
            return true;
        }

        if (IsVehicleTransition(ped)) {
            g_pendingModelSwitch = true;
            return false;
        }

        if (!CStreaming::IsModelLoaded(0)) {
            CStreaming::RequestModel(0, STREAMING_GAME_REQUIRED | STREAMING_KEEP_IN_MEMORY);
            g_pendingModelSwitch = true;
            Log("CClothesSystem: model 0 (CJ) not streamed yet, deferring");
            return false;
        }

        UninstallColorRenderer(ped);
        ped->SetModelIndex(0);
        g_pendingModelSwitch = !IsCJ(ped);
        return IsCJ(ped);
    }

    PedState& StateFor(CPed* ped) {
        if (g_state.valid && g_state.ped == ped)
            return g_state;
        g_state = PedState{};
        g_state.ped = ped;
        g_state.valid = true;
        if (ped == LivePlayerPed()) {
            g_state.skinColor = g_localSkinColor;
            g_state.hairColor = g_localHairColor;
        }
        // Ped baru -> hash lama tidak berlaku lagi.
        g_builtHashValid = false;
        return g_state;
    }

    void StoreItem(PedState& state, uint8_t type,
                   const char* texture, const char* model) {
        if (!ValidType(type)) return;
        auto& item = state.items[type];
        CopyText(item.texture, sizeof(item.texture), texture);
        CopyText(item.model, sizeof(item.model), model);
        item.active = item.texture[0] != '\0' || item.model[0] != '\0';
    }

    // PERF-2: hash FNV-1a atas seluruh slot. Dipakai untuk membatalkan
    // rebuild yang tidak mengubah apa pun (tap ganda, tap lalu APPLY,
    // atau tap bolak-balik yang berakhir di item yang sama).
    uint64_t HashState(const PedState& state) {
        uint64_t h = 1469598103934665603ull;
        const auto mixByte = [&h](unsigned char c) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        };
        const auto mixText = [&mixByte](const char* text) {
            if (text) {
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
                     *p; ++p) {
                    mixByte(*p);
                }
            }
            mixByte(0x1F);
        };

        for (uint8_t type = 0; type < CClothesSystem::MAX_CLOTHES; ++type) {
            const auto& item = state.items[type];
            mixByte(static_cast<unsigned char>(type));
            mixByte(item.active ? 1u : 0u);
            mixText(item.texture);
            mixText(item.model);
        }
        mixByte(0x5A);
        mixByte(state.skinColor);
        mixByte(0xA7);
        mixByte(state.hairColor);
        return h;
    }

    const char* DefaultTexture(uint8_t type) {
        switch (type) {
            case CClothesSystem::CLOTHES_SHIRT: return "VEST";
            case CClothesSystem::CLOTHES_HEAD: return "PLAYER_FACE";
            case CClothesSystem::CLOTHES_TROUSERS: return "JEANSDENIM";
            case CClothesSystem::CLOTHES_SHOES: return "SNEAKERBINCBLK";
            default: return nullptr;
        }
    }

    const char* DefaultModel(uint8_t type) {
        switch (type) {
            case CClothesSystem::CLOTHES_SHIRT: return "VEST";
            case CClothesSystem::CLOTHES_HEAD: return "HEAD";
            case CClothesSystem::CLOTHES_TROUSERS: return "JEANS";
            case CClothesSystem::CLOTHES_SHOES: return "SNEAKER";
            default: return nullptr;
        }
    }

    void SetDesc(void* desc, uint8_t type,
                 const char* texture, const char* model) {
        if (!desc || !ValidType(type)) return;

        if (g_setString) {
            g_setString(desc,
                        (texture && *texture) ? texture : nullptr,
                        (model && *model) ? model : nullptr,
                        static_cast<int>(type));
            return;
        }

        if (g_setNumeric) {
            const uint32_t t = texture && *texture
                ? static_cast<uint32_t>(std::strtoul(texture, nullptr, 10)) : 0u;
            const uint32_t m = model && *model
                ? static_cast<uint32_t>(std::strtoul(model, nullptr, 10)) : 0u;
            g_setNumeric(desc, t, m, static_cast<int>(type));
        }
    }

    // ------------------------------------------------------------------
    //  Streaming helpers (PERF-3 / PERF-5)
    // ------------------------------------------------------------------
    bool StreamingBusy() {
        return CStreaming::ms_numModelsRequested > 0;
    }

    // ms_memoryAvailable adalah size_t. Kalau nilainya 0, ekspresi
    // `ms_memoryAvailable - memoryToCleanInBytes` di MakeSpaceFor() akan
    // underflow menjadi angka raksasa dan seluruh jalur eviction mati.
    void EnsureStreamBudgetFloor() {
        const size_t current = CStreaming::ms_memoryAvailable;
        if (current == 0 || current > kStreamBudgetSane) {
            CStreaming::ms_memoryAvailable = kStreamBudgetFloor;
            Log("CClothesSystem: ms_memoryAvailable was %zu, clamped to %zu",
                current, kStreamBudgetFloor);
        }
    }

    void ClearBusyNotify();
    void NotifyBusy(bool busy);

    // ------------------------------------------------------------------
    //  Legacy catalog parser (kept unreachable for source compatibility)
    // ------------------------------------------------------------------
    std::string Trim(const std::string& in) {
        size_t a = 0;
        while (a < in.size() && std::isspace(static_cast<unsigned char>(in[a]))) ++a;
        size_t b = in.size();
        while (b > a && std::isspace(static_cast<unsigned char>(in[b - 1]))) --b;
        return in.substr(a, b - a);
    }

    std::string Lower(const std::string& in) {
        std::string out = in;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    bool IsCommentOrEmpty(const std::string& line) {
        const std::string s = Trim(line);
        return s.empty() || s[0] == '#';
    }

    int ParseInt(const char* s) {
        if (!s || !*s) return 0;
        char* end = nullptr;
        const long v = std::strtol(s, &end, 10);
        return (end && end != s) ? static_cast<int>(v) : 0;
    }

    int Tokenize(const std::string& line, char* buf, size_t bufSize,
                 char** tok, int maxTok) {
        std::string clean = line;
        const size_t comment = clean.find('#');
        if (comment != std::string::npos)
            clean.resize(comment);

        std::snprintf(buf, bufSize, "%s", clean.c_str());

        int count = 0;
        char* save = nullptr;
        for (char* p = strtok_r(buf, " \t\r\n", &save);
             p && count < maxTok;
             p = strtok_r(nullptr, " \t\r\n", &save)) {
            tok[count++] = p;
        }
        return count;
    }

    bool ParseClothesLine(const std::string& line, CClothesSystem::CatalogItem& out) {
        if (IsCommentOrEmpty(line)) return false;

        char buf[512]{};
        char* tok[32]{};
        const int count = Tokenize(line, buf, sizeof(buf), tok, 32);

        // Clothes section format: texture  gxt_name  model  type  ...  price
        if (count < 4) return false;

        const int type = ParseInt(tok[3]);
        if (type < 0 || type >= CClothesSystem::MAX_CLOTHES)
            return false;

        out = CClothesSystem::CatalogItem{};
        CopyText(out.texture, sizeof(out.texture), tok[0]);
        CopyText(out.displayName, sizeof(out.displayName), tok[1]);

        // '-' means texture-only / no DFF model (common for tattoos).
        if (tok[2][0] != '-' || tok[2][1] != '\0')
            CopyText(out.model, sizeof(out.model), tok[2]);

        out.type = static_cast<uint8_t>(type);
        out.price = ParseInt(tok[count - 1]);
        return out.texture[0] != '\0';
    }

    bool ParseHaircutLine(const std::string& line, CClothesSystem::CatalogItem& out) {
        if (IsCommentOrEmpty(line)) return false;

        char buf[512]{};
        char* tok[32]{};
        const int count = Tokenize(line, buf, sizeof(buf), tok, 32);
        if (count < 1) return false;

        out = CClothesSystem::CatalogItem{};

        // Haircuts: textureName  nameTag  modelName  type  ...  price
        CopyText(out.texture, sizeof(out.texture), tok[0]);
        if (count > 1 && tok[1][0] != '-')
            CopyText(out.displayName, sizeof(out.displayName), tok[1]);
        if (count > 2 && tok[2][0] != '-')
            CopyText(out.model, sizeof(out.model), tok[2]);

        out.type = CClothesSystem::CLOTHES_HEAD;
        out.price = ParseInt(tok[count - 1]);

        return out.texture[0] != '\0';
    }

    bool ParseTattooLine(const std::string& line, CClothesSystem::CatalogItem& out) {
        if (IsCommentOrEmpty(line)) return false;

        char buf[512]{};
        char* tok[32]{};
        const int count = Tokenize(line, buf, sizeof(buf), tok, 32);
        if (count < 3) return false;

        out = CClothesSystem::CatalogItem{};
        CopyText(out.texture, sizeof(out.texture), tok[0]);
        CopyText(out.displayName, sizeof(out.displayName), tok[1]);
        out.model[0] = '\0';   // tattoos never carry a DFF

        const int type = ParseInt(tok[2]);
        if (type < CClothesSystem::CLOTHES_TATTOO_LEFT_UPPER_ARM ||
            type > CClothesSystem::CLOTHES_TATTOO_LOWER_BACK)
            return false;

        out.type = static_cast<uint8_t>(type);
        out.price = ParseInt(tok[count - 1]);
        return out.texture[0] != '\0';
    }

    uint8_t ShopFromSection(const std::string& section) {
        const std::string s = Lower(section);
        if (s == "cschp")   return CClothesSystem::SHOP_BINCO;
        if (s == "lacs1")   return CClothesSystem::SHOP_SUBURBAN;
        if (s == "cssprt")  return CClothesSystem::SHOP_PROLAPS;
        if (s == "clothgp") return CClothesSystem::SHOP_ZIP;
        if (s == "csdesgn") return CClothesSystem::SHOP_VICTIM;
        if (s == "csexl")   return CClothesSystem::SHOP_DIDIER_SACHS;
        if (s == "barbers") return CClothesSystem::SHOP_BARBER_1;
        if (s == "barber2") return CClothesSystem::SHOP_BARBER_2;
        if (s == "barber3") return CClothesSystem::SHOP_BARBER_3;
        if (s == "tattoo")  return CClothesSystem::SHOP_TATTOO_1;
        if (s == "tatto2")  return CClothesSystem::SHOP_TATTOO_2;
        if (s == "tatto3")  return CClothesSystem::SHOP_TATTOO_3;
        if (s == "uniform") return CClothesSystem::SHOP_UNIFORM;
        return CClothesSystem::SHOP_SPECIAL;
    }

    void CollectShopItems(const std::vector<std::string>& lines) {
        g_itemShop.clear();
        int shopsDepth = 0;
        uint8_t currentShop = CClothesSystem::SHOP_SPECIAL;

        for (const auto& raw : lines) {
            const std::string s = Trim(raw);
            if (s.empty() || s[0] == '#') continue;
            const std::string lower = Lower(s);

            if (lower.rfind("section ", 0) == 0) {
                const std::string sec = Trim(s.substr(8));
                if (Lower(sec) == "shops") {
                    shopsDepth = 1;
                    currentShop = CClothesSystem::SHOP_SPECIAL;
                    continue;
                }
                if (shopsDepth > 0) {
                    ++shopsDepth;
                    currentShop = ShopFromSection(sec);
                    continue;
                }
                continue;
            }
            if (lower == "end") {
                if (shopsDepth > 0) {
                    --shopsDepth;
                    if (shopsDepth == 0) currentShop = CClothesSystem::SHOP_SPECIAL;
                }
                continue;
            }
            if (shopsDepth < 2) continue;

            char buf[512]{};
            char* tok[32]{};
            const int count = Tokenize(s, buf, sizeof(buf), tok, 32);
            if (count < 2) continue;
            if (!strcasecmp(tok[0], "item") && tok[1][0])
                g_itemShop[std::string(tok[1])] = currentShop;
        }
    }

    bool LoadShoppingDat(const char* path) {
        if (!path || !*path) return false;

        FILE* fp = std::fopen(path, "rb");
        if (!fp) return false;

        std::vector<std::string> lines;
        char line[1024];
        while (std::fgets(line, sizeof(line), fp))
            lines.emplace_back(line);
        std::fclose(fp);

        CollectShopItems(lines);

        std::vector<CClothesSystem::CatalogItem> parsed;
        std::string activeSection;
        int depth = 0;

        for (const auto& raw : lines) {
            const std::string s = Trim(raw);
            if (s.empty() || s[0] == '#') continue;
            const std::string lower = Lower(s);

            if (lower.rfind("section ", 0) == 0) {
                activeSection = Trim(s.substr(8));
                depth = 1;
                continue;
            }
            if (lower == "end") {
                depth = 0;
                activeSection.clear();
                continue;
            }
            if (depth != 1) continue;

            const std::string sec = Lower(activeSection);
            CClothesSystem::CatalogItem item{};
            bool ok = false;

            if (sec == "clothes")       ok = ParseClothesLine(s, item);
            else if (sec == "haircuts") ok = ParseHaircutLine(s, item);
            else if (sec == "tattoos")  ok = ParseTattooLine(s, item);

            if (!ok) continue;

            // Tattoos are first-class catalog entries. They use the native
            // CPedClothesDesc tattoo slots (4..12) and are rendered by
            // CClothes::RebuildPlayer together with the normal components.

            const auto it = g_itemShop.find(std::string(item.texture));
            item.shop = (it != g_itemShop.end()) ? it->second : CClothesSystem::SHOP_SPECIAL;
            parsed.push_back(item);
        }

        // Hair colors are synthetic catalog entries. They do not correspond
        // to CPedClothesDesc slots; ClothesMenu sends action 18 back with the
        // selected color id; legacy hair variants can be used when compiled where
        // available and a render-time material tint as fallback.
        for (uint8_t i = 0; i < kHairColorCount; ++i) {
            CClothesSystem::CatalogItem hair{};
            std::snprintf(hair.texture, sizeof(hair.texture), "hair_color_%u", i);
            CopyText(hair.displayName, sizeof(hair.displayName), kHairColors[i].name);
            hair.type = kCatalogHairColorType;
            hair.shop = CClothesSystem::SHOP_SPECIAL;
            hair.price = 0;
            parsed.push_back(hair);
        }

        // Skin colors are synthetic catalog entries. They do not correspond
        // to CPedClothesDesc slots; ClothesMenu sends action 17 back with the
        // selected color id and SetSkinColor() tints the live ped at render time.
        for (uint8_t i = 0; i < kSkinColorCount; ++i) {
            CClothesSystem::CatalogItem skin{};
            std::snprintf(skin.texture, sizeof(skin.texture), "skin_color_%u", i);
            CopyText(skin.displayName, sizeof(skin.displayName), kSkinColors[i].name);
            skin.type = kCatalogSkinColorType;
            skin.shop = CClothesSystem::SHOP_SPECIAL;
            skin.price = 0;
            parsed.push_back(skin);
        }

        if (parsed.empty()) return false;

        g_catalog.swap(parsed);
        CopyText(g_catalogPath, sizeof(g_catalogPath), path);
        g_catalogLoaded = true;
        g_catalogJsonValid = false;   // PERF-6: cache JSON harus dirakit ulang
        g_catalogJson.clear();
        g_catalogJson.shrink_to_fit();
        Log("CClothesSystem: catalog loaded (%d items) from %s",
            static_cast<int>(g_catalog.size()), g_catalogPath);
        return true;
    }

    bool TryCatalogPaths() {
        // Disabled intentionally. Catalog ownership moved to Pawn/server.
        g_catalogLoaded = true;
        g_catalogRetries = kCatalogMaxRetries;
        std::snprintf(g_catalogPath, sizeof(g_catalogPath), "%s", "server:pawn");
        return true;
    }

    void EnsureCatalog() {
        if (!g_catalogLoaded)
            TryCatalogPaths();
    }

    void JsonEscapeAppend(std::string& out, const char* text) {
        if (!text) return;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            switch (*p) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    // Strip control bytes: NewStringUTF() rejects them and ART
                    // aborts the process on malformed modified-UTF8.
                    if (*p >= 0x20) out += static_cast<char>(*p);
                    break;
            }
        }
    }

    // ------------------------------------------------------------------
    //  Busy indicator for the Java wardrobe
    // ------------------------------------------------------------------
    void NotifyBusy(bool busy) {
        if (!g_menuOpen) return;
        if (g_busyNotified == busy) return;
        g_busyNotified = busy;

        int selectedType = -1;
        if (g_lastSelectedIndex >= 0 &&
            g_lastSelectedIndex < static_cast<int>(g_catalog.size())) {
            selectedType = static_cast<int>(
                g_catalog[static_cast<size_t>(g_lastSelectedIndex)].type);
        }

        const std::string json =
            "{\"type\":" + std::to_string(selectedType) +
            ",\"index\":" + std::to_string(g_lastSelectedIndex) +
            ",\"busy\":" + std::string(busy ? "1" : "0") +
            ",\"cost\":" + std::to_string(g_lastCostMs) + "}";

        CJavaGui::SendPacket(CJavaGui::UI_CLOTHES, CJavaGui::CLOTHES_SET_STATE, json);
    }

    void ClearBusyNotify() {
        g_busyNotified = false;
    }

    // PERF-4: periode tenang dan interval minimum menyesuaikan berat
    // player.img perangkat ini, diukur dari rebuild terakhir.
    uint32_t QuietPeriodMs() {
        uint32_t quiet = kQuietMsMin;
        if (g_lastCostMs > 0) {
            const uint64_t adaptive =
                (static_cast<uint64_t>(g_lastCostMs) * 3ull) / 2ull;
            if (adaptive > quiet)
                quiet = static_cast<uint32_t>(adaptive);
        }
        if (quiet > kQuietMsMax) quiet = kQuietMsMax;
        return quiet;
    }

    uint32_t MinIntervalMs() {
        uint32_t interval = kMinIntervalMsBase;
        // Jangan pernah menghabiskan lebih dari sekitar separuh wall-clock
        // untuk rebuild: kalau satu rebuild makan 400 ms, jarak minimum
        // antar rebuild juga 400 ms.
        if (g_lastCostMs > interval)
            interval = g_lastCostMs;
        if (interval > kMinIntervalMsMax) interval = kMinIntervalMsMax;
        return interval;
    }
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
bool CClothesSystem::Initialize() {
#if defined(EAGLE_MODULAR_CHARACTERS)
    return false;
#endif
    if (!g_initialized) {
        g_gtasa = CHook::lib;
        if (!g_gtasa)
            g_gtasa = dlopen("libGTASA.so", RTLD_NOLOAD | RTLD_NOW);
        if (!g_gtasa)
            g_gtasa = dlopen("libGTASA.so", RTLD_NOW | RTLD_LOCAL);

        if (!g_gtasa)
            Log("CClothesSystem: libGTASA.so handle unavailable, using offsets only");

        g_descInit = ResolveSym<DescInitialiseFn>(
            "_ZN15CPedClothesDesc10InitialiseEv", kOffDescInitialise);
        g_setString = ResolveSym<DescSetStringFn>(
            "_ZN15CPedClothesDesc18SetTextureAndModelEPKcS1_i", kOffDescSetString);
        g_setNumeric = ResolveSym<DescSetNumericFn>(
            "_ZN15CPedClothesDesc18SetTextureAndModelEjji", kOffDescSetNumeric);
        g_rebuild = ResolveSym<RebuildPlayerFn>(
            "_ZN8CClothes13RebuildPlayerEP10CPlayerPedb", kOffRebuildPlayer);

        // No published offset for this one; optional by design.
        g_setup = ResolveSym<SetupClothesFn>(
            "_ZN8CClothes18SetupClothesForPedEP4CPed", 0);

        Log("CClothesSystem: string=%p numeric=%p init=%p setup=%p rebuild=%p",
            g_setString, g_setNumeric, g_descInit, g_setup, g_rebuild);

        // PERF-5: perbaiki anggaran streaming yang ditinggalkan di 0.
        EnsureStreamBudgetFloor();

        g_initialized = true;
    }

    // Catalog authority lives in Pawn. Never probe/read shopping.dat here.
    if (!g_catalogLoaded) {
        g_catalogLoaded = true;
        g_catalogRetries = kCatalogMaxRetries;
        std::snprintf(g_catalogPath, sizeof(g_catalogPath), "%s", "server:pawn");
    }
    return IsReady();
}

bool CClothesSystem::IsReady() {
    return g_rebuild && (g_setString || g_setNumeric);
}

bool CClothesSystem::ReloadCatalog() {
    // Server/Pawn owns the CJ component list. Keep legacy Java catalog empty
    // and explicitly mark it loaded so no filesystem probe can occur.
    g_catalog.clear();
    g_itemShop.clear();
    g_catalogJson.clear();
    g_catalogJsonValid = false;
    g_catalogLoaded = true;
    g_catalogRetries = kCatalogMaxRetries;
    std::snprintf(g_catalogPath, sizeof(g_catalogPath), "%s", "server:pawn");
    return true;
}

bool CClothesSystem::IsCatalogLoaded() { return g_catalogLoaded; }
const char* CClothesSystem::GetCatalogPath() { return g_catalogPath; }
int CClothesSystem::GetCatalogCount() { return static_cast<int>(g_catalog.size()); }

const CClothesSystem::CatalogItem* CClothesSystem::GetCatalogItem(int index) {
    if (index < 0 || index >= static_cast<int>(g_catalog.size())) return nullptr;
    return &g_catalog[static_cast<size_t>(index)];
}

// ---------------------------------------------------------------------------
//  Streaming budget (PERF-5)
// ---------------------------------------------------------------------------
void CClothesSystem::PushStreamingBudget() {
    if (g_budgetBoosted) return;

    EnsureStreamBudgetFloor();
    g_savedBudget = CStreaming::ms_memoryAvailable;
    CStreaming::ms_memoryAvailable = g_savedBudget + kStreamBudgetBonus;
    g_budgetBoosted = true;

    // Pin CJ selama wardrobe terbuka: model 0 tidak boleh dibuang di
    // tengah sesi ganti baju, karena membacanya ulang berarti membuka
    // player.img lagi.
    CStreaming::RequestModel(0, STREAMING_GAME_REQUIRED | STREAMING_KEEP_IN_MEMORY);

    Log("CClothesSystem: streaming budget %zu -> %zu (wardrobe open)",
        g_savedBudget, CStreaming::ms_memoryAvailable);
}

void CClothesSystem::PopStreamingBudget() {
    if (!g_budgetBoosted) return;

    // Hanya kembalikan kalau tidak ada pihak lain yang mengubahnya.
    if (CStreaming::ms_memoryAvailable == g_savedBudget + kStreamBudgetBonus)
        CStreaming::ms_memoryAvailable = g_savedBudget;

    g_budgetBoosted = false;
    Log("CClothesSystem: streaming budget restored to %zu",
        CStreaming::ms_memoryAvailable);
}

// ---------------------------------------------------------------------------
//  Deferred rebuild pipeline (PERF-1..PERF-4)
// ---------------------------------------------------------------------------
void CClothesSystem::ScheduleRebuild(bool urgent) {
    g_rebuildPending = true;
    g_rebuildUrgent = g_rebuildUrgent || urgent;
    g_rebuildRequestMs = NowMs();
    NotifyBusy(true);
}

bool CClothesSystem::IsRebuildPending() { return g_rebuildPending; }

uint32_t CClothesSystem::GetLastRebuildCostMs() { return g_lastCostMs; }

bool CClothesSystem::FlushPendingRebuild() {
    if (!g_rebuildPending) return false;

    CPed* ped = LivePlayerPed();
    if (IsVehicleTransition(ped)) return false;
    if (!ped || (g_state.valid && g_state.ped != ped)) {
        g_rebuildPending = false;
        g_rebuildUrgent = false;
        NotifyBusy(false);
        return false;
    }

    const uint64_t hash = HashState(g_state);
    if (g_builtHashValid && hash == g_builtHash) {
        g_rebuildPending = false;
        g_rebuildUrgent = false;
        NotifyBusy(false);
        return true;
    }

    const uint64_t t0 = NowMs();
    const bool ok = BuildPlayerClothes(ped, true);
    const uint64_t t1 = NowMs();

    g_lastCostMs = static_cast<uint32_t>(t1 - t0);
    g_rebuildLastEndMs = t1;
    g_rebuildPending = false;
    g_rebuildUrgent = false;

    if (ok) {
        g_builtHash = hash;
        g_builtHashValid = true;
    }

    NotifyBusy(false);
    return ok;
}

// ---------------------------------------------------------------------------
//  Native descriptor plumbing
// ---------------------------------------------------------------------------

namespace {
    bool IsSkinTextureName(const char* name) {
        if (!name || !*name) return false;
        // These are the base CJ body textures. Clothes such as shirts/hair
        // intentionally do not match this list, so only exposed skin is tinted.
        return !strcasecmp(name, "cj_ped_head") ||
               !strcasecmp(name, "cj_ped_torso") ||
               !strcasecmp(name, "cj_ped_legs") ||
               !strcasecmp(name, "cj_ped_feet") ||
               !strcasecmp(name, "cj_ped_neck");
    }

    struct SkinMaterialCollectContext {
        SkinAtomicMaterials* out = nullptr;
        CPed* ped = nullptr;
    };

    bool IsHairTextureName(const char* name) {
        if (!name || !*name) return false;

        // Exact names taken from the Haircuts section of the user's
        // legacy catalog data. The old detector only looked for generic tokens and
        // missed assets such as jhericurl, wedge, slope and the barber
        // facial-hair variants. Keep a token fallback for custom assets.
        static constexpr const char* kShoppingHairNames[] = {
            "hairblond", "hairred", "hairblue", "hairgreen", "hairpink",
            "bald", "baldbeard", "baldtash", "baldgoatee", "highfade",
            "highafro", "wedge", "slope", "jhericurl", "cornrows",
            "cornrowsb", "tramline", "groovecut", "mohawk", "mohawkblond",
            "mohawkpink", "mohawkbeard", "afro", "afrotash", "afrobeard",
            "afroblond", "flattop", "elvishair", "beard", "tash", "goatee",
            "afrogoatee"
        };

        for (const char* exact : kShoppingHairNames) {
            if (!strcasecmp(name, exact)) return true;
        }

        const char* tokens[] = {
            "hair", "afro", "buzz", "mohawk", "cornrow", "dread",
            "ponytail", "braid", "curly", "crewcut", "flattop", "fade",
            "jheri", "wedge", "tramline", "groove", "slope", "elvis"
        };
        for (const char* token : tokens) {
            if (strcasestr(name, token) != nullptr)
                return true;
        }
        return false;
    }

    bool IsGenericColoredHairTexture(const char* texture) {
        if (!texture || !*texture) return false;
        return !strcasecmp(texture, "hairblond") ||
               !strcasecmp(texture, "hairred") ||
               !strcasecmp(texture, "hairblue") ||
               !strcasecmp(texture, "hairgreen") ||
               !strcasecmp(texture, "hairpink");
    }

    bool IsMohawkColorable(const char* texture) {
        if (!texture || !*texture) return false;
        return !strcasecmp(texture, "mohawk") ||
               !strcasecmp(texture, "mohawkblond") ||
               !strcasecmp(texture, "mohawkpink");
    }

    bool IsAfroColorable(const char* texture) {
        if (!texture || !*texture) return false;
        return !strcasecmp(texture, "afro") ||
               !strcasecmp(texture, "afroblond");
    }

    const CClothesSystem::CatalogItem* FindHairCatalogItem(const char* texture) {
        if (!texture || !*texture) return nullptr;
        for (const auto& item : g_catalog) {
            if (item.type == CClothesSystem::CLOTHES_HEAD &&
                !strcasecmp(item.texture, texture)) {
                return &item;
            }
        }
        return nullptr;
    }

    // Resolve colors that have a compiled legacy asset. Other colors keep
    // the selected haircut and are rendered through the material tint
    // fallback. This preserves style (mohawk/afro/etc.) instead of replacing
    // it with the generic `head` asset when a matching colored DFF/TXD does
    // not exist.
    bool ResolveHairAssetForColor(const char* currentTexture, uint8_t colorId,
                                  char* outTexture, size_t outTextureSize,
                                  char* outModel, size_t outModelSize) {
        if (!currentTexture || !*currentTexture || !outTexture ||
            outTextureSize == 0 || !outModel || outModelSize == 0) {
            return false;
        }

        const char* target = nullptr;

        if (IsGenericColoredHairTexture(currentTexture)) {
            switch (colorId) {
                case 6:  target = "hairred";   break; // Red
                case 7:  target = "hairblond"; break; // Blonde
                case 10: target = "hairblue";  break; // Blue
                case 12: target = "hairpink";  break; // Pink
                default: break;
            }
        } else if (IsMohawkColorable(currentTexture)) {
            switch (colorId) {
                case 7:  target = "mohawkblond"; break; // Blonde
                case 12: target = "mohawkpink";  break; // Pink
                default: break;
            }
        } else if (IsAfroColorable(currentTexture)) {
            if (colorId == 7) target = "afroblond"; // Blonde
        }

        if (!target || !strcasecmp(target, currentTexture)) return false;

        const auto* found = FindHairCatalogItem(target);
        if (!found) return false;

        std::snprintf(outTexture, outTextureSize, "%s", found->texture);
        std::snprintf(outModel, outModelSize, "%s", found->model[0] ? found->model : "head");
        return true;
    }

    bool IsActiveHairTexture(CPed* ped, const char* name) {
        if (!ped || !name || !*name) return false;
        if (g_state.valid && g_state.ped == ped) {
            const auto& head = g_state.items[CClothesSystem::CLOTHES_HEAD];
            if (head.active && head.texture[0] && head.model[0] &&
                strcasecmp(head.model, "HEAD") != 0 &&
                strcasecmp(head.texture, name) == 0) {
                return true;
            }
        }
        return false;
    }

    RpMaterial* CollectSkinMaterial(RpMaterial* material, void* data) {
        auto* ctx = static_cast<SkinMaterialCollectContext*>(data);
        if (!ctx || !ctx->out || !material) return material;
        const char* name = material->texture ? RwTextureGetName(material->texture) : nullptr;
        if (IsSkinTextureName(name)) {
            if (ctx->out->count < ctx->out->materials.size())
                ctx->out->materials[ctx->out->count++] = material;
        } else if (ctx->out->hairCount < ctx->out->hairMaterials.size() &&
                   (IsHairTextureName(name) || IsActiveHairTexture(ctx->ped, name))) {
            ctx->out->hairMaterials[ctx->out->hairCount++] = material;
        }
        return material;
    }

    uint8_t SkinColorForPed(CPed* ped) {
        if (!ped) return 0;
        const auto it = g_skinColorByPed.find(ped);
        if (it != g_skinColorByPed.end())
            return std::min<uint8_t>(it->second, static_cast<uint8_t>(kSkinColorCount - 1));
        return ped == LivePlayerPed()
            ? std::min<uint8_t>(g_localSkinColor, static_cast<uint8_t>(kSkinColorCount - 1))
            : 0;
    }

    RpAtomic* ColorAtomicRenderCallback(RpAtomic* atomic) {
        if (!atomic) return nullptr;

        CPed* ped = nullptr;
        if (atomic->clump) {
            const auto it = g_skinPedByClump.find(atomic->clump);
            if (it != g_skinPedByClump.end())
                ped = it->second;
        }

        RpAtomicCallBackRender original = nullptr;
        const auto originalIt = g_skinOriginalRender.find(atomic);
        if (originalIt != g_skinOriginalRender.end())
            original = originalIt->second;
        if (original == &ColorAtomicRenderCallback)
            original = nullptr;

        const auto materialsIt = g_skinMaterialsByAtomic.find(atomic);
        if (!ped || materialsIt == g_skinMaterialsByAtomic.end() ||
            (materialsIt->second.count == 0 && materialsIt->second.hairCount == 0)) {
            return original
                ? original(atomic)
                : AtomicDefaultRenderCallBack(atomic);
        }

        const uint8_t skinColorId = SkinColorForPed(ped);
        const uint8_t hairColorId = [&]() -> uint8_t {
            const auto it = g_hairColorByPed.find(ped);
            if (it != g_hairColorByPed.end())
                return std::min<uint8_t>(it->second, static_cast<uint8_t>(kHairColorCount - 1));
            return ped == LivePlayerPed()
                ? std::min<uint8_t>(g_localHairColor, static_cast<uint8_t>(kHairColorCount - 1))
                : 0;
        }();
        const auto& skinPreset = kSkinColors[skinColorId];
        const auto& hairPreset = kHairColors[hairColorId];
        const RwRGBA skinTint{skinPreset.r, skinPreset.g, skinPreset.b, 255};
        const RwRGBA hairTint{hairPreset.r, hairPreset.g, hairPreset.b, 255};

        // Save the live color of each material immediately before drawing and
        // restore it afterwards. This is important because GTA can share
        // geometry/material objects between multiple player instances.
        struct RenderBackup {
            RpMaterial* material = nullptr;
            RwRGBA color{};
        };
        std::array<RenderBackup, 16> backups{};
        const auto& binding = materialsIt->second;
        uint8_t backupCount = 0;
        for (uint8_t i = 0; i < binding.count; ++i) {
            RpMaterial* material = binding.materials[i];
            if (!material || backupCount >= backups.size()) continue;
            backups[backupCount].material = material;
            backups[backupCount].color = material->color;
            material->color = skinTint;
            ++backupCount;
        }
        for (uint8_t i = 0; i < binding.hairCount && backupCount < backups.size(); ++i) {
            RpMaterial* material = binding.hairMaterials[i];
            if (!material) continue;
            backups[backupCount].material = material;
            backups[backupCount].color = material->color;
            material->color = hairTint;
            ++backupCount;
        }

        RpAtomic* result = original
            ? original(atomic)
            : AtomicDefaultRenderCallBack(atomic);

        for (uint8_t i = 0; i < backupCount; ++i) {
            if (backups[i].material)
                backups[i].material->color = backups[i].color;
        }
        return result;
    }

    RpAtomic* InstallColorAtomicCallback(RpAtomic* atomic, void* data) {
        auto* ped = static_cast<CPed*>(data);
        if (!atomic || !ped) return atomic;

        if (atomic->renderCallBack != &ColorAtomicRenderCallback) {
            g_skinOriginalRender[atomic] = atomic->renderCallBack;
            atomic->renderCallBack = &ColorAtomicRenderCallback;
        } else if (g_skinOriginalRender.find(atomic) == g_skinOriginalRender.end()) {
            g_skinOriginalRender[atomic] = AtomicDefaultRenderCallBack;
        }

        SkinAtomicMaterials binding{};
        SkinMaterialCollectContext ctx{&binding, ped};
        if (atomic->geometry)
            RpGeometryForAllMaterials(atomic->geometry, CollectSkinMaterial, &ctx);
        if (binding.count || binding.hairCount)
            g_skinMaterialsByAtomic[atomic] = binding;
        else
            g_skinMaterialsByAtomic.erase(atomic);
        return atomic;
    }

    RpAtomic* UninstallColorAtomicCallback(RpAtomic* atomic, void* /*data*/) {
        if (!atomic) return atomic;
        if (atomic->renderCallBack == &ColorAtomicRenderCallback) {
            const auto it = g_skinOriginalRender.find(atomic);
            atomic->renderCallBack =
                (it != g_skinOriginalRender.end() && it->second)
                    ? it->second
                    : AtomicDefaultRenderCallBack;
        }
        g_skinOriginalRender.erase(atomic);
        g_skinMaterialsByAtomic.erase(atomic);
        return atomic;
    }

    void InstallColorRenderer(CPed* ped) {
        if (!ped || !ped->m_pRwClump ||
            (SkinColorForPed(ped) == 0 && GetHairColorForPed(ped) == 0)) return;

        RpClump* clump = ped->m_pRwClump;
        const auto oldIt = g_skinClumpByPed.find(ped);
        if (oldIt != g_skinClumpByPed.end() && oldIt->second != clump)
            g_skinPedByClump.erase(oldIt->second);

        g_skinClumpByPed[ped] = clump;
        g_skinPedByClump[clump] = ped;
        RpClumpForAllAtomics(clump, InstallColorAtomicCallback, ped);
    }

    void UninstallColorRenderer(CPed* ped) {
        if (!ped) return;
        auto clumpIt = g_skinClumpByPed.find(ped);
        if (clumpIt != g_skinClumpByPed.end()) {
            RpClump* clump = clumpIt->second;
            // Do not traverse a clump which GTA has already replaced.
            if (clump && ped->m_pRwClump == clump)
                RpClumpForAllAtomics(clump, UninstallColorAtomicCallback, ped);
            g_skinPedByClump.erase(clump);
            g_skinClumpByPed.erase(clumpIt);
        }
    }
}

bool CClothesSystem::BuildPlayerClothes(CPed* ped, bool force) {
    if (!ped || !Initialize() || !g_rebuild)
        return false;
    if (IsVehicleTransition(ped)) return false;
    if (!EnsureCJ(ped))
        return false;

    // FIX-8 (FC 2026-09, SIGSEGV @0x30 di CClothesBuilder::BlendGeometry):
    // EnsureCJ() hanya memastikan ped->m_nModelIndex == 0; ia TIDAK memeriksa
    // apakah RwObject model 0 masih ada. Karena rebuild sekarang DITUNDA oleh
    // penjadwal, model pemain bisa saja dibuang streamer di antara saat
    // dijadwalkan dan saat dieksekusi. Kalau itu terjadi,
    // CClothes::ConstructPedModel meneruskan RpClump* null ke
    // CreateSkinnedClump dan BlendGeometry men-deref null+0x30.
    // Streaming.cpp sudah mem-pin MODEL_PLAYER, ini lapis kedua supaya modul
    // pakaian tetap aman di build mana pun.
    if (!CStreaming::IsModelLoaded(0)) {
        CStreaming::RequestModel(0, STREAMING_GAME_REQUIRED | STREAMING_KEEP_IN_MEMORY);
        g_pendingModelSwitch = true;
        Log("CClothesSystem: model 0 evicted before rebuild, deferring");
        return false;
    }

    // FIX-3: RebuildPlayer() indexes m_pPlayerData->m_pPedClothesDesc with no
    // null check of its own.
    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) {
            Log("CClothesSystem: ped %p has no clothes descriptor, rebuild skipped", ped);
            return false;
        }
    }

    // RebuildPlayer replaces the RW clump. Detach render callbacks first.
    UninstallColorRenderer(ped);
    g_rebuild(reinterpret_cast<void*>(ped), force);
    if (SkinColorForPed(ped) != 0 || GetHairColorForPed(ped) != 0)
        InstallColorRenderer(ped);
    else
        UninstallColorRenderer(ped);
    return true;
}

namespace {
    // Native tattoo renderer: GTA SA's CClothes builder consumes the 9
    // tattoo slots from CPedClothesDesc when RebuildPlayer() is called.
    // This wrapper deliberately keeps all descriptor writes on the game
    // thread and never touches RenderWare objects directly.
    bool ApplyTattooNative(CPed* ped, uint8_t slot, const char* texture,
                           bool rebuild) {
        if (!ped || !IsTattooType(slot) || !CClothesSystem::Initialize())
            return false;
        if (!EnsureCJ(ped)) return false;

        void* desc = nullptr;
        if (!HasDesc(ped, &desc)) {
            if (g_setup) g_setup(ped);
            if (!HasDesc(ped, &desc)) return false;
        }

        // Tattoo = texture-only descriptor entry.
        SetDesc(desc, slot, (texture && *texture) ? texture : nullptr, nullptr);
        auto& state = StateFor(ped);
        if (texture && *texture)
            StoreItem(state, slot, texture, "");
        else
            state.items[slot] = CClothesSystem::ClothesItem{};

        if (!rebuild) return true;

        if (ped == LivePlayerPed()) {
            CClothesSystem::ScheduleRebuild(false);
            return true;
        }
        return CClothesSystem::BuildPlayerClothes(ped, true);
    }
}

bool CClothesSystem::SetClothesByName(CPed* ped, uint8_t type,
                                      const char* textureName,
                                      const char* modelName,
                                      bool rebuild) {
    if (!ped || !ValidType(type) || !Initialize())
        return false;
    // Tattoo slots are valid CPedClothesDesc slots in the native clothes
    // descriptor. They must not be routed through the normal DFF component
    // assumptions: tattoo entries are texture-only (modelName == nullptr).
    if (!EnsureCJ(ped))
        return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) return false;
    }

    if (!g_setString && !g_setNumeric)
        return false;

    // Menulis descriptor itu murah (hanya hitung hash nama + 2 store),
    // jadi selalu dilakukan langsung. Yang mahal - dan yang ditunda -
    // adalah rebuild clump di bawah.
    SetDesc(desc, type, textureName, modelName);
    auto& state = StateFor(ped);
    StoreItem(state, type, textureName, modelName);

    // Hair Color keeps descriptor variants only when a compiled legacy entry exists.
    // Saat pemain mengganti MODEL rambut, pertahankan warna rambut yang
    // sudah dipilih dan gunakan variant asset bila tersedia (mis.
    // mohawk -> mohawkblond / mohawkpink, afro -> afroblond). Untuk warna
    // yang tidak punya variant per-model, render callback tetap memberi tint
    // tanpa mengganti model rambut.
    if (type == CLOTHES_HEAD && ped == LivePlayerPed() && state.hairColor != 0) {
        char variantTexture[64]{};
        char variantModel[64]{};
        if (ResolveHairAssetForColor(textureName, state.hairColor,
                                     variantTexture, sizeof(variantTexture),
                                     variantModel, sizeof(variantModel))) {
            SetDesc(desc, type, variantTexture, variantModel);
            StoreItem(state, type, variantTexture, variantModel);
        }
    }

    if (!rebuild)
        return true;

    // PERF-1: jangan pernah rebuild inline dari jalur UI. Pemain yang
    // menggeser daftar bisa memicu belasan panggilan dalam satu frame.
    if (ped == LivePlayerPed()) {
        ScheduleRebuild(false);
        return true;
    }

    // Ped non-lokal tidak dilayani scheduler; jalankan jalur lama.
    return BuildPlayerClothes(ped, true);
}

bool CClothesSystem::ClearClothesSlot(CPed* ped, uint8_t type, bool rebuild) {
    if (!ped || !ValidType(type) || !Initialize())
        return false;
    if (!EnsureCJ(ped))
        return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) return false;

    if (type <= CLOTHES_SHOES) {
        SetDesc(desc, type, DefaultTexture(type), DefaultModel(type));
        StoreItem(StateFor(ped), type, DefaultTexture(type), DefaultModel(type));
    } else {
        SetDesc(desc, type, nullptr, nullptr);
        StateFor(ped).items[type] = ClothesItem{};
    }

    if (!rebuild)
        return true;

    if (ped == LivePlayerPed()) {
        ScheduleRebuild(false);
        return true;
    }

    return BuildPlayerClothes(ped, true);
}

bool CClothesSystem::ResetToDefaultCJ(CPed* ped) {
    if (!ped || !Initialize()) return false;
    if (!EnsureCJ(ped)) return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) return false;
    }

    if (g_descInit) g_descInit(desc);

    auto& state = StateFor(ped);
    state.items = {};

    for (uint8_t type = 0; type <= CLOTHES_SHOES; ++type) {
        SetDesc(desc, type, DefaultTexture(type), DefaultModel(type));
        StoreItem(state, type, DefaultTexture(type), DefaultModel(type));
    }

    // Clear all 9 tattoo slots as part of a full reset.
    for (uint8_t type = CLOTHES_TATTOO_LEFT_UPPER_ARM;
         type <= CLOTHES_TATTOO_LOWER_BACK; ++type) {
        SetDesc(desc, type, nullptr, nullptr);
        state.items[type] = ClothesItem{};
    }

    g_lastSelectedIndex = -1;

    if (ped == LivePlayerPed()) {
        // Reset adalah aksi eksplisit pemain: lewati periode tenang,
        // tapi tetap hormati interval minimum dan gerbang streaming.
        ScheduleRebuild(true);
        return true;
    }

    return BuildPlayerClothes(ped, true);
}

bool CClothesSystem::ApplyClothesData(CPed* ped,
                                      const ClothesItem* items,
                                      int count) {
    if (!ped || !items || count < 0 || count > MAX_CLOTHES)
        return false;
    if (!Initialize()) return false;
    if (!EnsureCJ(ped)) return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) return false;
    }

    if (g_descInit) g_descInit(desc);

    auto& state = StateFor(ped);
    state.items = {};

    bool applied = false;
    for (int i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (!ValidType(static_cast<uint8_t>(i))) continue;
        const uint8_t type = static_cast<uint8_t>(i);

        if (!item.active) {
            if (IsTattooType(type))
                SetDesc(desc, type, nullptr, nullptr);
            continue;
        }

        // Tattoo slots are texture-only. Normal clothing may carry a DFF
        // model; tattoos deliberately pass nullptr for the model.
        SetDesc(desc, type, item.texture,
                IsTattooType(type) ? nullptr :
                (item.model[0] ? item.model : nullptr));
        StoreItem(state, static_cast<uint8_t>(i), item.texture, item.model);
        applied = true;
    }

    if (!applied)
        return ResetToDefaultCJ(ped);

    if (ped == LivePlayerPed()) {
        ScheduleRebuild(true);
        return true;
    }

    return BuildPlayerClothes(ped, true);
}

bool CClothesSystem::ApplyStoredClothes(CPed* ped) {
    if (!ped || !g_state.valid || g_state.ped != ped) return false;
    // ApplyClothesData clears the destination state before reading its input.
    // Snapshot our stored outfit so a reapply cannot erase its own source.
    const auto items = g_state.items;
    return ApplyClothesData(ped, items.data(), CClothesSystem::MAX_CLOTHES);
}

void CClothesSystem::ClearPed(CPed* ped) {
    if (!ped) return;
    g_skinColorByPed.erase(ped);
    g_hairColorByPed.erase(ped);
    UninstallColorRenderer(ped);
    g_skinClumpByPed.erase(ped);
    if (g_state.valid && g_state.ped == ped) {
        g_state = PedState{};
        g_builtHashValid = false;
        g_rebuildPending = false;
        g_rebuildUrgent = false;
    }
}

void CClothesSystem::BeforeClumpReplaced(CPed* ped) {
    UninstallColorRenderer(ped);
}

void CClothesSystem::AfterClumpReplaced(CPed* ped) {
    if (!ped || !ped->m_pRwClump) return;
    if (SkinColorForPed(ped) != 0 || GetHairColorForPed(ped) != 0)
        InstallColorRenderer(ped);
}

bool CClothesSystem::SetClothesComponent(CPed* ped, uint8_t type,
                                         uint32_t drawable,
                                         uint32_t texture) {
    if (!ped || !ValidType(type) || !Initialize()) return false;
    if (!EnsureCJ(ped)) return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) return false;
    }

    // Numeric network path must never pass numeric values through the string API.
    if (g_setNumeric) {
        g_setNumeric(desc, texture, drawable, static_cast<int>(type));

        // Simpan bentuk numerik sebagai teks supaya hash state tetap
        // membedakan komponen yang berbeda. Menyimpan string kosong akan
        // membuat dua komponen berbeda terlihat identik bagi dedupe.
        char t[32], m[32];
        std::snprintf(t, sizeof(t), "%u", texture);
        std::snprintf(m, sizeof(m), "%u", drawable);
        StoreItem(StateFor(ped), type, t, m);

        if (ped == LivePlayerPed()) {
            ScheduleRebuild(true);
            return true;
        }
        return BuildPlayerClothes(ped, true);
    }

    char t[32], m[32];
    std::snprintf(t, sizeof(t), "%u", texture);
    std::snprintf(m, sizeof(m), "%u", drawable);
    return SetClothesByName(ped, type, t, m, true);
}

bool CClothesSystem::ApplyNetworkState(CPed* ped,
                                       const ClothesItem* items, int count) {
    return ApplyClothesData(ped, items, count);
}

void CClothesSystem::StorePending(uint16_t playerId,
                                  const ClothesItem* items, int count,
                                  uint8_t hairColor, uint8_t skinColor) {
    if (!items || count < 0 || count > MAX_CLOTHES)
        return;

    PedState dst{};
    dst.valid = true;
    dst.hairColor = std::min<uint8_t>(hairColor, static_cast<uint8_t>(kHairColorCount - 1));
    dst.skinColor = std::min<uint8_t>(skinColor, static_cast<uint8_t>(kSkinColorCount - 1));
    for (int i = 0; i < count; ++i)
        dst.items[i] = items[i];

    g_pending[playerId] = dst;
}

void CClothesSystem::ClearNetworkState(uint16_t playerId) {
    g_pending.erase(playerId);
}

bool CClothesSystem::ApplyPending(uint16_t playerId, CPed* ped) {
    if (!ped) return false;

    const auto it = g_pending.find(playerId);
    if (it == g_pending.end() || !it->second.valid)
        return false;
    if (IsVehicleTransition(ped)) return false;

    // FIX-5: move out, then erase. No full-struct copy.
    PedState pending = std::move(it->second);
    g_pending.erase(it);

    const bool ok = ApplyClothesData(ped, pending.items.data(), CClothesSystem::MAX_CLOTHES);
    if (ok) {
        SetHairColor(ped, pending.hairColor);
        SetSkinColor(ped, pending.skinColor);
    }
    return ok;
}

const char* CClothesSystem::GetTypeName(uint8_t type) {
    if (type == kCatalogHairColorType) return "Hair Color";
    if (type == kCatalogSkinColorType) return "Skin Color";

    static const char* names[MAX_CLOTHES] = {
        "Shirt", "Head / Hair", "Trousers", "Shoes",
        "Tattoo - Left Upper Arm", "Tattoo - Left Lower Arm",
        "Tattoo - Right Upper Arm", "Tattoo - Right Lower Arm",
        "Tattoo - Back", "Tattoo - Left Chest", "Tattoo - Right Chest",
        "Tattoo - Stomach", "Tattoo - Lower Back",
        "Necklace", "Watch", "Glasses", "Hat", "Extra"
    };
    return ValidType(type) ? names[type] : "Unknown";
}

const char* CClothesSystem::GetShopName(uint8_t shop) {
    static const char* names[SHOP_MAX] = {
        "All", "Binco", "Sub Urban", "ProLaps", "ZIP", "Victim",
        "Didier Sachs", "Barber 1", "Barber 2", "Barber 3",
        "Tattoo 1", "Tattoo 2", "Tattoo 3", "Uniform", "Special"
    };
    return shop < SHOP_MAX ? names[shop] : "Special";
}

const CClothesSystem::ClothesItem* CClothesSystem::GetCurrent(uint8_t type) {
    if (!ValidType(type) || !g_state.valid) return nullptr;
    return &g_state.items[type];
}

bool CClothesSystem::HasStoredClothes(CPed* ped) {
    return ped && g_state.valid && g_state.ped == ped;
}

// ---------------------------------------------------------------------------
//  Menu lifecycle
// ---------------------------------------------------------------------------
void CClothesSystem::OpenMenu() {
    Initialize();

    // PERF-5: naikkan anggaran + pin CJ sebelum item pertama dipilih.
    PushStreamingBudget();
    ClearBusyNotify();

    if (g_menuOpen) {
        CJavaGui::TempToggle(CJavaGui::UI_CLOTHES, true);
        return;
    }
    g_menuOpen = true;
    CJavaGui::Create(CJavaGui::UI_CLOTHES);
}

void CClothesSystem::CloseMenu() {
    if (!g_menuOpen) {
        PopStreamingBudget();
        return;
    }

    // Jangan tinggalkan pemain dengan baju setengah jadi.
    FlushPendingRebuild();

    CJavaGui::Destroy(CJavaGui::UI_CLOTHES);
    g_menuOpen = false;
    ClearBusyNotify();
    PopStreamingBudget();
}

void CClothesSystem::OnMenuClosed() {
    if (g_rebuildPending)
        FlushPendingRebuild();

    g_menuOpen = false;
    ClearBusyNotify();
    PopStreamingBudget();
}

void CClothesSystem::ToggleMenu() {
    if (g_menuOpen) CloseMenu();
    else            OpenMenu();
}

bool CClothesSystem::IsMenuOpen() { return g_menuOpen; }

void CClothesSystem::RenderMenu() {
    // Clothes UI is rendered by the Java NativeGui screen (NewUiList id 2).
    // Kept for source compatibility with existing ImGui render call sites.
}

// Runs once per frame on the game thread (via CJavaGui::ProcessClothesQueue).
void CClothesSystem::Process() {
#if defined(EAGLE_MODULAR_CHARACTERS)
    // Old Java/CEF requests must not rebuild player.img behind the new renderer.
    std::lock_guard<std::mutex> lock(g_cefMutex);
    g_cefCatalogRequest = false;
    g_cefStateQueue.clear();
    return;
#endif
    bool requestCatalog = false;
    std::deque<std::string> cefStates;
    {
        std::lock_guard<std::mutex> lock(g_cefMutex);
        requestCatalog = g_cefCatalogRequest;
        g_cefCatalogRequest = false;
        cefStates.swap(g_cefStateQueue);
    }

    if (requestCatalog)
        SendCatalogToJava();

    for (const auto& cefState : cefStates) {
        if (!ApplyCefStateOnGameThread(cefState)) {
            // A newly spawned player may still be waiting for model 0/CJ or a
            // remote ped may not be ready yet. Keep the last valid state and
            // retry on the next game frame instead of losing the selection.
            if (cefState.find("\"items\"") != std::string::npos) {
                std::lock_guard<std::mutex> lock(g_cefMutex);
                if (g_cefStateQueue.size() < kCefStateQueueMax)
                    g_cefStateQueue.emplace_back(cefState);
            }
        }
    }

    if (!g_initialized) {
        // Do not force a catalog probe here; Initialize() is driven by the
        // first /clothes or the first catalog request.
        return;
    }

    CPed* live = LivePlayerPed();

    // FIX-4: drop the cached pointer the moment the real player ped changes,
    // otherwise Reapply() writes into freed memory after a respawn.
    if (g_state.valid && g_state.ped != live) {
        g_state = PedState{};
        g_pendingModelSwitch = false;
        g_rebuildPending = false;
        g_rebuildUrgent = false;
        g_builtHashValid = false;
        ClearBusyNotify();
        return;
    }

    if (!live) return;

    // Preserve the queued appearance until the vehicle animation ends.
    if (IsVehicleTransition(live)) return;

    if (g_pendingModelSwitch && CStreaming::IsModelLoaded(0)) {
        if (EnsureCJ(live))
            Reapply();
        return;
    }

    if (g_state.valid && g_state.ped == live && !IsCJ(live)) {
        if (EnsureCJ(live))
            Reapply();
        return;
    }

    // ------------------------------------------------------------------
    //  Rebuild scheduler (PERF-1 .. PERF-4)
    // ------------------------------------------------------------------
    if (!g_rebuildPending)
        return;

    const uint64_t now = NowMs();

    // PERF-1: tunggu pemain berhenti menekan.
    if (!g_rebuildUrgent && (now - g_rebuildRequestMs) < QuietPeriodMs())
        return;

    // PERF-4: jarak minimum antar rebuild, adaptif terhadap berat player.img.
    if (g_rebuildLastEndMs != 0 && (now - g_rebuildLastEndMs) < MinIntervalMs())
        return;

    // PERF-3: jangan menumpuk pembacaan sinkron player.img di atas
    // pembacaan asinkron yang sedang berjalan. Ada batas tunggu supaya
    // rebuild tidak pernah kelaparan saat pemain berkendara.
    if (StreamingBusy() && (now - g_rebuildRequestMs) < kStreamWaitMaxMs)
        return;

    // PERF-2: kalau state akhir sama dengan yang sudah dibangun, batalkan.
    const uint64_t hash = HashState(g_state);
    if (g_builtHashValid && hash == g_builtHash) {
        g_rebuildPending = false;
        g_rebuildUrgent = false;
        NotifyBusy(false);
        return;
    }

    const uint64_t t0 = NowMs();
    const bool ok = BuildPlayerClothes(live, true);
    const uint64_t t1 = NowMs();

    g_lastCostMs = static_cast<uint32_t>(t1 - t0);
    g_rebuildLastEndMs = t1;
    g_rebuildPending = false;
    g_rebuildUrgent = false;

    if (ok) {
        g_builtHash = hash;
        g_builtHashValid = true;
    } else {
        // Gagal (mis. model CJ belum siap). Coba lagi nanti lewat jalur
        // terjadwal supaya tidak menjadi loop panas: g_rebuildRequestMs
        // di-reset sehingga periode tenang berlaku lagi.
        g_rebuildPending = true;
        g_rebuildRequestMs = t1;
    }

    if (g_lastCostMs >= 60) {
        Log("CClothesSystem: rebuild took %u ms (quiet=%u min=%u)",
            g_lastCostMs, QuietPeriodMs(), MinIntervalMs());
    }

    if (!g_rebuildPending)
        NotifyBusy(false);
}

// ---------------------------------------------------------------------------
//  Java bridge
// ---------------------------------------------------------------------------
bool CClothesSystem::SelectCatalogItem(int index) {
    if (index < 0 || index >= static_cast<int>(g_catalog.size())) return false;

    CPed* ped = LivePlayerPed();
    if (!ped) return false;

    const auto& item = g_catalog[static_cast<size_t>(index)];

    if (item.type == kCatalogHairColorType) {
        unsigned colorId = 0;
        if (std::sscanf(item.texture, "hair_color_%u", &colorId) != 1 ||
            colorId >= kHairColorCount)
            return false;
        const bool ok = SetHairColor(ped, static_cast<uint8_t>(colorId));
        if (ok) g_lastSelectedIndex = index;
        return ok;
    }

    if (item.type == kCatalogSkinColorType) {
        unsigned colorId = 0;
        if (std::sscanf(item.texture, "skin_color_%u", &colorId) != 1 ||
            colorId >= kSkinColorCount)
            return false;
        const bool ok = SetSkinColor(ped, static_cast<uint8_t>(colorId));
        if (ok) g_lastSelectedIndex = index;
        return ok;
    }

    // Tattoo entries use the same deferred native rebuild path. Their model
    // field is empty by design, so SetClothesByName writes only the texture
    // into the corresponding tattoo slot.

    // PERF-2: tap item yang sedang dipakai tidak perlu menyentuh apa pun.
    if (g_lastSelectedIndex == index && g_builtHashValid && !g_rebuildPending) {
        const auto* current = GetCurrent(item.type);
        if (current && current->active &&
            strcasecmp(current->texture, item.texture) == 0 &&
            strcasecmp(current->model, item.model) == 0) {
            return true;
        }
    }

    const bool result = SetClothesByName(
        ped,
        item.type,
        item.texture,
        item.model[0] ? item.model : nullptr,
        true);

    if (result) {
        g_lastSelectedIndex = index;
    }

    return result;
}

bool CClothesSystem::RemoveCatalogType(uint8_t type) {
    CPed* ped = LivePlayerPed();
    if (!ped) return false;
    if (g_lastSelectedIndex >= 0) {
        const auto* item = GetCatalogItem(g_lastSelectedIndex);
        if (item && item->type == type)
            g_lastSelectedIndex = -1;
    }
    if (type == kCatalogHairColorType)
        return SetHairColor(ped, 0);
    if (type == kCatalogSkinColorType)
        return SetSkinColor(ped, 0);
    return ClearClothesSlot(ped, type, true);
}

void CClothesSystem::SendCatalogStateToJava(int selectedIndex) {
    if (!g_menuOpen) return;

    int selectedType = -1;
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(g_catalog.size()))
        selectedType = static_cast<int>(g_catalog[static_cast<size_t>(selectedIndex)].type);

    const std::string json =
        "{\"type\":" + std::to_string(selectedType) +
        ",\"index\":" + std::to_string(selectedIndex) +
        ",\"busy\":" + std::string(g_rebuildPending ? "1" : "0") +
        ",\"cost\":" + std::to_string(g_lastCostMs) +
        ",\"skinColor\":" + std::to_string(GetSkinColor(LivePlayerPed())) +
        ",\"hairColor\":" + std::to_string(GetHairColor(LivePlayerPed())) + "}";

    CJavaGui::SendPacket(CJavaGui::UI_CLOTHES, CJavaGui::CLOTHES_SET_STATE, json);
}

// FIX-7: this schema is what ClothesMenu.parseCatalog() actually reads.
// Keys: store, type, index, typeName, name.
// PERF-6: string JSON dirakit sekali per muat katalog, lalu dipakai ulang.

    bool JsonIntInObject(const std::string& obj, const char* key, int& out) {
        const std::string needle = std::string("\"") + key + "\"";
        size_t p = obj.find(needle);
        if (p == std::string::npos) return false;
        p = obj.find(':', p + needle.size());
        if (p == std::string::npos) return false;
        ++p;
        while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p]))) ++p;
        char* end = nullptr;
        const long v = std::strtol(obj.c_str() + p, &end, 10);
        if (end == obj.c_str() + p) return false;
        out = static_cast<int>(v);
        return true;
    }

    bool JsonStringInObject(const std::string& obj, const char* key, std::string& out) {
        const std::string needle = std::string("\"") + key + "\"";
        size_t p = obj.find(needle);
        if (p == std::string::npos) return false;
        p = obj.find(':', p + needle.size());
        if (p == std::string::npos) return false;
        ++p;
        while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p]))) ++p;
        if (p >= obj.size() || obj[p] != '\"') return false;
        ++p;
        out.clear();
        bool escaped = false;
        for (; p < obj.size(); ++p) {
            char c = obj[p];
            if (escaped) {
                switch (c) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '\"': out.push_back('\"'); break;
                    default: out.push_back(c); break;
                }
                escaped = false;
                continue;
            }
            if (c == '\\') { escaped = true; continue; }
            if (c == '\"') return true;
            out.push_back(c);
        }
        return false;
    }

    size_t JsonObjectEnd(const std::string& json, size_t begin) {
        if (begin >= json.size() || json[begin] != '{') return std::string::npos;
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (size_t i = begin; i < json.size(); ++i) {
            const char c = json[i];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '\"') quoted = false;
                continue;
            }
            if (c == '\"') { quoted = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

void CClothesSystem::SendCatalogToJava() {
    Initialize();

    // This is only a legacy Java fallback. The real CEF wardrobe receives
    // its catalog from Pawn (clothes_catalog) and must never be overwritten
    // by an Android-side shopping.dat catalog.
    if (!g_catalogJsonValid) {
        std::string json;
        json.reserve(g_catalog.size() * 160 + 256);
        json += "{\"path\":\"";
        JsonEscapeAppend(json, g_catalogPath);
        json += "\",\"count\":";
        json += std::to_string(g_catalog.size());
        json += ",\"items\":[";

        for (size_t i = 0; i < g_catalog.size(); ++i) {
            if (i) json += ',';
            const auto& item = g_catalog[i];

            json += "{\"store\":\"";
            JsonEscapeAppend(json, GetShopName(item.shop));
            json += "\",\"type\":" + std::to_string(static_cast<unsigned>(item.type));
            json += ",\"index\":" + std::to_string(i);
            json += ",\"typeName\":\"";
            JsonEscapeAppend(json, GetTypeName(item.type));
            json += "\"";
            // Legacy Java needs texture/model identifiers to apply an item.
            // These were previously omitted, so the UI showed names but sent
            // empty texture/model strings to the native renderer.
            json += ",\"texture\":\"";
            JsonEscapeAppend(json, item.texture);
            json += "\",\"model\":\"";
            JsonEscapeAppend(json, item.model);
            json += "\"";
            if (item.type == kCatalogHairColorType || item.type == kCatalogSkinColorType) {
                unsigned colorId = 0;
                if (item.type == kCatalogHairColorType)
                    std::sscanf(item.texture, "hair_color_%u", &colorId);
                else
                    std::sscanf(item.texture, "skin_color_%u", &colorId);
                json += ",\"colorId\":" + std::to_string(colorId);
                json += ",\"colorKind\":" + std::to_string(item.type == kCatalogHairColorType ? 1 : 2);
            }
            json += ",\"name\":\"";
            JsonEscapeAppend(json, item.displayName[0] ? item.displayName : item.texture);
            json += "\",\"description\":\"";
            JsonEscapeAppend(json, IsTattooType(item.type)
                                     ? "Tattoo design"
                                     : "You can wear it however you like.");
            json += "\",\"price\":" + std::to_string(item.price);
            json += "}";
        }

        json += "]}";

        g_catalogJson.swap(json);
        g_catalogJsonValid = true;
    }

    // Do not publish this empty legacy catalog to CEF; Pawn is authoritative.
    CJavaGui::SendPacket(CJavaGui::UI_CLOTHES, CJavaGui::CLOTHES_SET_CATALOG, g_catalogJson);
    SendCatalogStateToJava(g_lastSelectedIndex);
}

void CClothesSystem::RequestCefCatalog() {
    std::lock_guard<std::mutex> lock(g_cefMutex);
    g_cefCatalogRequest = true;
}

bool CClothesSystem::HandleCefState(const std::string& payload) {
    if (payload.empty()) return false;
    std::lock_guard<std::mutex> lock(g_cefMutex);
    if (g_cefStateQueue.size() >= kCefStateQueueMax)
        g_cefStateQueue.pop_front();
    g_cefStateQueue.emplace_back(payload);
    return true;
}

bool CClothesSystem::ApplyCefStateOnGameThread(const std::string& payload) {
    if (payload.empty()) return false;

    int owner = -1;
    const bool hasOwner = JsonIntInObject(payload, "owner", owner);
    const int localId = static_cast<int>(CPlayerPool::GetLocalPlayerID());

    CPed* ped = nullptr;
    if (!hasOwner || owner == localId) {
        ped = LivePlayerPed();
    } else if (owner >= 0 && owner < MAX_PLAYERS) {
        CRemotePlayer* remote = CPlayerPool::GetSpawnedPlayer(static_cast<PLAYERID>(owner));
        if (remote && remote->GetPlayerPed())
            ped = remote->GetPlayerPed()->m_pPed;
    }

    std::array<CClothesSystem::ClothesItem, CClothesSystem::MAX_CLOTHES> items{};
    int hairColor = 0;
    int skinColor = 0;
    JsonIntInObject(payload, "hairColor", hairColor);
    JsonIntInObject(payload, "skinColor", skinColor);

    size_t itemsKey = payload.find("\"items\"");
    size_t arrayStart = itemsKey == std::string::npos ? std::string::npos : payload.find('[', itemsKey);
    if (arrayStart == std::string::npos) return false;

    size_t p = arrayStart + 1;
    while (p < payload.size()) {
        while (p < payload.size() && (std::isspace(static_cast<unsigned char>(payload[p])) || payload[p] == ',')) ++p;
        if (p >= payload.size() || payload[p] == ']') break;
        if (payload[p] != '{') { ++p; continue; }
        const size_t end = JsonObjectEnd(payload, p);
        if (end == std::string::npos) return false;
        const std::string obj = payload.substr(p, end - p + 1);

        int type = -1, active = 0;
        std::string texture, model;
        JsonIntInObject(obj, "t", type);
        JsonIntInObject(obj, "a", active);
        JsonStringInObject(obj, "x", texture);
        JsonStringInObject(obj, "m", model);

        if (type >= 0 && type < CClothesSystem::MAX_CLOTHES) {
            auto& item = items[static_cast<size_t>(type)];
            item.active = active != 0;
            CopyText(item.texture, sizeof(item.texture), texture.c_str());
            CopyText(item.model, sizeof(item.model), model.c_str());
        }
        p = end + 1;
    }

    // If a remote ped is not spawned yet, keep the outfit until its ped exists.
    if (!ped) {
        if (hasOwner && owner >= 0 && owner < MAX_PLAYERS) {
            CClothesSystem::StorePending(
                static_cast<uint16_t>(owner), items.data(), CClothesSystem::MAX_CLOTHES,
                static_cast<uint8_t>(std::clamp(hairColor, 0, static_cast<int>(kHairColorCount - 1))),
                static_cast<uint8_t>(std::clamp(skinColor, 0, static_cast<int>(kSkinColorCount - 1))));
            return true;
        }
        return false;
    }

    // ApplyNetworkState uses the single live descriptor state internally.
    // Preserve the local state while applying a remote player's appearance.
    const PedState backup = g_state;
    const bool remoteState = hasOwner && owner != localId;

    const bool ok = CClothesSystem::ApplyNetworkState(ped, items.data(), CClothesSystem::MAX_CLOTHES);
    if (ok) {
        // Colors are independent render-time state and must be applied after
        // the clothes descriptor so the descriptor cannot overwrite them.
        const uint8_t hc = static_cast<uint8_t>(std::clamp(hairColor, 0, static_cast<int>(kHairColorCount - 1)));
        const uint8_t sc = static_cast<uint8_t>(std::clamp(skinColor, 0, static_cast<int>(kSkinColorCount - 1)));
        CClothesSystem::SetHairColor(ped, hc);
        CClothesSystem::SetSkinColor(ped, sc);
    }

    if (remoteState)
        g_state = backup;

    return ok;
}

bool CClothesSystem::ApplyTattoo(CPed* ped, uint8_t slot,
                                   const char* textureName, bool rebuild) {
    if (!IsTattooType(slot)) return false;
    return ApplyTattooNative(ped, slot, textureName, rebuild);
}

bool CClothesSystem::RemoveTattoo(CPed* ped, uint8_t slot, bool rebuild) {
    if (!IsTattooType(slot)) return false;
    return ApplyTattooNative(ped, slot, nullptr, rebuild);
}

bool CClothesSystem::ApplyAllTattoos(CPed* ped, bool rebuild) {
    if (!ped || !Initialize() || !EnsureCJ(ped)) return false;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) {
        if (g_setup) g_setup(ped);
        if (!HasDesc(ped, &desc)) return false;
    }

    auto& state = StateFor(ped);
    for (uint8_t slot = CLOTHES_TATTOO_LEFT_UPPER_ARM;
         slot <= CLOTHES_TATTOO_LOWER_BACK; ++slot) {
        const auto& item = state.items[slot];
        SetDesc(desc, slot,
                item.active && item.texture[0] ? item.texture : nullptr,
                nullptr);
    }

    if (!rebuild) return true;
    if (ped == LivePlayerPed()) {
        ScheduleRebuild(false);
        return true;
    }
    return BuildPlayerClothes(ped, true);
}

bool CClothesSystem::GetNetworkState(CPed* ped, ClothesItem* outItems, int maxCount) {
    if (!ped || !outItems || maxCount < MAX_CLOTHES) return false;

    if (g_state.valid && g_state.ped == ped) {
        for (int i = 0; i < MAX_CLOTHES; ++i)
            outItems[i] = g_state.items[i];
        return true;
    }

    return false;
}


uint8_t GetHairColorForPed(CPed* ped) {
    if (!ped) return 0;
    const auto it = g_hairColorByPed.find(ped);
    if (it != g_hairColorByPed.end())
        return std::min<uint8_t>(it->second, static_cast<uint8_t>(kHairColorCount - 1));
    return ped == LivePlayerPed()
        ? std::min<uint8_t>(g_localHairColor, static_cast<uint8_t>(kHairColorCount - 1))
        : 0;
}

bool CClothesSystem::SetHairColor(CPed* ped, uint8_t colorId) {
    if (!ped || colorId >= kHairColorCount)
        return false;
    if (!Initialize() || !EnsureCJ(ped))
        return false;

    g_hairColorByPed[ped] = colorId;

    auto& state = StateFor(ped);
    state.hairColor = colorId;

    bool descriptorChanged = false;

    // If a compiled legacy variant exists use it; otherwise material tint is used.
    // This happens on the descriptor, so network/respawn state carries the
    // actual asset name instead of relying only on a local render tint.
    if (state.items[CLOTHES_HEAD].active &&
        IsHairTextureName(state.items[CLOTHES_HEAD].texture)) {
        char variantTexture[64]{};
        char variantModel[64]{};
        if (ResolveHairAssetForColor(
                state.items[CLOTHES_HEAD].texture, colorId,
                variantTexture, sizeof(variantTexture),
                variantModel, sizeof(variantModel))) {
            void* desc = nullptr;
            if (HasDesc(ped, &desc)) {
                SetDesc(desc, CLOTHES_HEAD, variantTexture, variantModel);
                StoreItem(state, CLOTHES_HEAD, variantTexture, variantModel);
                descriptorChanged = true;
            }
        }
    }

    if (ped == LivePlayerPed()) {
        g_localHairColor = colorId;

        // Actual variant changes need a rebuild; pure tint changes do not.
        // Keep the existing deferred rebuild pipeline so selecting colors
        // quickly cannot cause repeated player.img reads.
        if (descriptorChanged) {
            g_builtHashValid = false;
            ScheduleRebuild(false);
        } else {
            if (SkinColorForPed(ped) != 0 || colorId != 0)
                InstallColorRenderer(ped);
            else
                UninstallColorRenderer(ped);
            g_builtHashValid = false;
            NotifyBusy(false);
        }
    } else {
        if (descriptorChanged) {
            BuildPlayerClothes(ped, true);
        } else if (SkinColorForPed(ped) != 0 || colorId != 0) {
            InstallColorRenderer(ped);
        } else {
            UninstallColorRenderer(ped);
        }
    }

    return true;
}

bool CClothesSystem::ApplyNetworkHairColor(CPed* ped, uint8_t colorId) {
    return SetHairColor(ped, colorId);
}

uint8_t CClothesSystem::GetHairColor(CPed* ped) {
    return GetHairColorForPed(ped);
}

uint8_t CClothesSystem::GetNetworkHairColor(CPed* ped) {
    return GetHairColorForPed(ped);
}

int CClothesSystem::GetHairColorCount() {
    return static_cast<int>(kHairColorCount);
}

const char* CClothesSystem::GetHairColorName(uint8_t colorId) {
    if (colorId >= kHairColorCount) return "Unknown";
    return kHairColors[colorId].name;
}

bool CClothesSystem::SetSkinColor(CPed* ped, uint8_t colorId) {
    if (!ped || colorId >= kSkinColorCount)
        return false;
    if (!Initialize() || !EnsureCJ(ped))
        return false;

    g_skinColorByPed[ped] = colorId;
    if (ped == LivePlayerPed()) {
        g_localSkinColor = colorId;
        auto& state = StateFor(ped);
        state.skinColor = colorId;
        // Tinting is render-time; no player.img rebuild is necessary.
        if (colorId != 0 || GetHairColorForPed(ped) != 0)
            InstallColorRenderer(ped);
        else
            UninstallColorRenderer(ped);
        g_builtHashValid = false;
        NotifyBusy(false);
    } else {
        if (colorId != 0 || GetHairColorForPed(ped) != 0)
            InstallColorRenderer(ped);
        else
            UninstallColorRenderer(ped);
    }
    return true;
}

bool CClothesSystem::ApplyNetworkSkinColor(CPed* ped, uint8_t colorId) {
    if (!ped || colorId >= kSkinColorCount)
        return false;
    return SetSkinColor(ped, colorId);
}

uint8_t CClothesSystem::GetSkinColor(CPed* ped) {
    return SkinColorForPed(ped);
}

uint8_t CClothesSystem::GetNetworkSkinColor(CPed* ped) {
    return SkinColorForPed(ped);
}

int CClothesSystem::GetSkinColorCount() {
    return static_cast<int>(kSkinColorCount);
}

const char* CClothesSystem::GetSkinColorName(uint8_t colorId) {
    if (colorId >= kSkinColorCount) return "Unknown";
    return kSkinColors[colorId].name;
}

bool CClothesSystem::Set(uint8_t type, const char* textureName,
                         const char* modelName, bool rebuild) {
    CPed* ped = LivePlayerPed();
    if (!ped) return false;
    return SetClothesByName(ped, type, textureName, modelName, rebuild);
}

void CClothesSystem::Reapply() {
    if (!g_state.valid || !g_state.ped) return;

    CPed* ped = g_state.ped;
    if (ped != LivePlayerPed()) {
        g_state = PedState{};
        g_builtHashValid = false;
        return;
    }
    if (!EnsureCJ(ped)) return;

    void* desc = nullptr;
    if (!HasDesc(ped, &desc)) return;
    if (g_descInit) g_descInit(desc);

    for (uint8_t type = 0; type < MAX_CLOTHES; ++type) {
        const auto& item = g_state.items[type];
        if (!item.active) continue;
        SetDesc(desc, type, item.texture,
                item.model[0] ? item.model : nullptr);
    }

    // Descriptor sudah ditulis ulang di game, jadi hash lama tidak lagi
    // mencerminkan clump yang terpasang: paksa satu rebuild.
    g_builtHashValid = false;
    ScheduleRebuild(true);
}

void CClothesSystem::OnPlayerModelChanged() {
    CPed* ped = LivePlayerPed();
    if (!ped) return;
    if (g_state.valid && g_state.ped == ped)
        Reapply();
    if (SkinColorForPed(ped) != 0 || GetHairColorForPed(ped) != 0)
        InstallColorRenderer(ped);
    else
        UninstallColorRenderer(ped);
}

void SetupDefaultCJClothes(CPed* ped) {
    CClothesSystem::ResetToDefaultCJ(ped);
}
