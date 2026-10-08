//
// Holy SA v5.5 - CClothesSystem
//
// PERF 2026-09 (player.img lag fix):
//   Setiap kali pemain menyentuh satu item, jalur lama langsung memanggil
//   CClothes::RebuildPlayer(force=true). Rebuild itu melakukan pembacaan
//   SINKRON ke player.img di atas render thread. Karena CJavaGui menguras
//   sampai 8 aksi per frame, satu kali scroll cepat di wardrobe bisa memicu
//   8 pembacaan player.img dalam satu frame -> freeze.
//
//   File ini sekarang memisahkan "menulis descriptor" (murah, instan) dari
//   "membangun ulang clump" (mahal, baca IMG). Penulisan descriptor tetap
//   langsung; rebuild dijadwalkan, di-dedupe lewat hash state, dikunci
//   dengan interval minimum adaptif, dan ditunda selama streamer masih
//   sibuk. Hasilnya: berapa pun banyaknya tap, hanya ada SATU rebuild.
//
#pragma once

#include <cstdint>
#include <string>

struct CPed;

class CClothesSystem {
public:
    enum eClothesType : uint8_t {
        CLOTHES_SHIRT = 0,
        CLOTHES_HEAD = 1,
        CLOTHES_TROUSERS = 2,
        CLOTHES_SHOES = 3,
        CLOTHES_TATTOO_LEFT_UPPER_ARM = 4,
        CLOTHES_TATTOO_LEFT_LOWER_ARM = 5,
        CLOTHES_TATTOO_RIGHT_UPPER_ARM = 6,
        CLOTHES_TATTOO_RIGHT_LOWER_ARM = 7,
        CLOTHES_TATTOO_BACK = 8,
        CLOTHES_TATTOO_LEFT_CHEST = 9,
        CLOTHES_TATTOO_RIGHT_CHEST = 10,
        CLOTHES_TATTOO_STOMACH = 11,
        CLOTHES_TATTOO_LOWER_BACK = 12,
        CLOTHES_NECKLACE = 13,
        CLOTHES_WATCH = 14,
        CLOTHES_GLASSES = 15,
        CLOTHES_HAT = 16,
        CLOTHES_EXTRA = 17,
        MAX_CLOTHES = 18
    };

    struct ClothesItem {
        char texture[64]{};
        char model[64]{};
        bool active = false;
    };

    // Legacy Java catalog API. The active CEF catalog is owned by Pawn/server;
    // the native client never reads shopping.dat. Assets still come from player.img.
    enum eShop : uint8_t {
        SHOP_ALL = 0,
        SHOP_BINCO,
        SHOP_SUBURBAN,
        SHOP_PROLAPS,
        SHOP_ZIP,
        SHOP_VICTIM,
        SHOP_DIDIER_SACHS,
        SHOP_BARBER_1,
        SHOP_BARBER_2,
        SHOP_BARBER_3,
        SHOP_TATTOO_1,
        SHOP_TATTOO_2,
        SHOP_TATTOO_3,
        SHOP_UNIFORM,
        SHOP_SPECIAL,
        SHOP_MAX
    };

    struct CatalogItem {
        char texture[64]{};
        char model[64]{};
        char displayName[64]{};
        uint8_t type = 0;
        uint8_t shop = SHOP_SPECIAL;
        int price = 0;
    };

    static bool Initialize();
    static bool IsReady();
    static bool ReloadCatalog();
    static bool IsCatalogLoaded();
    static const char* GetCatalogPath();
    static int GetCatalogCount();
    static const CatalogItem* GetCatalogItem(int index);
    static bool SelectCatalogItem(int index);
    static bool RemoveCatalogType(uint8_t type);
    static void SendCatalogToJava();
    static void SendCatalogStateToJava(int selectedIndex);
    static bool HandleCefState(const std::string& payload);
    static void RequestCefCatalog();
    static bool ApplyCefStateOnGameThread(const std::string& payload);
    static void ClearNetworkState(uint16_t playerId);

    static bool SetClothesByName(CPed* ped, uint8_t type,
                                 const char* textureName,
                                 const char* modelName,
                                 bool rebuild = true);
    // Tattoo renderer: 9 native tattoo slots (4..12).
    static bool ApplyTattoo(CPed* ped, uint8_t slot,
                            const char* textureName, bool rebuild = true);
    static bool RemoveTattoo(CPed* ped, uint8_t slot, bool rebuild = true);
    static bool ApplyAllTattoos(CPed* ped, bool rebuild = true);
    static bool ClearClothesSlot(CPed* ped, uint8_t type, bool rebuild = true);
    static bool ApplyClothesData(CPed* ped, const ClothesItem* items, int count);
    static bool ApplyStoredClothes(CPed* ped);
    static void ClearPed(CPed* ped);
    // Around a skin change that destroys the clump: callbacks come off the old clump first, and the
    // stored skin/hair colours go back on the new one. Outfit and colour state are kept.
    static void BeforeClumpReplaced(CPed* ped);
    static void AfterClumpReplaced(CPed* ped);

    static bool BuildPlayerClothes(CPed* ped, bool force = true);
    static bool ResetToDefaultCJ(CPed* ped);

    static bool SetClothesComponent(CPed* ped, uint8_t type,
                                    uint32_t drawable, uint32_t texture);
    static bool ApplyNetworkState(CPed* ped, const ClothesItem* items, int count);
    // Copies all 18 slots (including the 9 tattoo slots) for network RPC.
    static bool GetNetworkState(CPed* ped, ClothesItem* outItems, int maxCount);

    // Skin and hair colors are render-only per-ped state, kept separate from
    // the 18 clothes descriptor slots. The Java UI exposes them as synthetic
    // catalog entries and the network layer can serialize each as one byte.
    static bool SetHairColor(CPed* ped, uint8_t colorId);
    static bool ApplyNetworkHairColor(CPed* ped, uint8_t colorId);
    static uint8_t GetHairColor(CPed* ped);
    static uint8_t GetNetworkHairColor(CPed* ped);
    static int GetHairColorCount();
    static const char* GetHairColorName(uint8_t colorId);

    static bool SetSkinColor(CPed* ped, uint8_t colorId);
    static bool ApplyNetworkSkinColor(CPed* ped, uint8_t colorId);
    static uint8_t GetSkinColor(CPed* ped);
    static uint8_t GetNetworkSkinColor(CPed* ped);
    static int GetSkinColorCount();
    static const char* GetSkinColorName(uint8_t colorId);

    static void StorePending(uint16_t playerId, const ClothesItem* items, int count,
                             uint8_t hairColor = 0, uint8_t skinColor = 0);
    static bool ApplyPending(uint16_t playerId, CPed* ped);

    static const char* GetTypeName(uint8_t type);
    static const char* GetShopName(uint8_t shop);
    static const ClothesItem* GetCurrent(uint8_t type);

    static bool Set(uint8_t type, const char* textureName,
                    const char* modelName, bool rebuild = true);
    static void Reapply();
    static void OnPlayerModelChanged();
    static bool HasStoredClothes(CPed* ped);

    // ---------------------------------------------------------------
    //  Deferred rebuild pipeline (player.img lag fix)
    // ---------------------------------------------------------------
    // ScheduleRebuild() hanya menandai "state berubah". Rebuild sebenarnya
    // dijalankan oleh Process() pada game thread setelah:
    //   - periode tenang (quiet period) terlewati, dan
    //   - interval minimum sejak rebuild terakhir terpenuhi, dan
    //   - CStreaming tidak sedang punya request tertunda.
    // urgent = true melewati periode tenang, tapi TETAP menghormati
    // interval minimum dan gerbang streaming.
    static void ScheduleRebuild(bool urgent = false);

    // Paksa rebuild yang tertunda dieksekusi sekarang juga (dipakai saat
    // menu ditutup, supaya pemain tidak keluar dengan baju setengah jadi).
    static bool FlushPendingRebuild();

    static bool IsRebuildPending();

    // Durasi rebuild terakhir dalam milidetik. Dipakai untuk mengukur
    // seberapa berat player.img pada perangkat ini dan menyetel ulang
    // periode tenang secara adaptif.
    static uint32_t GetLastRebuildCostMs();

    // Naikkan/kembalikan anggaran streaming saat wardrobe dibuka supaya
    // geometri baju yang baru dibaca tidak langsung dibuang oleh
    // CStreaming::MakeSpaceFor() dan harus dibaca ulang dari player.img.
    static void PushStreamingBudget();
    static void PopStreamingBudget();

    // Java-hosted wardrobe (NewUiList id 2 -> ClothesMenu).
    static void RenderMenu();
    static void Process();
    static void ToggleMenu();
    static void OpenMenu();
    static void CloseMenu();
    static void OnMenuClosed();   // Java confirmed teardown (ACTION_ONCLOSED)
    static bool IsMenuOpen();
};

void SetupDefaultCJClothes(CPed* ped);
