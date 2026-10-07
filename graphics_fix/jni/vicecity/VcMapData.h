#pragma once
// The Vice City map as data: which models exist, where they stand. Generated into VcMapData.gen.cpp by
// tools/vc_convert.py from the SA-MP 0.3.DL filterscript of github.com/casualmind/samp-vice-city.
// No game dependencies.
#include <cstddef>
#include <cstdint>

namespace vc {

// Placement types of the script (MODEL_TYPES). The type decides how far away an object is created and drawn.
enum PlacementType : uint8_t {
    kTypeNone = 0,
    kTypeLandmass = 1,
    kTypeBuilding = 2,
    kTypeObject = 3,
    kTypeVegetation = 4,
    kTypeInterior = 5,
    kType2dfx = 6,
    kTypeCount = 7
};

struct TypeInfo {
    float drawDistance;   // the script streams an object in at drawDistance + 50
    uint8_t priority;     // higher = more important when not everything fits
    bool isStatic;        // the script keeps these streamed in for everyone, wherever they are
};

struct ModelDef {
    int32_t sampId;          // model id of the 0.3.DL script: negative, unique
    uint32_t ideFlags;       // IDE flags the script passes to AddSimpleModel's base model lookup
    const char* dff;         // file name, lower case
    uint16_t txd;            // index into kTxdFiles
    uint8_t timeOn;          // AddSimpleModelTimed hours; both 0 = not timed
    uint8_t timeOff;
    uint8_t flagsHaveBase;   // the script has a base model with exactly these flags (see EffectiveIdeFlags)
    float drawDistance;      // largest draw distance of the placements that use the model
    float box[6];            // min xyz, max xyz: the embedded collision joined with the geometry
};

struct Placement {
    // Negative: sampId of a map model; otherwise the id of a stock San Andreas model. The script places one
    // model it never defines (-1003): that placement is in the table too, so that the object a server sends
    // for it is recognised, and nothing is created for it.
    int32_t model;
    float pos[3];
    float rot[3];     // degrees, in the order SA-MP gives them to an object
    uint8_t type;     // PlacementType
};

struct MaterialDef {
    uint16_t placement;    // index into kPlacements
    uint8_t index;         // material index
    int32_t model;         // SetObjectMaterial arguments
    const char* txd;
    const char* texture;
    uint32_t color;
};

extern const TypeInfo kTypes[kTypeCount];
extern const char* const kTxdFiles[];
extern const size_t kTxdCount;
extern const ModelDef kModels[];
extern const size_t kModelCount;
extern const Placement kPlacements[];
extern const size_t kPlacementCount;
extern const MaterialDef kMaterials[];
extern const size_t kMaterialCount;

// IDE flag bits (San Andreas object definition flags).
constexpr uint32_t kIdeWetRoad = 0x1;
constexpr uint32_t kIdeDrawLast = 0x4;
constexpr uint32_t kIdeAdditive = 0x8;
constexpr uint32_t kIdeNoZWrite = 0x40;
constexpr uint32_t kIdeNoShadows = 0x80;
constexpr uint32_t kIdeGlass1 = 0x200;
constexpr uint32_t kIdeGlass2 = 0x400;
constexpr uint32_t kIdeGarageDoor = 0x800;
constexpr uint32_t kIdeDamageable = 0x1000;
constexpr uint32_t kIdeTree = 0x2000;
constexpr uint32_t kIdePalm = 0x4000;
constexpr uint32_t kIdeTag = 0x100000;
constexpr uint32_t kIdeNoBackfaceCull = 0x200000;
constexpr uint32_t kIdeBreakableStatue = 0x400000;

// Flags that only change how a model is drawn.
constexpr uint32_t kIdeRenderFlags =
    kIdeWetRoad | kIdeDrawLast | kIdeAdditive | kIdeNoZWrite | kIdeNoShadows | kIdeNoBackfaceCull;
// Flags that make the game treat an object as something special (glass that breaks, a garage door, ...).
constexpr uint32_t kIdeBehaviourFlags = kIdeGlass1 | kIdeGlass2 | kIdeGarageDoor | kIdeTree | kIdePalm | kIdeTag |
                                        kIdeBreakableStatue | 0x80000u;

// The IDE flags a model is registered with.
//
// On PC the script never hands the flags to the game: it picks a stock model that has exactly these flags
// (bit 0x20 ignored) as the base of AddSimpleModel, and the new model inherits the base model's flags. When no
// stock model matches, the base is a plain wall object. So: with a match the model gets its flags, including
// the special ones when `behaviour` is set; without a match it keeps only the flags that affect drawing, which
// is closer to what the model was made for than the wall's. A damageable model would need a second, damaged
// atomic, which these files do not have: that flag is never passed on.
constexpr uint32_t EffectiveIdeFlags(const ModelDef& m, bool behaviour) {
    const uint32_t flags = m.ideFlags & ~0x20u & ~kIdeDamageable;
    if (m.flagsHaveBase && behaviour) return flags;
    return flags & kIdeRenderFlags;
}

}  // namespace vc
