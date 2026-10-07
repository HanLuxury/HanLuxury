#include "VcMap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <dirent.h>
#include <sys/stat.h>

namespace vc {
namespace {

constexpr uint64_t kMaxModelBytes = 32ull * 1024 * 1024;   // the game's streaming buffer takes nothing larger
constexpr float kSamePlace = 0.05f;                        // a server object this close to a placement repeats it
// Placements of the map's own models are filed a second time under this "model", for the lookup that does
// not know which model stands there (FindStandIn). No server object can have it: FindPlacement refuses it.
constexpr int32_t kAnyMapModel = std::numeric_limits<int32_t>::min();

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int32_t Cell(float v) { return static_cast<int32_t>(std::floor(v)); }

}  // namespace

bool FolderIndex::Scan(const std::string& dir) {
    m_files.clear();
    m_dir = dir;
    if (!m_dir.empty() && m_dir.back() != '/') m_dir += '/';
    DIR* d = opendir(m_dir.c_str());
    if (!d) return false;
    while (const dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        if (e->d_type == DT_DIR || e->d_type == DT_LNK) continue;
        FoundFile file;
        file.path = m_dir + e->d_name;
        m_files.emplace(Lower(e->d_name), std::move(file));   // two spellings of one name: the first one read wins
    }
    closedir(d);
    return true;
}

FoundFile* FolderIndex::Find(std::string_view name, bool needSize) {
    const auto it = m_files.find(Lower(name));
    if (it == m_files.end()) return nullptr;
    FoundFile& file = it->second;
    if (needSize && !file.sized) {
        struct stat st {};
        // lstat: a link is not followed out of the folder
        if (lstat(file.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            m_files.erase(it);
            return nullptr;
        }
        file.size = static_cast<uint64_t>(st.st_size);
        file.sized = true;
    }
    return &file;
}

size_t FolderIndex::CountWithExtension(std::string_view extension) const {
    size_t n = 0;
    for (const auto& entry : m_files) {
        if (EndsWith(entry.first, extension)) ++n;
    }
    return n;
}

std::string FindModelFolder(const std::string& root) {
    static const char* const kPlaces[] = {"", "vice_city/", "models/vice_city/", "models/"};
    for (const char* place : kPlaces) {
        FolderIndex index;
        const std::string dir = root + place;
        if (index.Scan(dir) && index.CountWithExtension(".dff") > 0) return dir;
    }
    return std::string();
}

float BoxReach(const float box[6]) {
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float side = std::max(std::fabs(box[i]), std::fabs(box[i + 3]));
        sum += side * side;
    }
    return std::sqrt(sum);
}

uint64_t Map::CellKey(int32_t model, int32_t cx, int32_t cy) {
    uint64_t h = static_cast<uint32_t>(model);
    h = h * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(cx);
    h = h * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(cy);
    return h ^ (h >> 29);
}

void Map::Build(FolderIndex& files) {
    m_models.assign(kModelCount, MapModel());
    m_txd.assign(kTxdCount, MapTxd());
    m_bySampId.clear();
    m_cells.clear();
    m_usable = 0;

    for (size_t j = 0; j < kTxdCount; ++j) {
        MapTxd& t = m_txd[j];
        t.file = files.Find(kTxdFiles[j], true);
        if (t.file && t.file->size < 12) t.file = nullptr;
        snprintf(t.name, sizeof(t.name), "vct%03u", static_cast<unsigned>(j));
    }
    m_bySampId.reserve(kModelCount);
    for (size_t i = 0; i < kModelCount; ++i) {
        MapModel& m = m_models[i];
        m.def = &kModels[i];
        m.txd = m.def->txd < kTxdCount ? static_cast<int>(m.def->txd) : -1;
        m.dff = files.Find(m.def->dff, true);
        if (m.dff && (m.dff->size < 12 || m.dff->size > kMaxModelBytes)) m.dff = nullptr;
        m.usable = m.dff && m.txd >= 0 && m_txd[static_cast<size_t>(m.txd)].file;
        snprintf(m.name, sizeof(m.name), "vcm%04u", static_cast<unsigned>(i));
        if (m.usable) {
            ++m_txd[static_cast<size_t>(m.txd)].users;
            ++m_usable;
        }
        m_bySampId.emplace(m.def->sampId, static_cast<uint32_t>(i));
    }

    m_placementModel.assign(kPlacementCount, -1);
    m_cells.reserve(kPlacementCount * 2);
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const Placement& p = kPlacements[k];
        const int32_t cx = Cell(p.pos[0]), cy = Cell(p.pos[1]);
        m_cells.emplace(CellKey(p.model, cx, cy), static_cast<uint32_t>(k));
        if (p.model < 0) {
            m_placementModel[k] = ModelBySampId(p.model);
            m_cells.emplace(CellKey(kAnyMapModel, cx, cy), static_cast<uint32_t>(k));
        }
    }
}

std::vector<std::string> Map::MissingFiles(size_t limit) const {
    std::vector<std::string> out;
    for (const MapModel& m : m_models) {
        if (m.usable) continue;
        if (out.size() >= limit) break;
        std::string line = m.def->dff;
        const char* txdName = m.txd >= 0 ? kTxdFiles[static_cast<size_t>(m.txd)] : "TXD";
        const bool txdMissing = m.txd < 0 || !m_txd[static_cast<size_t>(m.txd)].file;
        if (!m.dff && txdMissing) line += std::string(" (DFF dan ") + txdName + " tidak ada)";
        else if (!m.dff) line += " (DFF tidak ada, kosong, atau lebih dari 32 MB)";
        else line += std::string(" (") + txdName + " tidak ada)";
        out.push_back(std::move(line));
    }
    return out;
}

int Map::ModelBySampId(int32_t sampId) const {
    const auto it = m_bySampId.find(sampId);
    return it == m_bySampId.end() ? -1 : static_cast<int>(it->second);
}

size_t Map::AssignIds(const std::vector<int>& freeIds) {
    size_t next = 0;
    for (MapModel& m : m_models) {
        m.gameId = -1;
        if (!m.usable || next >= freeIds.size()) continue;
        m.gameId = freeIds[next++];
    }
    return next;
}

// The first placement in the table that is filed under `filedUnder` and lies at `pos`, or -1.
int Map::FindAt(int32_t filedUnder, const float pos[3]) const {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > 1.0e6f) return -1;
    }
    int best = -1;
    // A placement sits in one cell; a position right at a cell border may have been rounded into the next one.
    const int32_t x0 = Cell(pos[0] - kSamePlace), x1 = Cell(pos[0] + kSamePlace);
    const int32_t y0 = Cell(pos[1] - kSamePlace), y1 = Cell(pos[1] + kSamePlace);
    for (int32_t cx = x0; cx <= x1; ++cx) {
        for (int32_t cy = y0; cy <= y1; ++cy) {
            const auto range = m_cells.equal_range(CellKey(filedUnder, cx, cy));
            for (auto it = range.first; it != range.second; ++it) {
                const int k = static_cast<int>(it->second);
                if (best >= 0 && k >= best) continue;
                const Placement& p = kPlacements[it->second];
                // The key is a hash: what was found has to be looked at.
                const bool filedHere = filedUnder == kAnyMapModel ? p.model < 0 : p.model == filedUnder;
                if (filedHere && std::fabs(p.pos[0] - pos[0]) <= kSamePlace && std::fabs(p.pos[1] - pos[1]) <= kSamePlace &&
                    std::fabs(p.pos[2] - pos[2]) <= kSamePlace) {
                    best = k;
                }
            }
        }
    }
    return best;
}

int Map::FindPlacement(int32_t sampModel, const float pos[3]) const {
    if (sampModel == kAnyMapModel) return -1;
    return FindAt(sampModel, pos);
}

int Map::FindStandIn(const float pos[3]) const { return FindAt(kAnyMapModel, pos); }

}  // namespace vc
