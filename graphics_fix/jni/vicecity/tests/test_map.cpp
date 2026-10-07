// VcMap and VcMapData: the generated tables themselves, the folder index, which models are usable with the
// files that are there, the model ids, and the lookups for server objects.
#include "vctest.h"

#include <cstring>

#include <climits>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_set>

#include "../VcMap.h"

using namespace vc;

namespace {

// A folder with a (tiny) file for every model and TXD of the tables, in the spelling the repository uses for
// some of them: mixed case.
void FillFolder(const std::string& dir, bool mixedCase = true) {
    const std::string body(16, 'x');
    for (size_t j = 0; j < kTxdCount; ++j) {
        std::string name = kTxdFiles[j];
        if (mixedCase && j % 3 == 0) name[0] = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        mltest::WriteFile(dir + name, body);
    }
    for (size_t i = 0; i < kModelCount; ++i) {
        std::string name = kModels[i].dff;
        if (mixedCase && i % 5 == 0) {
            for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
        mltest::WriteFile(dir + name, body);
    }
}

}  // namespace

ML_TEST(tables_are_consistent) {
    ML_CHECK(kModelCount > 2000);
    ML_CHECK(kPlacementCount > 7000);
    ML_CHECK(kTxdCount > 500 && kTxdCount < 0xFFFF);

    std::set<int32_t> ids;
    std::set<std::string> dffNames, txdNames;
    for (size_t j = 0; j < kTxdCount; ++j) {
        const std::string name = kTxdFiles[j];
        ML_CHECK(name.size() > 4 && name.compare(name.size() - 4, 4, ".txd") == 0);
        for (char c : name) ML_CHECK(c == static_cast<char>(tolower(static_cast<unsigned char>(c))));
        ML_CHECK(txdNames.insert(name).second);
    }
    size_t timed = 0;
    for (size_t i = 0; i < kModelCount; ++i) {
        const ModelDef& m = kModels[i];
        ML_CHECK(m.sampId < 0);
        ML_CHECK(ids.insert(m.sampId).second);
        const std::string name = m.dff;
        ML_CHECK(name.size() > 4 && name.compare(name.size() - 4, 4, ".dff") == 0);
        for (char c : name) ML_CHECK(c == static_cast<char>(tolower(static_cast<unsigned char>(c))));
        ML_CHECK(dffNames.insert(name).second);
        ML_CHECK(m.txd < kTxdCount);
        ML_CHECK(m.timeOn < 24 && m.timeOff < 24);
        if (m.timeOn || m.timeOff) ++timed;
        ML_CHECK(m.drawDistance >= 100.0f && m.drawDistance <= 1500.0f);
        for (int k = 0; k < 3; ++k) {
            ML_CHECK(std::isfinite(m.box[k]) && std::isfinite(m.box[k + 3]));
            ML_CHECK(m.box[k] < m.box[k + 3]);
            ML_CHECK(m.box[k + 3] - m.box[k] < 2000.0f);
        }
        ML_CHECK(BoxReach(m.box) > 0.0f && BoxReach(m.box) < 1500.0f);
    }
    ML_CHECK_EQ(timed, static_cast<size_t>(184));   // what the script registers with AddSimpleModelTimed

    size_t mapModels = 0, stock = 0, undefined = 0;
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const Placement& p = kPlacements[k];
        ML_CHECK(p.type < kTypeCount);
        for (int a = 0; a < 3; ++a) ML_CHECK(std::isfinite(p.pos[a]) && std::isfinite(p.rot[a]));
        // Everything lies east of the game's own map, where objects need the world limit patch.
        ML_CHECK(p.pos[0] > 4000.0f && p.pos[0] < 7100.0f);
        ML_CHECK(p.pos[1] > -900.0f && p.pos[1] < 2700.0f);
        if (p.model < 0) {
            ++mapModels;
            if (ids.count(p.model) != 1) ++undefined;
        } else {
            ++stock;
            ML_CHECK(p.model < 20000);
        }
    }
    ML_CHECK(mapModels > 2000 && stock > 4000);
    // Line 4127 of the script places model -1003, which it never defines. The place is in the table (a server
    // may send an object for it); every other placement names a model the table has.
    ML_CHECK_EQ(undefined, static_cast<size_t>(1));

    for (size_t t = 0; t < kTypeCount; ++t) {
        ML_CHECK(kTypes[t].drawDistance >= 100.0f && kTypes[t].drawDistance <= 1500.0f);
        ML_CHECK(kTypes[t].priority >= 1);
    }
    ML_CHECK(kTypes[kTypeLandmass].isStatic);
    ML_CHECK(kTypes[kTypeLandmass].priority > kTypes[kTypeObject].priority);

    ML_CHECK_EQ(kMaterialCount, static_cast<size_t>(3));
    for (size_t k = 0; k < kMaterialCount; ++k) {
        ML_CHECK(kMaterials[k].placement < kPlacementCount);
        ML_CHECK(kPlacements[kMaterials[k].placement].model >= 0);   // stock objects with a material
        ML_CHECK(kMaterials[k].index < 16);
        ML_CHECK(strlen(kMaterials[k].txd) > 0 && strlen(kMaterials[k].texture) > 0);
    }
}

ML_TEST(ide_flags_follow_the_script) {
    ModelDef m {};
    m.flagsHaveBase = 1;
    m.ideFlags = 0x84;   // draw last + no shadows: the script has a base model with exactly these
    ML_CHECK_EQ(EffectiveIdeFlags(m, false), 0x84u);
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x84u);
    m.ideFlags = 0xA4;   // bit 0x20 is masked out by the script before it looks
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x84u);

    m.ideFlags = 0x404;  // glass: special behaviour only on request
    ML_CHECK_EQ(EffectiveIdeFlags(m, false), 0x4u);
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x404u);
    m.ideFlags = 0x800;  // garage door
    ML_CHECK_EQ(EffectiveIdeFlags(m, false), 0u);
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x800u);
    m.ideFlags = 0x1004; // damageable needs a second atomic: never
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x4u);

    m.flagsHaveBase = 0; // no base model with these flags: on PC the model falls back to a plain wall
    m.ideFlags = 0x484;
    ML_CHECK_EQ(EffectiveIdeFlags(m, true), 0x84u);   // the drawing flags are kept, the glass is not
    m.ideFlags = 0x20000C;
    ML_CHECK_EQ(EffectiveIdeFlags(m, false), 0x20000Cu);

    // What the map really uses.
    size_t withoutBase = 0, special = 0;
    for (size_t i = 0; i < kModelCount; ++i) {
        const uint32_t plain = EffectiveIdeFlags(kModels[i], false);
        const uint32_t full = EffectiveIdeFlags(kModels[i], true);
        ML_CHECK_EQ(plain & ~kIdeRenderFlags, 0u);
        ML_CHECK_EQ(full & kIdeDamageable, 0u);
        ML_CHECK_EQ(full & kIdeRenderFlags, plain);
        if (!kModels[i].flagsHaveBase) {
            ++withoutBase;
            ML_CHECK_EQ(full, plain);
        }
        if (full != plain) ++special;
    }
    ML_CHECK_EQ(withoutBase, static_cast<size_t>(63));
    ML_CHECK(special > 0 && special < 100);
}

ML_TEST(folder_index_finds_files_whatever_their_case) {
    mltest::TempDir dir;
    mltest::WriteFile(dir.path + "Hotel.DFF", "0123456789abcdef");
    mltest::WriteFile(dir.path + "hotel.txd", "0123");
    mltest::WriteFile(dir.path + "readme.txt", "");
    mltest::MakeDirs(dir.path + "sub.dff");               // a folder with a model's name
    ML_CHECK_EQ(symlink("/etc/passwd", (dir.path + "link.dff").c_str()), 0);

    FolderIndex index;
    ML_CHECK(index.Scan(dir.path));
    ML_CHECK_EQ(index.dir(), dir.path);
    FoundFile* f = index.Find("hotel.dff", true);
    ML_CHECK(f != nullptr);
    if (f) {
        ML_CHECK_EQ(f->size, static_cast<uint64_t>(16));
        ML_CHECK_EQ(f->path, dir.path + "Hotel.DFF");
    }
    ML_CHECK(index.Find("HOTEL.TXD") != nullptr);
    ML_CHECK(index.Find("nothere.dff") == nullptr);
    ML_CHECK(index.Find("sub.dff", true) == nullptr);     // not a file
    ML_CHECK(index.Find("link.dff", true) == nullptr);    // links are not followed out of the folder
    ML_CHECK_EQ(index.CountWithExtension(".txd"), static_cast<size_t>(1));

    // Without a trailing slash, and a folder that is not there.
    FolderIndex second;
    std::string noSlash = dir.path;
    noSlash.pop_back();
    ML_CHECK(second.Scan(noSlash));
    ML_CHECK_EQ(second.dir(), dir.path);
    ML_CHECK(second.Find("hotel.dff") != nullptr);
    FolderIndex missing;
    ML_CHECK(!missing.Scan(dir.path + "nope/"));
    ML_CHECK(missing.Find("hotel.dff") == nullptr);
}

ML_TEST(model_folder_is_found_in_the_layouts_people_copy) {
    {   // the files directly in the data folder
        mltest::TempDir dir;
        mltest::WriteFile(dir.path + "a.dff", "x");
        ML_CHECK_EQ(FindModelFolder(dir.path), dir.path);
    }
    {   // the repository's "models" folder copied as a whole
        mltest::TempDir dir;
        mltest::WriteFile(dir.path + "models/vice_city/a.dff", "x");
        mltest::WriteFile(dir.path + "models/minimap.txd", "x");
        ML_CHECK_EQ(FindModelFolder(dir.path), dir.path + "models/vice_city/");
    }
    {   // the "vice_city" folder copied into the data folder
        mltest::TempDir dir;
        mltest::WriteFile(dir.path + "vice_city/A.DFF", "x");
        ML_CHECK_EQ(FindModelFolder(dir.path), dir.path + "vice_city/");
    }
    {   // nothing yet
        mltest::TempDir dir;
        mltest::WriteFile(dir.path + "vice_city.ini", "x");
        ML_CHECK_EQ(FindModelFolder(dir.path), std::string());
    }
}

ML_TEST(map_with_every_file_is_fully_usable) {
    mltest::TempDir dir;
    FillFolder(dir.path);
    FolderIndex index;
    ML_CHECK(index.Scan(dir.path));
    Map map;
    map.Build(index);
    ML_CHECK_EQ(map.models().size(), kModelCount);
    ML_CHECK_EQ(map.txd().size(), kTxdCount);
    ML_CHECK_EQ(map.usableModels(), kModelCount);
    ML_CHECK(map.MissingFiles(10).empty());

    // Names inside the game: unique, short enough for the game's fixed fields, never a stock-looking name.
    std::unordered_set<std::string> names;
    for (const MapModel& m : map.models()) {
        ML_CHECK(strlen(m.name) == 7 && strncmp(m.name, "vcm", 3) == 0);
        ML_CHECK(names.insert(m.name).second);
        ML_CHECK(m.usable && m.dff && m.txd >= 0);
    }
    size_t users = 0;
    for (const MapTxd& t : map.txd()) {
        ML_CHECK(strlen(t.name) == 6 && strncmp(t.name, "vct", 3) == 0);
        ML_CHECK(names.insert(t.name).second);
        ML_CHECK(t.users > 0);   // the table lists only TXD files a model uses
        users += static_cast<size_t>(t.users);
    }
    ML_CHECK_EQ(users, kModelCount);

    for (size_t i = 0; i < kModelCount; i += 97) ML_CHECK_EQ(map.ModelBySampId(kModels[i].sampId), static_cast<int>(i));
    ML_CHECK_EQ(map.ModelBySampId(-1), -1);
    ML_CHECK_EQ(map.ModelBySampId(-1500), -1);
    ML_CHECK_EQ(map.ModelBySampId(1337), -1);
    size_t withoutModel = 0;
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const int m = map.ModelOfPlacement(k);
        if (kPlacements[k].model >= 0) {
            ML_CHECK_EQ(m, -1);
        } else if (m < 0) {
            ++withoutModel;   // the model the script never defines
            ML_CHECK_EQ(map.ModelBySampId(kPlacements[k].model), -1);
        } else {
            ML_CHECK_EQ(kModels[m].sampId, kPlacements[k].model);
        }
    }
    ML_CHECK_EQ(withoutModel, static_cast<size_t>(1));
}

ML_TEST(map_models_with_missing_files_are_left_out) {
    mltest::TempDir dir;
    FillFolder(dir.path, false);
    // One model file gone, one TXD gone (takes all its models with it), one model file empty.
    unlink((dir.path + kModels[10].dff).c_str());
    const size_t lostTxd = kModels[200].txd;
    unlink((dir.path + kTxdFiles[lostTxd]).c_str());
    mltest::WriteFile(dir.path + kModels[300].dff, "tiny");

    FolderIndex index;
    ML_CHECK(index.Scan(dir.path));
    Map map;
    map.Build(index);
    size_t expectLost = 0;
    for (size_t i = 0; i < kModelCount; ++i) {
        const bool lost = i == 10 || i == 300 || kModels[i].txd == lostTxd;
        if (lost) ++expectLost;
        ML_CHECK_EQ(map.models()[i].usable, !lost);
    }
    ML_CHECK_EQ(map.usableModels(), kModelCount - expectLost);
    const std::vector<std::string> missing = map.MissingFiles(1000);
    ML_CHECK_EQ(missing.size(), expectLost);
    ML_CHECK_EQ(map.MissingFiles(2).size(), static_cast<size_t>(2));
    bool namesTheTxd = false;
    for (const std::string& line : missing) namesTheTxd = namesTheTxd || line.find(kTxdFiles[lostTxd]) != std::string::npos;
    ML_CHECK(namesTheTxd);
    ML_CHECK_EQ(map.txd()[lostTxd].users, 0);

    // Model ids go to the usable models only, in table order.
    std::vector<int> ids;
    for (int id = 22000; id < 25000; ++id) ids.push_back(id);
    const size_t given = map.AssignIds(ids);
    ML_CHECK_EQ(given, kModelCount - expectLost);
    int last = -1;
    for (const MapModel& m : map.models()) {
        if (!m.usable) {
            ML_CHECK_EQ(m.gameId, -1);
            continue;
        }
        ML_CHECK(m.gameId > last);   // ascending, as the game looks its archive entries up
        last = m.gameId;
    }

    // Fewer ids than models: the first ones in the table get them.
    std::vector<int> few = {24990, 24991, 24992};
    ML_CHECK_EQ(map.AssignIds(few), static_cast<size_t>(3));
    size_t withId = 0;
    for (const MapModel& m : map.models()) withId += m.gameId >= 0 ? 1 : 0;
    ML_CHECK_EQ(withId, static_cast<size_t>(3));
    ML_CHECK_EQ(map.AssignIds({}), static_cast<size_t>(0));
}

ML_TEST(map_empty_folder_has_nothing_usable) {
    mltest::TempDir dir;
    FolderIndex index;
    ML_CHECK(index.Scan(dir.path));
    Map map;
    map.Build(index);
    ML_CHECK_EQ(map.usableModels(), static_cast<size_t>(0));
    ML_CHECK_EQ(map.MissingFiles(5).size(), static_cast<size_t>(5));
}

ML_TEST(server_objects_that_repeat_a_placement_are_recognised) {
    mltest::TempDir dir;
    FolderIndex index;
    index.Scan(dir.path);
    Map map;
    map.Build(index);   // the lookup does not depend on the files

    for (size_t k = 0; k < kPlacementCount; k += 7) {
        const Placement& p = kPlacements[k];
        const int found = map.FindPlacement(p.model, p.pos);
        ML_CHECK(found >= 0);
        if (found >= 0) {
            // The same model at the same place (the map has a few exact duplicates of its own).
            ML_CHECK_EQ(kPlacements[found].model, p.model);
            ML_CHECK(std::fabs(kPlacements[found].pos[0] - p.pos[0]) < 0.06f);
        }
        // The way a float travels through the server changes at most its last digits.
        const float nudged[3] = {p.pos[0] + 0.01f, p.pos[1] - 0.01f, p.pos[2] + 0.02f};
        ML_CHECK(map.FindPlacement(p.model, nudged) >= 0);
        // Another model at that place, or the model somewhere else, is the server's own object.
        ML_CHECK_EQ(map.FindPlacement(p.model == 1337 ? 1338 : 1337, p.pos), -1);
        const float moved[3] = {p.pos[0], p.pos[1], p.pos[2] + 0.5f};
        const int other = map.FindPlacement(p.model, moved);
        ML_CHECK(other == -1 || std::fabs(kPlacements[other].pos[2] - moved[2]) <= 0.051f);
    }

    // Right at a cell border, on either side of it.
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const Placement& p = kPlacements[k];
        const float frac = p.pos[0] - std::floor(p.pos[0]);
        if (frac > 0.04f) continue;
        const float left[3] = {p.pos[0] - 0.045f, p.pos[1], p.pos[2]};
        ML_CHECK(map.FindPlacement(p.model, left) >= 0);
        break;
    }

    const float nowhere[3] = {0.0f, 0.0f, 0.0f};
    ML_CHECK_EQ(map.FindPlacement(kPlacements[0].model, nowhere), -1);
    const float nan[3] = {std::nanf(""), 0.0f, 0.0f};
    ML_CHECK_EQ(map.FindPlacement(kPlacements[0].model, nan), -1);
    const float huge[3] = {1.0e30f, -1.0e30f, 0.0f};
    ML_CHECK_EQ(map.FindPlacement(kPlacements[0].model, huge), -1);
}

// open.mp sends an object with a custom model to a 0.3.7 client as model 18631 (the question mark) in the same
// place. Where the map has a model of its own, that is what such an object stands for.
ML_TEST(stand_ins_are_recognised_where_the_map_has_a_model_of_its_own) {
    ML_CHECK_EQ(kStandInModel, 18631);
    mltest::TempDir dir;
    FolderIndex index;
    index.Scan(dir.path);
    Map map;
    map.Build(index);   // the lookup does not depend on the files

    size_t own = 0, withoutModel = 0, stockOnly = 0, stockBeside = 0;
    for (size_t k = 0; k < kPlacementCount; ++k) {
        const Placement& p = kPlacements[k];
        const int found = map.FindStandIn(p.pos);
        if (found >= 0) {
            // Always one of the map's own models, the first one in the table at that place.
            ML_CHECK(kPlacements[found].model < 0);
            ML_CHECK(static_cast<size_t>(found) <= k || p.model >= 0);
            for (int a = 0; a < 3; ++a) ML_CHECK(std::fabs(kPlacements[found].pos[a] - p.pos[a]) <= 0.051f);
        }
        if (p.model < 0) {
            ++own;
            ML_CHECK(found >= 0);
            if (map.ModelOfPlacement(k) < 0) ++withoutModel;   // recognised all the same: nothing is shown there
            // The stand-in is not a placement of the map itself, and the second filing has no model a server
            // could name.
            ML_CHECK_EQ(map.FindPlacement(kStandInModel, p.pos), -1);
            ML_CHECK_EQ(map.FindPlacement(INT32_MIN, p.pos), -1);
            // The way a float travels through the server changes at most its last digits.
            const float nudged[3] = {p.pos[0] - 0.01f, p.pos[1] + 0.02f, p.pos[2] - 0.01f};
            ML_CHECK(map.FindStandIn(nudged) >= 0);
        } else if (found >= 0) {
            ++stockBeside;   // a stock object right where one of the map's own models stands
        } else {
            ++stockOnly;     // a question mark here would be the server's own object
        }
    }
    ML_CHECK(own > 2000);
    ML_CHECK_EQ(withoutModel, static_cast<size_t>(1));
    ML_CHECK(stockOnly > 4000);
    ML_CHECK(stockBeside < 20);

    // Half a metre above a model that has nothing above it: not a stand-in.
    size_t lonely = 0;
    for (size_t k = 0; k < kPlacementCount && lonely < 50; k += 11) {
        const Placement& p = kPlacements[k];
        if (p.model >= 0) continue;
        const float moved[3] = {p.pos[0], p.pos[1], p.pos[2] + 0.5f};
        const int other = map.FindStandIn(moved);
        if (other >= 0) {
            ML_CHECK(std::fabs(kPlacements[other].pos[2] - moved[2]) <= 0.051f);
            continue;
        }
        ++lonely;
    }
    ML_CHECK_EQ(lonely, static_cast<size_t>(50));

    const float nowhere[3] = {0.0f, 0.0f, 0.0f};
    ML_CHECK_EQ(map.FindStandIn(nowhere), -1);
    const float nan[3] = {0.0f, std::nanf(""), 0.0f};
    ML_CHECK_EQ(map.FindStandIn(nan), -1);
    const float huge[3] = {-1.0e30f, 1.0e30f, 0.0f};
    ML_CHECK_EQ(map.FindStandIn(huge), -1);
}

// The 0.3.7 filterscript tells the client that the server uses the map by creating objects that repeat
// placements of the map. Every CreateObject() in it has to be such a placement, to the digit, and with a
// stock San Andreas model: a client without the map must be able to show it.
ML_TEST(server_filterscript_beacons_are_placements_of_the_map) {
    std::string path = __FILE__;
    const size_t slash = path.find_last_of('/');
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "../server/vice_city_037.pwn";
    const std::string script = mltest::ReadFile(path);
    ML_CHECK(!script.empty());

    mltest::TempDir dir;
    FolderIndex index;
    index.Scan(dir.path);
    Map map;
    map.Build(index);

    size_t beacons = 0;
    size_t at = 0;
    while ((at = script.find("CreateObject(", at)) != std::string::npos) {
        at += strlen("CreateObject(");
        int model = 0;
        float v[7] = {};
        const int got = sscanf(script.c_str() + at, "%d , %f , %f , %f , %f , %f , %f , %f", &model, &v[0], &v[1], &v[2],
                               &v[3], &v[4], &v[5], &v[6]);
        ML_CHECK_EQ(got, 8);
        if (got != 8) continue;
        ++beacons;
        ML_CHECK(model >= 0 && model < 18631);   // a model of the game itself, not one SA-MP or the map adds
        const int k = map.FindPlacement(model, v);
        ML_CHECK(k >= 0);
        if (k < 0) continue;
        const Placement& p = kPlacements[k];
        ML_CHECK_EQ(p.model, model);
        for (int i = 0; i < 3; ++i) {
            ML_CHECK_EQ(p.pos[i], v[i]);       // the script's own digits
            ML_CHECK_EQ(p.rot[i], v[3 + i]);
        }
    }
    ML_CHECK_EQ(beacons, static_cast<size_t>(2));
}

ML_TEST(box_reach_is_the_farthest_corner) {
    const float box[6] = {-3.0f, -4.0f, 0.0f, 1.0f, 2.0f, 12.0f};
    ML_CHECK(std::fabs(BoxReach(box) - 13.0f) < 1.0e-4f);
    const float centred[6] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    ML_CHECK(std::fabs(BoxReach(centred) - std::sqrt(3.0f)) < 1.0e-4f);
}
