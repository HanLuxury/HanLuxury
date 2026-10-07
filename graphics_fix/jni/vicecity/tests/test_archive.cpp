#include "vctest.h"

#include <set>

#include "../../modloader/ImgOverlay.h"
#include "../VcArchive.h"
#include "../VcMap.h"

using mltest::TempDir;
using mltest::WriteFile;

namespace {

std::string Bytes(size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>(x >> 24);
    }
    return s;
}

struct DirEntry {
    uint32_t offset = 0;   // sectors
    uint32_t sectors = 0;
    std::string name;
};

// The archive as the game reads it: through the FILE*, directory first.
bool ReadDirectory(FILE* f, std::vector<DirEntry>& out) {
    out.clear();
    uint8_t head[8];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(head, 1, 8, f) != 8 || memcmp(head, "VER2", 4) != 0) return false;
    const uint32_t count = ml::detail::GetU32(head + 4);
    if (count > 100000) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t e[32];
        if (fread(e, 1, 32, f) != 32) return false;
        DirEntry d;
        d.offset = ml::detail::GetU32(e);
        d.sectors = ml::detail::GetU16(e + 4);
        if (ml::detail::GetU16(e + 6) != 0) return false;   // "size in archive": the game prefers it when it is set
        d.name.assign(reinterpret_cast<const char*>(e + 8), strnlen(reinterpret_cast<const char*>(e + 8), 24));
        out.push_back(std::move(d));
    }
    return true;
}

std::string ReadEntry(FILE* f, const DirEntry& d) {
    std::string out(static_cast<size_t>(d.sectors) * ml::kSector, '\0');
    if (fseek(f, static_cast<long>(d.offset) * static_cast<long>(ml::kSector), SEEK_SET) != 0) return std::string();
    if (fread(out.data(), 1, out.size(), f) != out.size()) return std::string();
    return out;
}

// Models with files of the given sizes; every one gets a game id unless `ids` says otherwise.
struct Fixture {
    TempDir dir;
    vc::FolderIndex files;
    std::vector<vc::MapModel> models;
    std::vector<std::string> contents;

    explicit Fixture(const std::vector<size_t>& sizes) {
        for (size_t i = 0; i < sizes.size(); ++i) {
            const std::string name = "model" + std::to_string(i) + ".dff";
            contents.push_back(Bytes(sizes[i], static_cast<unsigned>(i + 1)));
            WriteFile(dir.path + name, contents.back());
        }
        files.Scan(dir.path);
        models.resize(sizes.size());
        for (size_t i = 0; i < sizes.size(); ++i) {
            vc::MapModel& m = models[i];
            m.dff = files.Find("model" + std::to_string(i) + ".dff", true);
            m.gameId = 20000 + static_cast<int>(i);
            m.usable = true;
            snprintf(m.name, sizeof(m.name), "vcm%04u", static_cast<unsigned>(i));
        }
    }
};

}  // namespace

ML_TEST(archive_name_is_recognised_as_the_game_spells_it) {
    ML_CHECK(vc::IsArchivePath("VICECITY\\GTA3.IMG"));
    ML_CHECK(vc::IsArchivePath("VICECITY/GTA3.IMG"));    // the game turns '\\' into '/' before it opens a file
    ML_CHECK(vc::IsArchivePath("vicecity/gta3.img"));
    ML_CHECK(vc::IsArchivePath("ViceCity\\Gta3.Img"));
    ML_CHECK(vc::IsArchivePath("./VICECITY/GTA3.IMG"));
    ML_CHECK(vc::IsArchivePath(vc::kArchiveName));
    ML_CHECK(vc::IsArchivePath(vc::kArchiveKey));

    ML_CHECK(!vc::IsArchivePath(nullptr));
    ML_CHECK(!vc::IsArchivePath(""));
    ML_CHECK(!vc::IsArchivePath("TEXDB\\GTA3.IMG"));     // the game's own archive of that name
    ML_CHECK(!vc::IsArchivePath("TEXDB/GTA3.IMG"));
    ML_CHECK(!vc::IsArchivePath("GTA3.IMG"));
    ML_CHECK(!vc::IsArchivePath("VICECITY/GTA3.IMGX"));
    ML_CHECK(!vc::IsArchivePath("VICECITY/GTA3.IM"));
    ML_CHECK(!vc::IsArchivePath("XVICECITY/GTA3.IMG"));
    ML_CHECK(!vc::IsArchivePath("VICECITY/GTA_INT.IMG"));
    ML_CHECK(!vc::IsArchivePath("DATA/VICECITY/GTA3.IMG"));

    // What the game makes of the name: the texture database of every model in it.
    const std::string name = vc::kArchiveName;
    const size_t slash = name.find('\\');
    ML_CHECK(slash != std::string::npos);
    std::string db = name.substr(slash + 1, name.size() - slash - 1 - 4);
    for (char& c : db) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    ML_CHECK_EQ(db, std::string(vc::kTextureDatabase));
    ML_CHECK(name.size() < 40);   // tStreamingFileDesc::m_szName
}

ML_TEST(archive_holds_every_model_file) {
    // Sizes around the sector limits, and one model that is large.
    Fixture fx({12, 100, 2047, 2048, 2049, 4096, 4097, 300000, 1});
    vc::Archive archive;
    std::string why;
    ML_CHECK(archive.Build(fx.models, why));
    ML_CHECK(why.empty());
    ML_CHECK_EQ(archive.entryCount(), 9u);
    ML_CHECK(archive.overlay() != nullptr);
    ML_CHECK(archive.sizeBytes() % ml::kSector == 0);

    FILE* f = archive.Open();
    ML_CHECK(f != nullptr);
    if (!f) return;
    std::vector<DirEntry> dir;
    ML_CHECK(ReadDirectory(f, dir));
    // The first entry is a name the game skips; then one entry per model, in table order.
    ML_CHECK_EQ(dir.size(), 10u);
    if (dir.size() == 10) {
        ML_CHECK_EQ(dir[0].sectors, 0u);
        ML_CHECK(dir[0].name.find(".dff") == std::string::npos);
        uint32_t expectedOffset = 1;   // the directory takes one sector
        for (size_t i = 0; i < 9; ++i) {
            const DirEntry& d = dir[i + 1];
            char name[32];
            snprintf(name, sizeof(name), "vcm%04zu.dff", i);
            ML_CHECK_EQ(d.name, std::string(name));
            const size_t size = fx.contents[i].size();
            ML_CHECK_EQ(d.sectors, static_cast<uint32_t>((size + ml::kSector - 1) / ml::kSector));
            ML_CHECK_EQ(d.offset, expectedOffset);
            expectedOffset += d.sectors;
            // The file, followed by zeros up to the end of its last sector.
            std::string expected = fx.contents[i];
            expected.resize(static_cast<size_t>(d.sectors) * ml::kSector, '\0');
            ML_CHECK(ReadEntry(f, d) == expected);
        }
        ML_CHECK_EQ(static_cast<uint64_t>(expectedOffset) * ml::kSector, archive.sizeBytes());
    }
    // Reading past the end gives nothing, as with a file.
    ML_CHECK_EQ(fseek(f, static_cast<long>(archive.sizeBytes()), SEEK_SET), 0);
    char byte;
    ML_CHECK_EQ(fread(&byte, 1, 1, f), 0u);
    ML_CHECK(feof(f) != 0);
    fclose(f);
}

// The game's streaming thread and its loader read through handles of their own.
ML_TEST(archive_can_be_opened_several_times) {
    Fixture fx({5000, 7000, 9000});
    vc::Archive archive;
    std::string why;
    ML_CHECK(archive.Build(fx.models, why));
    FILE* a = archive.Open();
    FILE* b = archive.Open();
    ML_CHECK(a && b && a != b);
    if (a && b) {
        std::vector<DirEntry> da, db;
        ML_CHECK(ReadDirectory(a, da));
        ML_CHECK(ReadDirectory(b, db));
        ML_CHECK_EQ(da.size(), 4u);
        if (da.size() == 4 && db.size() == 4) {
            const std::string third = ReadEntry(a, da[3]);
            const std::string first = ReadEntry(b, db[1]);
            ML_CHECK(third.compare(0, 9000, fx.contents[2]) == 0);
            ML_CHECK(first.compare(0, 5000, fx.contents[0]) == 0);
            // Closing one handle leaves the other usable.
            fclose(a);
            a = nullptr;
            ML_CHECK(ReadEntry(b, db[2]).compare(0, 7000, fx.contents[1]) == 0);
        }
    }
    if (a) fclose(a);
    if (b) fclose(b);
}

// Only models that got a game id are in the archive: a model the game does not know by name would be put
// into its list of "extra objects" instead.
ML_TEST(archive_leaves_out_models_without_an_id) {
    Fixture fx({3000, 3001, 3002, 3003});
    fx.models[1].gameId = -1;
    fx.models[3].dff = nullptr;
    vc::Archive archive;
    std::string why;
    ML_CHECK(archive.Build(fx.models, why));
    ML_CHECK_EQ(archive.entryCount(), 2u);
    FILE* f = archive.Open();
    ML_CHECK(f != nullptr);
    if (!f) return;
    std::vector<DirEntry> dir;
    ML_CHECK(ReadDirectory(f, dir));
    ML_CHECK_EQ(dir.size(), 3u);
    if (dir.size() == 3) {
        ML_CHECK_EQ(dir[1].name, "vcm0000.dff");
        ML_CHECK_EQ(dir[2].name, "vcm0002.dff");
        ML_CHECK(ReadEntry(f, dir[2]).compare(0, 3002, fx.contents[2]) == 0);
    }
    fclose(f);
}

ML_TEST(archive_refuses_what_the_game_cannot_take) {
    {   // nothing to put in
        Fixture fx({3000});
        fx.models[0].gameId = -1;
        vc::Archive archive;
        std::string why;
        ML_CHECK(!archive.Build(fx.models, why));
        ML_CHECK(!why.empty());
        ML_CHECK(archive.Open() == nullptr);
        ML_CHECK_EQ(archive.entryCount(), 0u);
        ML_CHECK_EQ(archive.sizeBytes(), 0u);
    }
    {   // no models at all
        vc::Archive archive;
        std::string why;
        ML_CHECK(!archive.Build({}, why));
        ML_CHECK(archive.Open() == nullptr);
    }
    {   // an empty file: the game takes an entry of no sectors for "not there"
        Fixture fx({3000, 0});
        vc::Archive archive;
        std::string why;
        ML_CHECK(!archive.Build(fx.models, why));
        ML_CHECK(why.find("model1.dff") != std::string::npos);
        ML_CHECK(archive.Open() == nullptr);
    }
    {   // larger than the game's streaming buffer may become
        Fixture fx({3000});
        fx.models[0].dff->size = ml::kMaxStreamFileBytes + 1;
        vc::Archive archive;
        std::string why;
        ML_CHECK(!archive.Build(fx.models, why));
        ML_CHECK(archive.Open() == nullptr);
    }
    {   // a second Build replaces the first one
        Fixture fx({3000, 5000});
        vc::Archive archive;
        std::string why;
        ML_CHECK(archive.Build(fx.models, why));
        ML_CHECK_EQ(archive.entryCount(), 2u);
        fx.models[1].gameId = -1;
        ML_CHECK(archive.Build(fx.models, why));
        ML_CHECK_EQ(archive.entryCount(), 1u);
        fx.models[0].gameId = -1;
        ML_CHECK(!archive.Build(fx.models, why));
        ML_CHECK_EQ(archive.entryCount(), 0u);
        ML_CHECK(archive.Open() == nullptr);
    }
}

// What the client does before it hands the archive to the game.
ML_TEST(archive_self_test) {
    Fixture fx({12, 2048, 5000, 70000});
    vc::Archive archive;
    std::string why = "x";
    ML_CHECK(!archive.SelfTest(why));   // nothing built yet
    ML_CHECK(!why.empty());
    ML_CHECK(archive.Build(fx.models, why));
    ML_CHECK(archive.SelfTest(why));
    ML_CHECK(why.empty());

    // A file that changed after the archive was laid out is noticed (its first sector is compared).
    mltest::WriteFile(fx.dir.path + "model2.dff", std::string(5000, 'z'));
    ML_CHECK(archive.SelfTest(why));    // read fresh on both sides: still the same bytes
    // ... while a file that is gone says nothing about the reading and does not fail the test.
    unlink((fx.dir.path + "model1.dff").c_str());
    ML_CHECK(archive.SelfTest(why));

    // Many models: a sample is read, the last one always.
    std::vector<size_t> sizes(200, 2500);
    Fixture many(sizes);
    vc::Archive big;
    ML_CHECK(big.Build(many.models, why));
    ML_CHECK(big.SelfTest(why));
}

// A model file that disappears while the game runs: the game reads zeros and rejects that model, the
// others are untouched.
ML_TEST(archive_survives_a_vanished_file) {
    Fixture fx({4000, 4000, 4000});
    vc::Archive archive;
    std::string why;
    ML_CHECK(archive.Build(fx.models, why));
    unlink((fx.dir.path + "model1.dff").c_str());
    FILE* f = archive.Open();
    ML_CHECK(f != nullptr);
    if (!f) return;
    std::vector<DirEntry> dir;
    ML_CHECK(ReadDirectory(f, dir));
    if (dir.size() == 4) {
        ML_CHECK(ReadEntry(f, dir[1]).compare(0, 4000, fx.contents[0]) == 0);
        ML_CHECK(ReadEntry(f, dir[2]) == std::string(2 * ml::kSector, '\0'));
        ML_CHECK(ReadEntry(f, dir[3]).compare(0, 4000, fx.contents[2]) == 0);
    }
    fclose(f);
}

// The whole map from a checkout of the repository (VC_REPO=<folder>): the tables joined with the files, ids
// handed out, the archive built and every model read back through it.
ML_TEST(archive_of_the_real_map) {
    const char* repo = getenv("VC_REPO");
    if (!repo || !*repo) {
        printf("  (skipped: VC_REPO is not set)\n");
        return;
    }
    const std::string root = std::string(repo) + "/";
    const std::string modelDir = vc::FindModelFolder(root);
    ML_CHECK_EQ(modelDir, root + "models/vice_city/");
    vc::FolderIndex files;
    ML_CHECK(files.Scan(modelDir));
    vc::Map map;
    map.Build(files);
    // One model of the script has no texture file in the repository (DS_SIGN.txd).
    ML_CHECK_EQ(map.usableModels(), vc::kModelCount - 1);
    const std::vector<std::string> missing = map.MissingFiles(10);
    ML_CHECK_EQ(missing.size(), 1u);
    for (const std::string& m : missing) printf("  missing: %s\n", m.c_str());

    std::vector<int> ids;
    for (int id = 25000 - static_cast<int>(map.usableModels()); id < 25000; ++id) ids.push_back(id);
    ML_CHECK_EQ(map.AssignIds(ids), map.usableModels());

    vc::Archive archive;
    std::string why;
    ML_CHECK(archive.Build(map.models(), why));
    ML_CHECK(archive.SelfTest(why));
    ML_CHECK_EQ(archive.entryCount(), map.usableModels());
    FILE* f = archive.Open();
    ML_CHECK(f != nullptr);
    if (!f) return;
    std::vector<DirEntry> dir;
    ML_CHECK(ReadDirectory(f, dir));
    ML_CHECK_EQ(dir.size(), map.usableModels() + 1);

    std::set<std::string> names;
    uint32_t largest = 0;
    uint64_t total = 0;
    size_t checked = 0, next = 1;
    for (const vc::MapModel& m : map.models()) {
        if (m.gameId < 0 || next >= dir.size()) continue;
        const DirEntry& d = dir[next++];
        ML_CHECK_EQ(d.name, std::string(m.name) + ".dff");
        ML_CHECK(names.insert(d.name).second);
        // What the game's directory reader needs of a name: one dot, at most 20 characters before it.
        ML_CHECK(d.name.find('.') == d.name.size() - 4 && d.name.size() - 4 <= 20);
        ML_CHECK(d.sectors > 0);
        largest = std::max(largest, d.sectors);
        total += d.sectors;
        const std::string raw = mltest::ReadFile(m.dff->path);
        const std::string got = ReadEntry(f, d);
        ML_CHECK_EQ(raw.size(), m.dff->size);
        const bool same = got.size() >= raw.size() && got.compare(0, raw.size(), raw) == 0 &&
                          got.find_first_not_of('\0', raw.size()) == std::string::npos;
        ML_CHECK(same);
        if (!same) printf("  differs: %s\n", m.def->dff);
        // A model file starts with a clump, or with the UV animations that belong to it.
        const uint32_t firstChunk = raw.size() >= 4 ? vctest::Get32(raw, 0) : 0;
        ML_CHECK(firstChunk == 0x10 || firstChunk == 0x2B);
        ++checked;
    }
    fclose(f);
    ML_CHECK_EQ(checked, map.usableModels());
    printf("  %zu models, %.1f MB, largest %.1f KiB (the game's streaming buffer grows to that)\n", checked,
           static_cast<double>(total) * ml::kSector / (1024.0 * 1024.0), static_cast<double>(largest) * ml::kSector / 1024.0);
    ML_CHECK(static_cast<uint64_t>(largest) * ml::kSector <= ml::kMaxStreamFileBytes);
}
