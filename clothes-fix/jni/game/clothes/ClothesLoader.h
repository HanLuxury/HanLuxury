#pragma once
#include "ClothesItem.h"
#include "../character/CharacterRenderWare.h"
#include "../character/CharacterCatalog.h"
#include <map>
namespace Eagle::Character {
struct AssetPart {uint16_t region=0;std::string material;};
struct ClothesAsset {
    // Clump is destroyed before texture leases; clones share its geometry.
    std::vector<TexturePtr> textures;
    ClumpPtr clump;
    std::map<const RpGeometry*,AssetPart> parts; // frame tag of each part, by shared geometry
    size_t memory=0;
};
class ClothesLoader {
public:
    ClothesLoader(std::string root,const CharacterCatalog& catalog):root_(std::move(root)),catalog_(catalog) {}
    ~ClothesLoader();
    std::shared_ptr<ClothesAsset> Load(const AssetDefinition& definition,const std::vector<uint8_t>& bytes,std::string& error);
    TexturePtr Texture(const std::string& name);
    static bool ValidateDff(const std::vector<uint8_t>& bytes,std::string& error);
    // Validated DFF only: frame names longer than the game keeps become "#<frame>"; full names in `names`.
    static bool ShortenFrameNames(std::vector<uint8_t>& bytes,std::vector<std::string>& names);
private:
    std::string root_;const CharacterCatalog& catalog_;
    RwTexDictionary* dictionary_=nullptr;
    std::map<std::string,std::weak_ptr<RwTexture>> textures_;
};
}
