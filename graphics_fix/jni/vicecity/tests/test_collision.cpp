// VcCollision: finding SA-MP's collision chunk in a DFF, and deciding whether the game's COL3 loader can be
// given the file. The loader is simulated here from its disassembly (CFileLoader::LoadCollisionModelVer3 and
// the users of its result in libGTASA.so 2.10 arm64): whatever CheckCol3() lets through must not make it read
// outside the file.
#include "vctest.h"

#include <random>

#include "../VcCollision.h"

using namespace vc;
using namespace vctest;

namespace {

FileSource MemorySource(const std::string& bytes) {
    FileSource source;
    source.size = bytes.size();
    source.read = [&bytes](uint64_t offset, uint8_t* dst, size_t n) {
        if (offset > bytes.size() || n > bytes.size() - offset) return false;
        memcpy(dst, bytes.data() + offset, n);
        return true;
    };
    return source;
}

uint32_t RdU16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint32_t RdU32(const uint8_t* p) { return RdU16(p) | RdU16(p + 2) << 16; }
int RdI16(const uint8_t* p) { return static_cast<int16_t>(RdU16(p)); }

// Every byte range the game touches for a COL3 file of `size` bytes, checked against the file. The loader
// keeps its own copy of everything behind the info block, and the pointers it builds from the offsets land in
// that copy at the same distance from the start as in the file.
bool GameWouldSurvive(const uint8_t* file, size_t size, std::string& why) {
    const size_t dataStart = kColFileHeader + kCol3Info;
    if (size < dataStart) {
        why = "shorter than the head";
        return false;
    }
    if (size == dataStart) return true;   // dataSize == 0x58: nothing is allocated
    const uint8_t* info = file + kColFileHeader;
    const int spheres = RdI16(info + 0x28), boxes = RdI16(info + 0x2A), faces = RdI16(info + 0x2C);
    const int lines = info[0x2E];
    const uint32_t flags = RdU32(info + 0x30);
    const uint32_t offSpheres = RdU32(info + 0x34), offBoxes = RdU32(info + 0x38), offLines = RdU32(info + 0x3C);
    const uint32_t offVertices = RdU32(info + 0x40), offFaces = RdU32(info + 0x44);
    const int shadowFaces = static_cast<int32_t>(RdU32(info + 0x4C));
    const uint32_t offShadowVertices = RdU32(info + 0x50), offShadowFaces = RdU32(info + 0x54);

    const auto inside = [&](uint32_t off, uint64_t bytes) {
        const uint64_t at = static_cast<uint64_t>(off) + 4;
        return at >= dataStart && at <= size && bytes <= size - at;
    };
    if (spheres < 0 || boxes < 0 || faces < 0) {
        why = "negative count";
        return false;
    }
    if (spheres > 0 && (offSpheres == 0 || !inside(offSpheres, static_cast<uint64_t>(spheres) * 20))) {
        why = "spheres";
        return false;
    }
    if (boxes > 0 && (offBoxes == 0 || !inside(offBoxes, static_cast<uint64_t>(boxes) * 28))) {
        why = "boxes";
        return false;
    }
    if (lines > 0 && (offLines == 0 || !inside(offLines, static_cast<uint64_t>(lines) * 32))) {
        why = "lines";
        return false;
    }
    // Triangles are expanded into an array of their own when there is an offset.
    int highest = -1;
    if (faces > 0) {
        if (offFaces == 0 || !inside(offFaces, static_cast<uint64_t>(faces) * 8)) {
            why = "faces";
            return false;
        }
        for (int i = 0; i < faces; ++i) {
            const uint8_t* f = file + offFaces + 4 + static_cast<size_t>(i) * 8;
            highest = std::max(highest, static_cast<int>(std::max(RdU16(f), std::max(RdU16(f + 2), RdU16(f + 4)))));
        }
    }
    // GetNoVectors(): the largest index + 1, or 1 without triangles.
    if (offVertices != 0) {
        const int vertices = faces > 0 ? highest + 1 : 1;
        if (!inside(offVertices, static_cast<uint64_t>(vertices) * 6)) {
            why = "vertices";
            return false;
        }
    } else if (faces > 0) {
        why = "triangles without vertices";
        return false;
    }
    if (offShadowFaces != 0 && shadowFaces > 0 && !inside(offShadowFaces, static_cast<uint64_t>(shadowFaces) * 8)) {
        why = "shadow faces";
        return false;
    }
    if (offShadowVertices != 0) {
        // GetNoShadVectors() walks the expanded shadow triangles, which exist only with an offset.
        if (shadowFaces > 0 && offShadowFaces == 0) {
            why = "shadow vertices without shadow faces";
            return false;
        }
        if (!inside(offShadowVertices, 6)) {
            why = "shadow vertices";
            return false;
        }
    }
    // Sections: a count in front of the triangles, the sections in front of the count, read backwards; the
    // two triangle numbers are signed.
    if (flags & 8) {
        if (offFaces < dataStart) {
            why = "sections without triangles";
            return false;
        }
        const int count = static_cast<int32_t>(RdU32(file + offFaces));
        for (int i = 0; i < count; ++i) {
            const uint64_t at = static_cast<uint64_t>(offFaces) - 28ull * static_cast<uint64_t>(i + 1);
            if (28ull * static_cast<uint64_t>(i + 1) > offFaces || at < dataStart) {
                why = "section table";
                return false;
            }
            const int first = RdI16(file + at + 24), last = RdI16(file + at + 26);
            if (first <= last && (first < 0 || last >= faces)) {
                why = "section range";
                return false;
            }
        }
    }
    return true;
}

ColSpec TypicalSpec() {
    ColSpec spec;
    spec.spheres = 2;
    spec.boxes = 3;
    spec.vertices = 6;
    spec.faces = {{0, 1, 2, 4, 0}, {2, 3, 4, 9, 0}, {3, 4, 5, 51, 0}, {0, 4, 5, 0, 0}};
    spec.groups = {{0, 1}, {2, 3}};
    spec.flags = 2 | 8;
    spec.padAfterVertices = 0;
    return spec;
}

ColResult Check(std::string& file, Collision& out) {
    size_t size = file.size();
    const ColResult result = CheckCol3(reinterpret_cast<uint8_t*>(file.data()), size, out);
    if (result == ColResult::Loadable) file.resize(size);
    return result;
}

}  // namespace

ML_TEST(col_valid_file_is_loadable_and_counted) {
    std::string file = BuildCol3(TypicalSpec());
    Collision out;
    ML_CHECK(Check(file, out) == ColResult::Loadable);
    ML_CHECK_EQ(out.spheres, 2u);
    ML_CHECK_EQ(out.boxes, 3u);
    ML_CHECK_EQ(out.faces, 4u);
    ML_CHECK_EQ(out.vertices, 6u);
    ML_CHECK_EQ(out.faceGroups, 2u);
    ML_CHECK(out.hasBox);
    ML_CHECK_EQ(out.box[0], -1.0f);
    ML_CHECK_EQ(out.box[5], 6.0f);
    std::string why;
    ML_CHECK(GameWouldSurvive(reinterpret_cast<const uint8_t*>(file.data()), file.size(), why));
}

ML_TEST(col_padding_between_vertices_and_groups_is_fine) {
    ColSpec spec = TypicalSpec();
    spec.vertices = 7;
    spec.padAfterVertices = 2;   // what the map's files have when the vertex data does not end on four bytes
    std::string file = BuildCol3(spec);
    Collision out;
    ML_CHECK(Check(file, out) == ColResult::Loadable);
    ML_CHECK_EQ(out.faceGroups, 2u);
}

ML_TEST(col_without_volumes_is_empty) {
    ColSpec spec;
    spec.flags = 0;
    std::string file = BuildCol3(spec);
    ML_CHECK_EQ(file.size(), static_cast<size_t>(0x78));
    Collision out;
    ML_CHECK(Check(file, out) == ColResult::Empty);
    ML_CHECK(out.hasBox);

    // The flag says "empty": whatever follows is not handed to the game.
    ColSpec flagged = TypicalSpec();
    flagged.flags = 8;
    std::string file2 = BuildCol3(flagged);
    ML_CHECK(Check(file2, out) == ColResult::Empty);

    // Volumes promised, none there.
    ColSpec hollow;
    hollow.flags = 2;
    std::string file3 = BuildCol3(hollow);
    ML_CHECK(Check(file3, out) == ColResult::Empty);
}

ML_TEST(col_shadow_mesh_and_lines_are_dropped) {
    ColSpec spec = TypicalSpec();
    spec.shadow = true;
    std::string file = BuildCol3(spec);
    file[0x20 + 0x2E] = 3;                 // lines the file has no data for
    Set32(file, 0x20 + 0x3C, 0x7FFFFFF0);  // ... at an offset far outside
    Set32(file, 0x20 + 0x48, 0x7FFFFFF0);  // plane table
    Collision out;
    ML_CHECK(Check(file, out) == ColResult::Loadable);
    ML_CHECK_EQ(static_cast<int>(file[0x20 + 0x2E]), 0);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x3C), 0u);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x48), 0u);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x4C), 0u);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x50), 0u);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x54), 0u);
    ML_CHECK_EQ(Get32(file, 0x20 + 0x30), 2u | 8u);   // the shadow flag is gone too
    std::string why;
    ML_CHECK(GameWouldSurvive(reinterpret_cast<const uint8_t*>(file.data()), file.size(), why));
}

ML_TEST(col_broken_face_groups_are_switched_off_not_fatal) {
    {   // a group that names a triangle that does not exist
        ColSpec spec = TypicalSpec();
        spec.groups = {{0, 1}, {2, 9}};
        std::string file = BuildCol3(spec);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Loadable);
        ML_CHECK_EQ(out.faceGroups, 0u);
        ML_CHECK_EQ(Get32(file, 0x20 + 0x30), 2u);
    }
    {   // first > last
        ColSpec spec = TypicalSpec();
        spec.groups = {{3, 1}};
        std::string file = BuildCol3(spec);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Loadable);
        ML_CHECK_EQ(out.faceGroups, 0u);
    }
    {   // the flag without any group data: the "count" is whatever lies in front of the triangles
        ColSpec spec = TypicalSpec();
        spec.groups.clear();
        std::string file = BuildCol3(spec);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Loadable);
        ML_CHECK_EQ(out.faceGroups, 0u);
        std::string why;
        ML_CHECK(GameWouldSurvive(reinterpret_cast<const uint8_t*>(file.data()), file.size(), why));
    }
    {   // a count larger than what fits in front of the triangles
        ColSpec spec = TypicalSpec();
        std::string file = BuildCol3(spec);
        const uint32_t offFaces = Get32(file, 0x20 + 0x44);
        Set32(file, offFaces, 1000);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Loadable);
        ML_CHECK_EQ(out.faceGroups, 0u);
    }
}

ML_TEST(col_unknown_surfaces_become_default) {
    ColSpec spec = TypicalSpec();
    spec.faces[1].material = 200;
    std::string file = BuildCol3(spec);
    const uint32_t offFaces = Get32(file, 0x20 + 0x44);
    const uint32_t offSpheres = Get32(file, 0x20 + 0x34);
    const uint32_t offBoxes = Get32(file, 0x20 + 0x38);
    file[offSpheres + 4 + 16] = static_cast<char>(250);
    file[offBoxes + 4 + 28 + 24] = static_cast<char>(179);
    Collision out;
    ML_CHECK(Check(file, out) == ColResult::Loadable);
    ML_CHECK_EQ(static_cast<int>(file[offFaces + 4 + 8 + 6]), 0);
    ML_CHECK_EQ(static_cast<int>(file[offFaces + 4 + 6]), 4);   // a known one is left alone
    ML_CHECK_EQ(static_cast<int>(file[offSpheres + 4 + 16]), 0);
    ML_CHECK_EQ(static_cast<int>(file[offBoxes + 4 + 28 + 24]), 0);
    ML_CHECK_EQ(static_cast<int>(file[offBoxes + 4 + 24]), 9);
}

ML_TEST(col_out_of_range_data_is_rejected) {
    const auto rejected = [](size_t field, uint32_t value) {
        std::string file = BuildCol3(TypicalSpec());
        Set32(file, 0x20 + field, value);
        Collision out;
        return Check(file, out) == ColResult::Bad;
    };
    ML_CHECK(rejected(0x34, 0x7FFFFFF0));   // spheres far outside
    ML_CHECK(rejected(0x34, 0x10));         // spheres inside the head
    ML_CHECK(rejected(0x34, 0));            // spheres promised, no offset
    ML_CHECK(rejected(0x38, 0xFFFFFFFF));   // boxes: offset + 4 wraps in 32 bits
    ML_CHECK(rejected(0x38, 0));
    ML_CHECK(rejected(0x44, 0x7FFFFFF0));   // triangles
    ML_CHECK(rejected(0x44, 0));
    ML_CHECK(rejected(0x40, 0x7FFFFFF0));   // vertices
    ML_CHECK(rejected(0x40, 0));

    {   // one sphere more than the file holds
        ColSpec spec;
        spec.spheres = 2;
        std::string file = BuildCol3(spec);
        file[0x20 + 0x28] = 3;
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
    {   // a triangle that uses a vertex behind the end of the file
        ColSpec spec = TypicalSpec();
        spec.groups.clear();
        spec.flags = 2;
        spec.spheres = spec.boxes = 0;
        spec.faces = {{0, 1, 60000, 0, 0}};
        std::string file = BuildCol3(spec);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
    {   // counts the loader would read as negative
        std::string file = BuildCol3(TypicalSpec());
        file[0x20 + 0x2D] = static_cast<char>(0x80);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
    {   // bounds that are not numbers
        std::string file = BuildCol3(TypicalSpec());
        Set32(file, 0x20 + 8, 0x7FC00000);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
    {   // a sphere with an infinite radius
        std::string file = BuildCol3(TypicalSpec());
        Set32(file, Get32(file, 0x20 + 0x34) + 4 + 12, 0x7F800000);
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
}

ML_TEST(col_size_field_decides_how_much_is_looked_at) {
    {   // trailing bytes behind the declared end are cut off
        std::string file = BuildCol3(TypicalSpec());
        const size_t real = file.size();
        file += std::string(40, '\x7F');
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Loadable);
        ML_CHECK_EQ(file.size(), real);
    }
    {   // the file claims more than there is
        std::string file = BuildCol3(TypicalSpec());
        Set32(file, 4, static_cast<uint32_t>(file.size()));
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
    {   // every truncation of a valid file is rejected or still safe
        const std::string full = BuildCol3(TypicalSpec());
        for (size_t cut = 0; cut < full.size(); ++cut) {
            std::string file = full.substr(0, cut);
            Collision out;
            const ColResult result = Check(file, out);
            ML_CHECK(result == ColResult::Bad);
        }
    }
    {   // not a COL3 at all
        std::string file = BuildCol3(TypicalSpec());
        file[3] = '2';
        Collision out;
        ML_CHECK(Check(file, out) == ColResult::Bad);
    }
}

ML_TEST(col_mutated_files_never_pass_unsafe) {
    // Whatever is done to the head of a file, what passes must still be something the loader survives.
    std::mt19937 rng(20241006);
    ColSpec spec = TypicalSpec();
    spec.vertices = 9;
    spec.padAfterVertices = 2;
    const std::string original = BuildCol3(spec);
    int passed = 0, rejected = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string file = original;
        const int edits = 1 + static_cast<int>(rng() % 3);
        for (int e = 0; e < edits; ++e) {
            // mostly the info block (counts, flags, offsets), sometimes anywhere
            const size_t at = (rng() % 4 != 0) ? 0x20 + 0x28 + rng() % 0x30 : rng() % file.size();
            switch (rng() % 4) {
                case 0: file[at] = static_cast<char>(rng()); break;
                case 1: file[at] = static_cast<char>(file[at] ^ (1 << (rng() % 8))); break;
                case 2: file[at] = 0; break;
                default: file[at] = static_cast<char>(0xFF); break;
            }
        }
        Collision out;
        const ColResult result = Check(file, out);
        if (result == ColResult::Loadable) {
            ++passed;
            std::string why;
            if (!GameWouldSurvive(reinterpret_cast<const uint8_t*>(file.data()), file.size(), why)) {
                ML_CHECK(false);
                printf("  round %d: passed but unsafe: %s\n", round, why.c_str());
                break;
            }
        } else {
            ++rejected;
        }
    }
    ML_CHECK(passed > 1000);    // the test must not pass by rejecting everything
    ML_CHECK(rejected > 1000);
}

ML_TEST(dff_collision_chunk_is_found) {
    const std::string col = BuildCol3(TypicalSpec());
    {
        const std::string dff = BuildDff(col);
        uint64_t offset = 0;
        uint32_t size = 0;
        std::string why;
        ML_CHECK(FindSampCollision(MemorySource(dff), offset, size, why));
        ML_CHECK_EQ(size, static_cast<uint32_t>(col.size()));
        ML_CHECK(dff.compare(static_cast<size_t>(offset), col.size(), col) == 0);
    }
    {   // behind another plugin of the clump, and with a UV animation dictionary in front of the clump
        const std::string dff = BuildDff(col, true, true);
        uint64_t offset = 0;
        uint32_t size = 0;
        std::string why;
        ML_CHECK(FindSampCollision(MemorySource(dff), offset, size, why));
        ML_CHECK(dff.compare(static_cast<size_t>(offset), col.size(), col) == 0);
    }
    {   // Rockstar's own collision plugin is not SA-MP's
        const std::string dff = BuildDff(col, false, false, 0x253F2FA);
        uint64_t offset = 0;
        uint32_t size = 0;
        std::string why;
        ML_CHECK(!FindSampCollision(MemorySource(dff), offset, size, why));
        ML_CHECK(why.empty());
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(dff), out) == ColResult::NoChunk);
    }
    {   // no collision at all
        const std::string dff = BuildDff("");
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(dff), out) == ColResult::NoChunk);
    }
}

ML_TEST(dff_damaged_files_are_reported_not_followed) {
    const std::string col = BuildCol3(TypicalSpec());
    const std::string dff = BuildDff(col, false, true);
    {   // every truncation: never found, never a crash
        for (size_t cut = 0; cut < dff.size(); cut += 3) {
            const std::string part = dff.substr(0, cut);
            Collision out;
            const ColResult result = ReadCollision(MemorySource(part), out);
            ML_CHECK(result == ColResult::Bad || result == ColResult::NoChunk);
        }
    }
    {   // a clump that claims to be larger than the file
        std::string bad = dff;
        Set32(bad, 4, 0x7FFFFFFF);
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(bad), out) == ColResult::Bad);
    }
    {   // random damage to the chunk headers
        std::mt19937 rng(7);
        for (int round = 0; round < 3000; ++round) {
            std::string bad = dff;
            for (int e = 0; e < 3; ++e) bad[rng() % std::min<size_t>(bad.size(), 400)] = static_cast<char>(rng());
            Collision out;
            const ColResult result = ReadCollision(MemorySource(bad), out);
            if (result == ColResult::Loadable) {
                std::string why;
                ML_CHECK(GameWouldSurvive(out.file.data(), out.file.size(), why));
            }
        }
    }
    {   // not a model file
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(std::string("hello")), out) == ColResult::Bad);
        ML_CHECK(ReadCollision(MemorySource(std::string(64, '\0')), out) == ColResult::Bad);
    }
}

ML_TEST(dff_other_collision_versions) {
    {   // version 1 without anything in it: the five such files of the map
        std::string coll = "COLL";
        Put32(coll, 84);
        std::string name = "concerth05";
        name.resize(22, '\0');
        coll += name;
        Put16(coll, 0);
        PutF(coll, 2.0f);
        for (int i = 0; i < 3; ++i) PutF(coll, 0.0f);
        for (float v : {-1.0f, -2.0f, -3.0f, 1.0f, 2.0f, 3.0f}) PutF(coll, v);
        for (int i = 0; i < 5; ++i) Put32(coll, 0);
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(BuildDff(coll)), out) == ColResult::Empty);
        ML_CHECK(out.hasBox);
        ML_CHECK_EQ(out.box[1], -2.0f);
        ML_CHECK_EQ(out.box[5], 3.0f);
        ML_CHECK(out.file.empty());

        // ... and with a sphere: not something this client loads
        std::string filled = coll.substr(0, 0x20 + 40);
        Put32(filled, 1);
        filled += std::string(20, '\0');
        for (int i = 0; i < 4; ++i) Put32(filled, 0);
        ML_CHECK(ReadCollision(MemorySource(BuildDff(filled)), out) == ColResult::Unsupported);
        ML_CHECK(out.file.empty());

        // ... and cut short
        ML_CHECK(ReadCollision(MemorySource(BuildDff(coll.substr(0, coll.size() - 6))), out) == ColResult::Bad);
    }
    {   // version 2
        std::string col2 = BuildCol3(TypicalSpec());
        col2[3] = '2';
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(BuildDff(col2)), out) == ColResult::Unsupported);
        ML_CHECK(out.hasBox);
        ML_CHECK(out.file.empty());
    }
    {   // garbage in the chunk
        Collision out;
        ML_CHECK(ReadCollision(MemorySource(BuildDff(std::string(200, 'x'))), out) == ColResult::Bad);
        ML_CHECK(ReadCollision(MemorySource(BuildDff(std::string(8, 'x'))), out) == ColResult::Bad);
    }
}

ML_TEST(dff_collision_from_a_file_on_disk) {
    mltest::TempDir dir;
    const std::string col = BuildCol3(TypicalSpec());
    mltest::WriteFile(dir.path + "model.dff", BuildDff(col, false, true));
    Collision out;
    ML_CHECK(ReadCollisionFile((dir.path + "model.dff").c_str(), out) == ColResult::Loadable);
    ML_CHECK_EQ(out.file.size(), col.size());
    ML_CHECK_EQ(out.faces, 4u);
    ML_CHECK(ReadCollisionFile((dir.path + "missing.dff").c_str(), out) == ColResult::Bad);
    ML_CHECK(ReadCollisionFile(dir.path.c_str(), out) == ColResult::Bad);   // a folder
    ML_CHECK(ReadCollisionFile(nullptr, out) == ColResult::Bad);
}

// With VC_REPO=<checkout of samp-vice-city> the real files are read: every model of the map.
ML_TEST(real_map_every_collision_is_loadable_or_empty) {
    const char* repo = getenv("VC_REPO");
    if (!repo || !*repo) {
        printf("  (skipped: VC_REPO not set)\n");
        return;
    }
    const std::string dir = std::string(repo) + "/models/vice_city/";
    DIR* d = opendir(dir.c_str());
    ML_CHECK(d != nullptr);
    if (!d) return;
    size_t files = 0, loadable = 0, empty = 0, none = 0, other = 0;
    uint64_t faces = 0, boxes = 0, spheres = 0, bytes = 0, groups = 0;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() < 4 || strcasecmp(name.c_str() + name.size() - 4, ".dff") != 0) continue;
        ++files;
        Collision out;
        const ColResult result = ReadCollisionFile((dir + name).c_str(), out);
        switch (result) {
            case ColResult::Loadable: {
                ++loadable;
                faces += out.faces;
                boxes += out.boxes;
                spheres += out.spheres;
                groups += out.faceGroups ? 1 : 0;
                bytes += out.file.size();
                std::string why;
                if (!GameWouldSurvive(out.file.data(), out.file.size(), why)) {
                    ML_CHECK(false);
                    printf("  %s: %s\n", name.c_str(), why.c_str());
                }
                break;
            }
            case ColResult::Empty: ++empty; break;
            case ColResult::NoChunk: ++none; break;
            default:
                ++other;
                printf("  %s: %s\n", name.c_str(), out.detail.c_str());
                break;
        }
    }
    closedir(d);
    printf("  %zu dff: %zu loadable, %zu empty, %zu without collision, %zu other; %llu faces, %llu boxes, %llu spheres, "
           "%zu with sections, %llu bytes\n",
           files, loadable, empty, none, other, static_cast<unsigned long long>(faces),
           static_cast<unsigned long long>(boxes), static_cast<unsigned long long>(spheres),
           static_cast<size_t>(groups), static_cast<unsigned long long>(bytes));
    // What the repository holds (commit of 2024): 2433 models, one of them (vc_map.dff) without collision.
    ML_CHECK_EQ(files, static_cast<size_t>(2433));
    ML_CHECK_EQ(other, static_cast<size_t>(0));
    ML_CHECK_EQ(none, static_cast<size_t>(1));
    ML_CHECK_EQ(loadable + empty, static_cast<size_t>(2432));
    ML_CHECK_EQ(faces, 321146ull);
    ML_CHECK_EQ(boxes, 5488ull);
    ML_CHECK_EQ(spheres, 556ull);
    ML_CHECK_EQ(static_cast<size_t>(groups), static_cast<size_t>(868));
}
