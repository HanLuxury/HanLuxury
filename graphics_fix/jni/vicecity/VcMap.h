#pragma once
// The map as the client holds it at run time: the tables of VcMapData joined with the files that are really
// on the device, the model ids the game gave out, and the lookups the network code needs.
// No game dependencies.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "VcMapData.h"

namespace vc {

struct FoundFile {
    std::string path;
    uint64_t size = 0;
    bool sized = false;   // `size` has been asked from the file system
};

// The files of one folder by name, whatever their case. Not recursive.
class FolderIndex {
public:
    // False when the folder cannot be read. Symbolic links and sub-folders are left out.
    bool Scan(const std::string& dir);
    // nullptr when there is no such file. The size is looked up on the first call that asks for it.
    FoundFile* Find(std::string_view name, bool needSize = false);
    size_t CountWithExtension(std::string_view extension) const;   // ".dff"
    const std::string& dir() const { return m_dir; }               // with a trailing '/'
    size_t size() const { return m_files.size(); }

private:
    std::string m_dir;
    std::unordered_map<std::string, FoundFile> m_files;   // lower-case name
};

// The folder that holds the models: `root` itself, or one of the places a copy of the repository puts them
// below it ("vice_city/", "models/vice_city/", "models/"). "" when none of them has a .dff file.
// `root` ends with '/'.
std::string FindModelFolder(const std::string& root);

struct MapModel {
    const ModelDef* def = nullptr;
    FoundFile* dff = nullptr;   // nullptr: the file is not there
    int txd = -1;               // index into Map::txd()
    int gameId = -1;            // model id in the game, once it has one
    bool usable = false;        // both files are there
    char name[16] = {};         // name of the model inside the game ("vcm0000"): unique, never a stock name
};

struct MapTxd {
    FoundFile* file = nullptr;  // nullptr: the file is not there
    int slot = -1;              // CTxdStore slot, once it has one
    int users = 0;              // usable models that take their textures from it
    char name[16] = {};         // name of the TXD slot inside the game ("vct000")
};

class Map {
public:
    // Joins the tables with the files in `files` (kept by pointer: `files` must outlive the map).
    void Build(FolderIndex& files);

    std::vector<MapModel>& models() { return m_models; }
    const std::vector<MapModel>& models() const { return m_models; }
    std::vector<MapTxd>& txd() { return m_txd; }
    const std::vector<MapTxd>& txd() const { return m_txd; }
    size_t usableModels() const { return m_usable; }
    // Models whose files are missing, as "<dff> (<what is missing>)", at most `limit`.
    std::vector<std::string> MissingFiles(size_t limit) const;

    // Index of the model a 0.3.DL script calls `sampId`, or -1.
    int ModelBySampId(int32_t sampId) const;
    // The same for a placement's model: -1 for a stock San Andreas model, and for the one model the script
    // places without defining it.
    int ModelOfPlacement(size_t placement) const { return m_placementModel[placement]; }

    // Hands the ids in `freeIds` to the usable models, in table order, and returns how many got one. With
    // `freeIds` in ascending order the game finds the models of its archive in the order it looks for them.
    size_t AssignIds(const std::vector<int>& freeIds);

    // The placement of the map that a server object repeats (same model, same place), or -1. `sampModel` is
    // the model id as the server sends it. Of several such placements the first one in the table.
    int FindPlacement(int32_t sampModel, const float pos[3]) const;
    // A placement of one of the map's own models at `pos`, whatever the model: what an object with
    // kStandInModel at that place stands for. Several can share a place (the day and the night version of a
    // building, the parts of a block): the first one in the table. -1 when there is none.
    int FindStandIn(const float pos[3]) const;

private:
    static uint64_t CellKey(int32_t model, int32_t cx, int32_t cy);
    int FindAt(int32_t filedUnder, const float pos[3]) const;

    std::vector<MapModel> m_models;
    std::vector<MapTxd> m_txd;
    std::unordered_map<int32_t, uint32_t> m_bySampId;
    std::vector<int32_t> m_placementModel;
    std::unordered_multimap<uint64_t, uint32_t> m_cells;
    size_t m_usable = 0;
};

// What a server that cannot send the map's own models to this client puts in their place: open.mp sends an
// object with a custom (negative) model id to a 0.3.7 client as this model, the question mark, at the same
// position.
constexpr int32_t kStandInModel = 18631;

// How far a model reaches from its origin: the corner of its box that lies farthest away.
float BoxReach(const float box[6]);

}  // namespace vc
