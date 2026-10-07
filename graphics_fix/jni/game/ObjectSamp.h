#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

#include "game/Core/Vector.h"
#include "game/RW/RenderWare.h"

// SA-MP addresses object materials with index 0..15.
static constexpr int OBJECT_MAX_MATERIALS = 16; 

enum : uint8_t {
    OBJECT_MATERIAL_SIZE_32X32 = 10,
    OBJECT_MATERIAL_SIZE_64X32 = 20,
    OBJECT_MATERIAL_SIZE_64X64 = 30,
    OBJECT_MATERIAL_SIZE_128X32 = 40,
    OBJECT_MATERIAL_SIZE_128X64 = 50,
    OBJECT_MATERIAL_SIZE_128X128 = 60,
    OBJECT_MATERIAL_SIZE_256X32 = 70,
    OBJECT_MATERIAL_SIZE_256X64 = 80,
    OBJECT_MATERIAL_SIZE_256X128 = 90,
    OBJECT_MATERIAL_SIZE_256X256 = 100,
    OBJECT_MATERIAL_SIZE_512X64 = 110,
    OBJECT_MATERIAL_SIZE_512X128 = 120,
    OBJECT_MATERIAL_SIZE_512X256 = 130,
    OBJECT_MATERIAL_SIZE_512X512 = 140
};

enum : uint8_t {
    OBJECT_MATERIAL_ALIGN_LEFT = 0,
    OBJECT_MATERIAL_ALIGN_CENTER = 1,
    OBJECT_MATERIAL_ALIGN_RIGHT = 2
};

enum : uint8_t {
    OBJECT_MATERIAL_NONE = 0,
    OBJECT_MATERIAL_TEXTURE = 1,
    OBJECT_MATERIAL_TEXT = 2
};

// One SetObjectMaterial / SetObjectMaterialText slot. Same layout and rules as
// the Alyn client (MATERIAL_OBJECT).
struct ObjectMaterialText {
    uint8_t  materialSize = OBJECT_MATERIAL_SIZE_256X256;
    char     fontName[33]{};
    uint8_t  fontSize = 24;
    uint8_t  fontBold = 0;
    uint32_t fontColor = 0xFFFFFFFF;
    uint32_t backgroundColor = 0;
    uint8_t  align = OBJECT_MATERIAL_ALIGN_CENTER;
    // Heap string (max 2048 chars): a fixed char[2049] in all 16 slots of every
    // object cost ~32 KB per SA-MP object even without any material.
    std::string text;
};

struct ObjectMaterialState {
    uint8_t            type = OBJECT_MATERIAL_NONE;
    // True only after the replacement texture has actually been obtained, or
    // when the slot intentionally has no replacement texture (modelid == -1).
    // A failed load remains pending and is retried from ProcessMaterialText().
    bool               textureWasCreated = false;
    // True while this material owns a streaming keep-alive request for its
    // source model. The source must remain resident while the RwTexture is
    // attached to a network object.
    bool               sourceModelPinned = false;
    ObjectMaterialText textInfo{};
    // One reference owned by this slot, released with RwTextureDestroy.
    RwTexture*         texture = nullptr;
    uint32_t           color = 0;
    // Bytes of the material-text render target held by this slot (budget).
    uint32_t           textBytes = 0;
    // Texture name lookups that failed: retried with a growing delay, then
    // given up (the model's own texture stays) instead of every frame.
    uint8_t            lookupFailures = 0;
    uint32_t           nextLookupTick = 0;

    // Parameters are retained so an object whose model/TXD is still streaming
    // can resolve the material later without losing the server's request.
    int32_t             modelId = -1;
    char                txdName[256]{};
    char                textureName[256]{};
};

enum class eObjectAttachType {
    NONE,
    TO_PLAYER,
    TO_VEHICLE,
};

class CObjectSamp
{
public:
    RwMatrix            m_matTarget;
    RwMatrix            m_matCurrent;
    bool                m_bIsMoving {false};
    float               m_fMoveSpeed {0.f};
    bool                m_bIsPlayerSurfing;
    bool                m_bNeedRotate;

    CQuaternion         m_quatTarget;
    CQuaternion         m_quatStart;

    CVector             m_vecAttachedOffset;
    CVector             m_vecAttachedRotation;
    uint16_t            m_usAttachedVehicle;
    eObjectAttachType   m_bAttachedType;

    CVector             m_vecRot;
    CVector             m_vecRotationTarget;
    CVector             m_vecSubRotationTarget;
    float               m_fDistanceToTargetPoint;
    uint32_t            m_iStartMoveTick;
    bool                bNeedReAttach = false;

    CPhysical*          m_pEntity;
    uint32_t            m_dwGTAId;
    // Model owned by this network object. Keeping this explicit prevents the
    // GTA streamer from treating the RW instance as an unowned world object.
    int32_t             m_iObjectModelId = -1;
    // True while we hold a GAME_REQUIRED request on m_iObjectModelId that
    // must be released once the RW object exists (or the object dies).
    bool                m_bModelPinned = false;

    static inline std::vector<CEntity*> objectToIdMap {};

    CObjectSamp(int iModel, float fPosX, float fPosY, float fPosZ, CVector vecRot, float fDrawDistance);
    ~CObjectSamp();

    // Lightweight validity check used by attachment/player code.
    // The GTA object pointer is the authoritative live-handle here; callers
    // still perform their own m_pEntity checks before dereferencing it.
    inline bool IsValid() const noexcept { return m_pEntity != nullptr; }

    void Process(float fElapsedTime);
    float DistanceRemaining(RwMatrix *matPos);

    // Keeps the GTA entity pointer, its RW instance and its model alive.
    // Called from Process() and for objects that are not in CObjectPool
    // (player attachments), which never get Process() and used to stay
    // invisible when their model was not resident at creation time.
    void ProcessStreaming();

    void SetPos(float x, float y, float z);
    void MoveTo(float x, float y, float z, float speed, float rX, float rY, float rZ);

    void AttachToVehicle(uint16_t usVehID, CVector* pVecOffset, CVector* pVecRot);
    void ProcessAttachToVehicle(CVehicleMP* pVehicle);

    void InstantRotate(float x, float y, float z);
    void StopMoving();

    void GetRotation(float* pfX, float* pfY, float* pfZ);
    void SetRot(float &radX, float &radY, float &radZ);

    // ---------------------------------------------------------------- material
    // Ported from the Alyn client (Game/Object.cpp).
    void SetMaterial(int modelId, uint8_t materialIndex, const char* txdName,
                     const char* textureName, uint32_t color);
    void SetMaterialText(uint8_t materialIndex, uint8_t materialSize, const char* fontName,
                         uint8_t fontSize, uint8_t fontBold, uint32_t fontColor,
                         uint32_t backgroundColor, uint8_t align, const char* text);
    void ProcessMaterialText();
    // Called once per game tick before ProcessMaterialText() of all objects.
    static void BeginMaterialTextTick();
    void ClearAllMaterials();

    // Around CObject::Render (see CObjectMaterialScope in hooks.cpp).
    void TryChangeToCustomObjectMaterial();
    void ChangeToCustomObjectMaterial();
    void ChangeToOriginalObjectMaterial();

    bool HasCustomMaterial() const { return m_bHasCustomObjectMaterial; }

    ObjectMaterialState m_Material[OBJECT_MAX_MATERIALS]{};

    // The same GTA model can contain multiple RpAtomic objects and each
    // atomic can point at a different RpGeometry/material list. Backing up
    // only one 0..15 texture array (the old implementation) causes the last
    // atomic to overwrite the backups for all previous atomics. That is what
    // made a second object's texture leak/restore onto another part of a
    // building. Backups are therefore kept per geometry for one render call.
    struct GeometryMaterialBackup {
        const void* geometry = nullptr;
        uint32_t flags = 0;
        uint8_t materialCount = 0;
        std::array<RwTexture*, OBJECT_MAX_MATERIALS> textures{};
        std::array<RwRGBA, OBJECT_MAX_MATERIALS> colors{};
        std::array<RwSurfaceProperties, OBJECT_MAX_MATERIALS> surfaceProps{};
    };
    std::vector<GeometryMaterialBackup> m_MaterialBackups;
    bool       m_bHasCustomObjectMaterial = false;

    // Objects with a custom material by GTA entity, for the render hook.
    static inline std::unordered_map<const void*, CObjectSamp*> ms_MaterialObjects{};
    static inline std::recursive_mutex ms_MaterialMutex{};
    static CObjectSamp* FindMaterialObject(const void* pGtaEntity);

private:
    const void* m_pMaterialRegKey = nullptr;
    void SyncMaterialRegistration();
};

// Applies the custom materials on construction and always puts the originals
// back on destruction, so no early return inside CObject::Render can leave a
// swapped texture behind on the model's shared geometry.
class CObjectMaterialScope
{
public:
    explicit CObjectMaterialScope(CObjectSamp* pObject) : m_pObject(pObject)
    {
        if (m_pObject) m_pObject->TryChangeToCustomObjectMaterial();
    }

    ~CObjectMaterialScope()
    {
        if (m_pObject) m_pObject->ChangeToOriginalObjectMaterial();
    }

    CObjectMaterialScope(const CObjectMaterialScope&) = delete;
    CObjectMaterialScope& operator=(const CObjectMaterialScope&) = delete;

private:
    CObjectSamp* m_pObject;
};
