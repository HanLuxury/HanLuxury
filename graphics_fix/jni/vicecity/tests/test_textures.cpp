#include "vctest.h"

#include <algorithm>
#include <thread>

#include <dirent.h>
#include <strings.h>

#include "../../modloader/TxdConvert.h"
#include "../VcMap.h"
#include "../VcMapData.h"
#include "../VcTextures.h"

using mltest::TempDir;
using mltest::WriteFile;
using vctest::BuildTxd;

// ------------------------------------------------------------------------------------------------------
// A fake game: texture dictionaries with the reference counting rules of the real one.
namespace fake {

struct Dict;

struct Texture {
    std::string name;
    int refs = 1;   // the dictionary's own reference
    Dict* dict = nullptr;
};

struct Dict {
    std::vector<Texture*> textures;
};

struct Stream {
    const uint8_t* data;
    uint32_t size;
};

struct Game {
    int liveTextures = 0, liveDicts = 0, liveStreams = 0;
    int dictsCreated = 0, texturesRead = 0;
    std::string rejectTexture;              // ReadTexture() refuses the texture with this name
    bool rejectAll = false;
    bool noDictionaries = false;            // CreateDictionary() fails
    std::function<void()> whileReading;     // runs inside ReadTexture(), the way game code could call back in
    uint64_t bytesSeen = 0;
};

Game* g = nullptr;

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

void* OpenMemoryStream(const uint8_t* data, uint32_t size) {
    ++g->liveStreams;
    return new Stream{data, size};
}

void CloseStream(void* stream) {
    --g->liveStreams;
    delete static_cast<Stream*>(stream);
}

// Accepts exactly what the converter produces: a texture native chunk with a STRUCT chunk for platform 9,
// whose sizes add up.
void* ReadTexture(void* streamPtr) {
    const Stream* s = static_cast<Stream*>(streamPtr);
    ++g->texturesRead;
    g->bytesSeen += s->size;
    if (g->whileReading) g->whileReading();
    if (g->rejectAll) return nullptr;
    if (s->size < 24 + 88 || U32(s->data) != 0x15 || U32(s->data + 12) != 0x01 || U32(s->data + 24) != 9) return nullptr;
    if (U32(s->data + 4) != s->size - 12 || U32(s->data + 16) > s->size - 24) return nullptr;
    const char* name = reinterpret_cast<const char*>(s->data + 24 + 8);
    Texture* t = new Texture;
    t->name.assign(name, strnlen(name, 32));
    if (!g->rejectTexture.empty() && t->name == g->rejectTexture) {
        delete t;
        return nullptr;
    }
    ++g->liveTextures;
    return t;
}

void AddTextureRef(void* texture) { ++static_cast<Texture*>(texture)->refs; }

void* CreateDictionary() {
    if (g->noDictionaries) return nullptr;
    ++g->liveDicts;
    ++g->dictsCreated;
    return new Dict;
}

void AddTexture(void* dict, void* texture) {
    static_cast<Texture*>(texture)->dict = static_cast<Dict*>(dict);
    static_cast<Dict*>(dict)->textures.push_back(static_cast<Texture*>(texture));
}

void* FindTexture(void* dict, const char* name) {
    for (Texture* t : static_cast<Dict*>(dict)->textures) {
        if (strcasecmp(t->name.c_str(), name) == 0) return t;
    }
    return nullptr;
}

// RwTextureDestroy(): what a material does when it lets go of a texture.
void Release(void* texturePtr) {
    Texture* t = static_cast<Texture*>(texturePtr);
    if (--t->refs > 0) return;
    if (t->dict) {
        auto& list = t->dict->textures;
        list.erase(std::remove(list.begin(), list.end(), t), list.end());
    }
    --g->liveTextures;
    delete t;
}

// CTxdStore::RemoveTxd(): textures somebody still holds leave the dictionary and live on, the rest goes.
void DestroyDictionary(void* dictPtr) {
    Dict* d = static_cast<Dict*>(dictPtr);
    const std::vector<Texture*> all = d->textures;
    for (Texture* t : all) {
        if (t->refs > 1) {
            --t->refs;
            t->dict = nullptr;
            d->textures.erase(std::remove(d->textures.begin(), d->textures.end(), t), d->textures.end());
        }
    }
    for (Texture* t : d->textures) {
        --g->liveTextures;
        delete t;
    }
    --g->liveDicts;
    delete d;
}

vc::TextureApi Api() {
    vc::TextureApi api;
    api.OpenMemoryStream = &OpenMemoryStream;
    api.CloseStream = &CloseStream;
    api.ReadTexture = &ReadTexture;
    api.AddTextureRef = &AddTextureRef;
    api.CreateDictionary = &CreateDictionary;
    api.AddTexture = &AddTexture;
    api.FindTexture = &FindTexture;
    api.DestroyDictionary = &DestroyDictionary;
    return api;
}

// The game for the length of one test; checks at the end that nothing of it was leaked.
struct Scope {
    Game game;
    Scope() { g = &game; }
    ~Scope() {
        ML_CHECK_EQ(game.liveStreams, 0);
        ML_CHECK_EQ(game.liveDicts, 0);
        ML_CHECK_EQ(game.liveTextures, 0);
        g = nullptr;
    }
};

const std::string& Name(void* texture) { return static_cast<Texture*>(texture)->name; }
int Refs(void* texture) { return static_cast<Texture*>(texture)->refs; }

}  // namespace fake

namespace {

bool HasFailure(vc::TextureCache& cache, const char* part) {
    for (const std::string& f : cache.Failures(100)) {
        if (f.find(part) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

ML_TEST(textures_file_is_read_on_first_use) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"wall", "Roof", "door"}));
    {
        vc::TextureCache cache(fake::Api());
        const int h = cache.Add(dir.path + "a.txd");
        ML_CHECK_EQ(h, 0);
        ML_CHECK_EQ(cache.stats().files, 1u);
        ML_CHECK_EQ(cache.stats().loaded, 0u);
        ML_CHECK_EQ(game.game.texturesRead, 0);   // registering reads nothing

        void* roof = cache.Find(h, "roof", 1000, false);   // names compare the way the game compares them
        ML_CHECK(roof != nullptr);
        if (roof) {
            ML_CHECK_EQ(fake::Name(roof), "Roof");
            ML_CHECK_EQ(fake::Refs(roof), 1);
        }
        ML_CHECK_EQ(game.game.texturesRead, 3);
        ML_CHECK_EQ(game.game.dictsCreated, 1);
        ML_CHECK_EQ(game.game.liveStreams, 0);
        const vc::TextureCache::Stats st = cache.stats();
        ML_CHECK_EQ(st.loaded, 1u);
        ML_CHECK_EQ(st.loads, 1u);
        ML_CHECK_EQ(st.failed, 0u);
        ML_CHECK_EQ(st.bytes, game.game.bytesSeen);
        ML_CHECK(st.bytes > 3 * 64);

        // The second request does not read the file again.
        ML_CHECK(cache.Find(h, "WALL", 1001, false) != nullptr);
        ML_CHECK(cache.Find(h, "door", 1002, false) != nullptr);
        ML_CHECK(cache.Find(h, "window", 1003, false) == nullptr);
        ML_CHECK_EQ(game.game.texturesRead, 3);
        ML_CHECK_EQ(cache.stats().loads, 1u);

        // A reference of the caller's own, as RwTextureRead hands textures out.
        void* door = cache.Find(h, "door", 1004, true);
        ML_CHECK(door != nullptr);
        if (door) {
            ML_CHECK_EQ(fake::Refs(door), 2);
            fake::Release(door);
        }
        ML_CHECK(cache.Failures(10).empty());
    }
    // The cache released what it had loaded.
    ML_CHECK_EQ(game.game.liveDicts, 0);
}

ML_TEST(textures_bad_requests) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"wall"}));
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "a.txd");
    ML_CHECK(cache.Find(-1, "wall", 0, false) == nullptr);
    ML_CHECK(cache.Find(1, "wall", 0, false) == nullptr);
    ML_CHECK(cache.Find(1000000, "wall", 0, false) == nullptr);
    ML_CHECK(cache.Find(h, nullptr, 0, false) == nullptr);
    ML_CHECK(cache.Find(h, "", 0, false) == nullptr);
    ML_CHECK_EQ(game.game.texturesRead, 0);   // none of them made the cache read the file
    ML_CHECK(cache.Find(h, "wall", 0, false) != nullptr);
}

// Without the game's functions the cache answers "not here" and touches nothing.
ML_TEST(textures_incomplete_api) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"wall"}));
    {
        vc::TextureCache none{vc::TextureApi()};
        const int h = none.Add(dir.path + "a.txd");
        ML_CHECK(none.Find(h, "wall", 0, true) == nullptr);
        ML_CHECK_EQ(none.Collect(100000, 1, nullptr), 0u);
        ML_CHECK_EQ(none.stats().loaded, 0u);
    }
    {
        vc::TextureApi partial = fake::Api();
        partial.DestroyDictionary = nullptr;
        ML_CHECK(!partial.Complete());
        vc::TextureCache cache(partial);
        const int h = cache.Add(dir.path + "a.txd");
        ML_CHECK(cache.Find(h, "wall", 0, true) == nullptr);
    }
    ML_CHECK(fake::Api().Complete());
    ML_CHECK_EQ(game.game.texturesRead, 0);
}

ML_TEST(textures_unused_files_are_released) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"a1", "a2"}));
    WriteFile(dir.path + "b.txd", BuildTxd({"b1"}));
    WriteFile(dir.path + "c.txd", BuildTxd({"c1"}));
    vc::TextureCache cache(fake::Api());
    const int a = cache.Add(dir.path + "a.txd");
    const int b = cache.Add(dir.path + "b.txd");
    const int c = cache.Add(dir.path + "c.txd", true);   // pinned
    ML_CHECK(cache.Find(a, "a1", 1000, false) != nullptr);
    ML_CHECK(cache.Find(b, "b1", 1000, false) != nullptr);
    ML_CHECK(cache.Find(c, "c1", 1000, false) != nullptr);
    ML_CHECK_EQ(cache.stats().loaded, 3u);
    const uint64_t bytesAll = cache.stats().bytes;

    // Not idle long enough.
    ML_CHECK_EQ(cache.Collect(1000 + 4999, 5000, nullptr), 0u);
    ML_CHECK_EQ(cache.stats().loaded, 3u);

    // b was used in between; a is claimed by the game (a model that uses it is loaded).
    ML_CHECK(cache.Find(b, "b1", 5000, false) != nullptr);
    std::vector<int> asked;
    ML_CHECK_EQ(cache.Collect(7000, 5000,
                              [&](int handle) {
                                  asked.push_back(handle);
                                  return handle == a;
                              }),
                0u);
    ML_CHECK_EQ(asked.size(), 1u);   // only the idle, unpinned file is asked about
    if (!asked.empty()) ML_CHECK_EQ(asked[0], a);

    // Nobody claims a any more: it goes, b (recently used) and c (pinned) stay.
    ML_CHECK_EQ(cache.Collect(7000, 5000, [](int) { return false; }), 1u);
    vc::TextureCache::Stats st = cache.stats();
    ML_CHECK_EQ(st.loaded, 2u);
    ML_CHECK_EQ(st.released, 1u);
    ML_CHECK(st.bytes < bytesAll);
    ML_CHECK_EQ(game.game.liveDicts, 2);
    ML_CHECK_EQ(game.game.liveTextures, 2);

    // Much later b goes too; the pinned file never does.
    ML_CHECK_EQ(cache.Collect(1000000, 5000, nullptr), 1u);
    ML_CHECK_EQ(cache.stats().loaded, 1u);
    ML_CHECK_EQ(cache.Collect(100000000, 1, nullptr), 0u);
    ML_CHECK_EQ(game.game.liveDicts, 1);
    const uint64_t bytesPinned = cache.stats().bytes;
    ML_CHECK(bytesPinned > 0 && bytesPinned < st.bytes);

    // Asked for again, a file is simply read again.
    const int readBefore = game.game.texturesRead;
    ML_CHECK(cache.Find(a, "a2", 2000000, false) != nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore + 2);
    ML_CHECK_EQ(cache.stats().loads, 4u);
    ML_CHECK_EQ(cache.stats().released, 2u);
    ML_CHECK_EQ(cache.stats().bytes, bytesPinned + (bytesAll - st.bytes));   // the pinned file and a
}

// A texture a material still holds lives on when its file is released, and the next model gets a fresh one.
ML_TEST(textures_held_textures_survive_release) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"wall", "roof"}));
    vc::TextureCache cache(fake::Api());
    const int a = cache.Add(dir.path + "a.txd");
    void* held = cache.Find(a, "wall", 1000, true);
    ML_CHECK(held != nullptr);
    if (!held) return;
    ML_CHECK_EQ(fake::Refs(held), 2);

    ML_CHECK_EQ(cache.Collect(100000, 5000, nullptr), 1u);
    ML_CHECK_EQ(game.game.liveDicts, 0);
    ML_CHECK_EQ(game.game.liveTextures, 1);   // "roof" went with the dictionary, "wall" is still in use
    ML_CHECK_EQ(fake::Refs(held), 1);
    ML_CHECK_EQ(fake::Name(held), "wall");

    void* again = cache.Find(a, "wall", 200000, true);
    ML_CHECK(again != nullptr);
    ML_CHECK(again != held);
    ML_CHECK_EQ(game.game.liveTextures, 3);

    fake::Release(held);   // the old model is unloaded
    ML_CHECK_EQ(game.game.liveTextures, 2);
    if (again) fake::Release(again);
}

ML_TEST(textures_failures_are_final_and_reported) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "nottxd.txd", std::string(4096, 'x'));
    WriteFile(dir.path + "empty.txd", "");
    WriteFile(dir.path + "none.txd", BuildTxd({}));
    WriteFile(dir.path + "refused.txd", BuildTxd({"no"}));
    mltest::MakeDirs(dir.path + "folder.txd");
    std::string cut = BuildTxd({"one", "two"});
    cut.resize(cut.size() - 40);   // ends inside the second texture
    WriteFile(dir.path + "cut.txd", cut);

    vc::TextureCache cache(fake::Api());
    const int missing = cache.Add(dir.path + "missing.txd");
    const int notTxd = cache.Add(dir.path + "nottxd.txd");
    const int empty = cache.Add(dir.path + "empty.txd");
    const int none = cache.Add(dir.path + "none.txd");
    const int refused = cache.Add(dir.path + "refused.txd");
    const int folder = cache.Add(dir.path + "folder.txd");
    const int cutFile = cache.Add(dir.path + "cut.txd");
    game.game.rejectTexture = "no";

    ML_CHECK(cache.Find(missing, "x", 1000, false) == nullptr);
    ML_CHECK(cache.Find(notTxd, "x", 1000, false) == nullptr);
    ML_CHECK(cache.Find(empty, "x", 1000, false) == nullptr);
    ML_CHECK(cache.Find(none, "x", 1000, false) == nullptr);
    ML_CHECK(cache.Find(refused, "no", 1000, false) == nullptr);
    ML_CHECK(cache.Find(folder, "x", 1000, false) == nullptr);
    // A file that ends early still gives its complete textures.
    ML_CHECK(cache.Find(cutFile, "one", 1000, false) != nullptr);
    ML_CHECK(cache.Find(cutFile, "two", 1000, false) == nullptr);

    vc::TextureCache::Stats st = cache.stats();
    ML_CHECK_EQ(st.files, 7u);
    ML_CHECK_EQ(st.failed, 6u);
    ML_CHECK_EQ(st.loaded, 1u);
    ML_CHECK_EQ(st.retries, 0u);
    ML_CHECK_EQ(game.game.liveDicts, 1);   // nothing is left behind by the files that failed

    ML_CHECK(HasFailure(cache, "missing.txd: file tidak bisa dibuka"));
    ML_CHECK(HasFailure(cache, "nottxd.txd: bukan file TXD"));
    ML_CHECK(HasFailure(cache, "empty.txd: bukan file biasa"));
    ML_CHECK(HasFailure(cache, "none.txd: tidak ada tekstur yang bisa dipakai"));
    ML_CHECK(HasFailure(cache, "refused.txd: tidak ada tekstur yang diterima pembaca game"));
    ML_CHECK(HasFailure(cache, "folder.txd: bukan file biasa"));
    ML_CHECK(!HasFailure(cache, "cut.txd"));
    ML_CHECK_EQ(cache.Failures(100).size(), 6u);
    ML_CHECK_EQ(cache.Failures(2).size(), 2u);
    ML_CHECK_EQ(cache.Failures(0).size(), 0u);
    // The report names the file, not the folders of the device.
    for (const std::string& f : cache.Failures(100)) ML_CHECK(f.find('/') == std::string::npos);

    // Not tried again, however often it is asked for and however much time passes - even when the file is
    // there by now (models loaded meanwhile would have missed it anyway).
    WriteFile(dir.path + "missing.txd", BuildTxd({"x"}));
    game.game.rejectTexture.clear();
    const int readBefore = game.game.texturesRead;
    for (int i = 0; i < 50; ++i) {
        ML_CHECK(cache.Find(missing, "x", 2000 + static_cast<uint64_t>(i) * 100000, false) == nullptr);
        ML_CHECK(cache.Find(refused, "no", 2000 + static_cast<uint64_t>(i) * 100000, false) == nullptr);
    }
    ML_CHECK_EQ(game.game.texturesRead, readBefore);
    ML_CHECK_EQ(cache.stats().failed, 6u);
    ML_CHECK_EQ(cache.Collect(1000000000, 1, nullptr), 1u);
}

// One texture the game's reader does not take: the others of the file are used.
ML_TEST(textures_one_refused_texture) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"good1", "bad", "good2"}));
    vc::TextureCache cache(fake::Api());
    const int a = cache.Add(dir.path + "a.txd");
    game.game.rejectTexture = "bad";
    ML_CHECK(cache.Find(a, "good1", 0, false) != nullptr);
    ML_CHECK(cache.Find(a, "good2", 0, false) != nullptr);
    ML_CHECK(cache.Find(a, "bad", 0, false) == nullptr);
    ML_CHECK_EQ(game.game.liveTextures, 2);
    ML_CHECK_EQ(cache.stats().failed, 0u);
    ML_CHECK(cache.Failures(10).empty());
}

// Out of memory while a file is read: the request fails without a crash and without a leak, and - unlike a
// broken file - the file is tried again a little later.
ML_TEST(textures_out_of_memory_is_retried) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "big.txd", vctest::BuildTxdSized({{"t1", 128}, {"t2", 128}, {"t3", 128}}));   // 64 KiB each
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "big.txd");
    {
        const mltest::FailLargeAllocations poor(32 * 1024);
        ML_CHECK(cache.Find(h, "t1", 10000, false) == nullptr);
    }
    vc::TextureCache::Stats st = cache.stats();
    ML_CHECK_EQ(st.loaded, 0u);
    ML_CHECK_EQ(st.failed, 0u);
    ML_CHECK_EQ(st.retries, 1u);
    ML_CHECK_EQ(game.game.liveDicts, 0);
    ML_CHECK(HasFailure(cache, "big.txd: memori tidak cukup"));

    // Memory is back, but the file is left alone until the waiting time is over: a model that asks for forty
    // textures of it does not make the cache read the file forty times.
    const int readBefore = game.game.texturesRead;
    ML_CHECK(cache.Find(h, "t1", 10001, false) == nullptr);
    ML_CHECK(cache.Find(h, "t2", 10000 + vc::TextureCache::kRetryMs - 1, false) == nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore);
    ML_CHECK_EQ(cache.stats().retries, 1u);

    ML_CHECK(cache.Find(h, "t1", 10000 + vc::TextureCache::kRetryMs, false) != nullptr);
    ML_CHECK(cache.Find(h, "t3", 10000 + vc::TextureCache::kRetryMs, false) != nullptr);
    st = cache.stats();
    ML_CHECK_EQ(st.loaded, 1u);
    ML_CHECK_EQ(st.loads, 1u);
    ML_CHECK(cache.Failures(10).empty());
}

// Memory runs out in the middle of a file: what was read is used, and the rest comes with another reading
// once somebody asks for a texture that is missing - without taking anything away from the models that
// already hold textures of the first reading.
ML_TEST(textures_partly_read_file_is_read_again) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "mixed.txd",
              vctest::BuildTxdSized({{"small1", 4}, {"big1", 128}, {"small2", 8}, {"big2", 128}}));
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "mixed.txd");
    void* held = nullptr;
    {
        const mltest::FailLargeAllocations poor(32 * 1024);
        held = cache.Find(h, "small1", 1000, true);
        ML_CHECK(held != nullptr);
        ML_CHECK(cache.Find(h, "small2", 1000, false) != nullptr);
        ML_CHECK(cache.Find(h, "big1", 1000, false) == nullptr);
    }
    vc::TextureCache::Stats st = cache.stats();
    ML_CHECK_EQ(st.loaded, 1u);
    ML_CHECK_EQ(st.partial, 1u);
    ML_CHECK_EQ(st.failed, 0u);
    ML_CHECK_EQ(st.loads, 1u);
    ML_CHECK_EQ(st.retries, 1u);
    ML_CHECK_EQ(game.game.liveTextures, 2);
    ML_CHECK(HasFailure(cache, "mixed.txd: sebagian tekstur belum termuat"));
    const uint64_t bytesPartial = st.bytes;

    // Before the waiting time is over nothing is read, whatever is asked for.
    const int readBefore = game.game.texturesRead;
    ML_CHECK(cache.Find(h, "big1", 1000 + vc::TextureCache::kRetryMs - 1, false) == nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore);
    // Afterwards a texture that is there does not cause a reading either ...
    ML_CHECK(cache.Find(h, "small2", 1000 + vc::TextureCache::kRetryMs, false) != nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore);
    // ... but one that is missing does, and now the file is complete.
    void* big = cache.Find(h, "big1", 1000 + vc::TextureCache::kRetryMs, false);
    ML_CHECK(big != nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore + 4);
    ML_CHECK(cache.Find(h, "big2", 1000 + vc::TextureCache::kRetryMs, false) != nullptr);
    st = cache.stats();
    ML_CHECK_EQ(st.loaded, 1u);
    ML_CHECK_EQ(st.partial, 0u);
    ML_CHECK_EQ(st.loads, 2u);
    ML_CHECK_EQ(st.retries, 1u);
    ML_CHECK(st.bytes > bytesPartial + 2 * 64 * 1024);
    ML_CHECK(cache.Failures(10).empty());
    ML_CHECK_EQ(game.game.liveDicts, 1);
    // The texture a model took from the first reading is still alive, next to the four of the second one.
    ML_CHECK_EQ(game.game.liveTextures, 5);
    if (held) {
        ML_CHECK_EQ(fake::Refs(held), 1);
        ML_CHECK_EQ(fake::Name(held), "small1");
        ML_CHECK(cache.Find(h, "small1", 1000 + vc::TextureCache::kRetryMs, false) != held);
        fake::Release(held);
    }
    ML_CHECK_EQ(game.game.liveTextures, 4);
    // A name the file does not have never causes another reading once the file is complete.
    ML_CHECK(cache.Find(h, "nosuch", 100000000, false) == nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore + 4);
}

// Still short of memory at the second reading: what is there stays, and the next attempt waits again.
ML_TEST(textures_partial_reading_that_does_not_improve_changes_nothing) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "mixed.txd", vctest::BuildTxdSized({{"small", 4}, {"big", 128}}));
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "mixed.txd");
    const mltest::FailLargeAllocations poor(32 * 1024);
    void* small = cache.Find(h, "small", 0, false);
    ML_CHECK(small != nullptr);
    for (int round = 1; round <= 3; ++round) {
        const uint64_t now = static_cast<uint64_t>(round) * vc::TextureCache::kRetryMs;
        ML_CHECK(cache.Find(h, "big", now, false) == nullptr);
        ML_CHECK(cache.Find(h, "big", now + 1, false) == nullptr);   // asked twice, read once
        ML_CHECK_EQ(cache.Find(h, "small", now, false), small);      // the dictionary was not replaced
        ML_CHECK_EQ(cache.stats().retries, static_cast<uint64_t>(round) + 1);
        ML_CHECK_EQ(game.game.liveDicts, 1);
        ML_CHECK_EQ(game.game.liveTextures, 1);
    }
    ML_CHECK_EQ(cache.stats().partial, 1u);
    ML_CHECK_EQ(cache.stats().loads, 1u);
    // Released like any other file; the state it had does not come back with it.
    ML_CHECK_EQ(cache.Collect(100000000, 1, nullptr), 1u);
    ML_CHECK_EQ(cache.stats().partial, 0u);
    ML_CHECK(cache.Failures(10).empty());
}

// A pinned file is never read a second time while it is loaded: the text draws keep plain pointers to its
// textures. A pinned file of which nothing could be read is tried again like any other.
ML_TEST(textures_pinned_file_is_not_replaced) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "map.txd", vctest::BuildTxdSized({{"small", 4}, {"big", 128}}));
    WriteFile(dir.path + "all.txd", vctest::BuildTxdSized({{"big", 128}}));
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "map.txd", true);
    const int all = cache.Add(dir.path + "all.txd", true);
    void* small = nullptr;
    {
        const mltest::FailLargeAllocations poor(32 * 1024);
        small = cache.Find(h, "small", 0, false);
        ML_CHECK(small != nullptr);
        ML_CHECK(cache.Find(all, "big", 0, false) == nullptr);
    }
    const int readBefore = game.game.texturesRead;
    ML_CHECK(cache.Find(h, "big", 10 * vc::TextureCache::kRetryMs, false) == nullptr);
    ML_CHECK_EQ(game.game.texturesRead, readBefore);
    ML_CHECK_EQ(cache.Find(h, "small", 10 * vc::TextureCache::kRetryMs, false), small);
    ML_CHECK(cache.Find(all, "big", 10 * vc::TextureCache::kRetryMs, false) != nullptr);
    ML_CHECK_EQ(cache.Collect(100000000, 1, nullptr), 0u);
}

// The game cannot allocate a dictionary: the same shortage, seen from the other side.
ML_TEST(textures_no_dictionary_is_retried) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"t1"}));
    vc::TextureCache cache(fake::Api());
    const int h = cache.Add(dir.path + "a.txd");
    game.game.noDictionaries = true;
    ML_CHECK(cache.Find(h, "t1", 500, false) == nullptr);
    ML_CHECK_EQ(cache.stats().failed, 0u);
    ML_CHECK_EQ(cache.stats().retries, 1u);
    ML_CHECK_EQ(cache.Failures(10).size(), 1u);
    game.game.noDictionaries = false;
    ML_CHECK(cache.Find(h, "t1", 500 + vc::TextureCache::kRetryMs, false) != nullptr);
    ML_CHECK(cache.Failures(10).empty());
}

// The game's reader runs inside the cache. Should it ask for a texture itself, it gets "not here" instead of
// a deadlock; the same goes for a collection started from in there.
ML_TEST(textures_reentry_from_the_reader) {
    fake::Scope game;
    TempDir dir;
    WriteFile(dir.path + "a.txd", BuildTxd({"t1", "t2"}));
    WriteFile(dir.path + "b.txd", BuildTxd({"u1"}));
    vc::TextureCache cache(fake::Api());
    const int a = cache.Add(dir.path + "a.txd");
    const int b = cache.Add(dir.path + "b.txd");
    ML_CHECK(cache.Find(b, "u1", 0, false) != nullptr);
    int inner = 0;
    game.game.whileReading = [&] {
        ++inner;
        ML_CHECK(cache.Find(b, "u1", 0, false) == nullptr);
        ML_CHECK(cache.Find(a, "t1", 0, false) == nullptr);
        ML_CHECK_EQ(cache.Collect(100000000, 1, nullptr), 0u);
    };
    ML_CHECK(cache.Find(a, "t2", 0, false) != nullptr);
    ML_CHECK_EQ(inner, 2);
    game.game.whileReading = nullptr;
    ML_CHECK(cache.Find(b, "u1", 0, false) != nullptr);   // and afterwards everything works as before
    ML_CHECK_EQ(cache.stats().loaded, 2u);
}

// The streaming thread and the render thread may both ask.
ML_TEST(textures_two_threads) {
    fake::Scope game;
    TempDir dir;
    std::vector<int> handles;
    vc::TextureCache cache(fake::Api());
    for (int i = 0; i < 8; ++i) {
        const std::string path = dir.path + "f" + std::to_string(i) + ".txd";
        WriteFile(path, BuildTxd({"t" + std::to_string(i), "shared"}));
        handles.push_back(cache.Add(path));
    }
    std::atomic<int> found{0};
    const auto worker = [&](int seed) {
        for (int round = 0; round < 400; ++round) {
            const int i = (round * 7 + seed) % 8;
            if (cache.Find(handles[static_cast<size_t>(i)], "shared", static_cast<uint64_t>(round), false)) ++found;
            if (round % 50 == 49) cache.Collect(static_cast<uint64_t>(round) + 1000000, 1, nullptr);
        }
    };
    std::thread first(worker, 0), second(worker, 3);
    first.join();
    second.join();
    ML_CHECK_EQ(found.load(), 800);
    ML_CHECK(cache.stats().loads >= 8u);
}

// Every texture file of the real map, when a checkout of the repository is at hand (VC_REPO=<folder>).
ML_TEST(textures_real_map_files) {
    const char* repo = getenv("VC_REPO");
    if (!repo || !*repo) {
        printf("  (skipped: VC_REPO is not set)\n");
        return;
    }
    // The script and the files do not agree on upper and lower case: found the way the client finds them.
    vc::FolderIndex folder;
    ML_CHECK(folder.Scan(std::string(repo) + "/models/vice_city/"));
    fake::Scope game;
    size_t files = 0, textures = 0, skipped = 0, notes = 0, withoutTextures = 0, dxt1 = 0, dxt35 = 0, raw = 0, alpha = 0;
    uint64_t bytes = 0;
    uint32_t largestSide = 0;
    {
        vc::TextureCache cache(fake::Api());
        for (size_t j = 0; j < vc::kTxdCount; ++j) {
            const vc::FoundFile* found = folder.Find(vc::kTxdFiles[j], true);
            if (!found) {
                printf("  not in the repository: %s\n", vc::kTxdFiles[j]);
                continue;
            }
            ++files;
            // What the converter says about the file ...
            const std::string data = mltest::ReadFile(found->path);
            ML_CHECK_EQ(data.size(), found->size);
            ml::TxdResult result;
            const bool ok = ml::ConvertTxd(reinterpret_cast<const uint8_t*>(data.data()), data.size(), 0, result);
            ML_CHECK(ok);
            skipped += result.skipped.size();
            notes += result.notes.size();
            for (const std::string& s : result.skipped) printf("  %s skipped %s\n", vc::kTxdFiles[j], s.c_str());
            for (const std::string& s : result.notes) printf("  %s: %s\n", vc::kTxdFiles[j], s.c_str());
            if (result.textures.empty()) {
                ++withoutTextures;
                printf("  %s has no usable texture\n", vc::kTxdFiles[j]);
            }
            // ... and what the cache makes of it: every converted texture can be found by its name.
            const int h = cache.Add(found->path);
            const int before = game.game.liveTextures;
            for (const ml::TxdTexture& t : result.textures) {
                ML_CHECK(cache.Find(h, t.name.c_str(), 1000, false) != nullptr);
                if (t.format == "DXT1") ++dxt1;
                else if (t.compressed) ++dxt35;
                else ++raw;
                alpha += t.alpha ? 1 : 0;
                largestSide = std::max(largestSide, std::max(t.width, t.height));
            }
            textures += static_cast<size_t>(game.game.liveTextures - before);
            // Two textures of one name in a file: the game finds the first one, like the PC game does.
            ML_CHECK(static_cast<size_t>(game.game.liveTextures - before) == result.textures.size());
            bytes += found->size;
            if (j % 16 == 15) cache.Collect(100000000, 1, nullptr);   // keep the test's memory small
        }
        const vc::TextureCache::Stats st = cache.stats();
        ML_CHECK_EQ(st.failed, withoutTextures);
        ML_CHECK_EQ(st.retries, 0u);
        ML_CHECK_EQ(st.partial, 0u);
        for (const std::string& f : cache.Failures(20)) printf("  failure: %s\n", f.c_str());
    }
    printf("  %zu files (%.1f MB), %zu textures (%zu DXT1, %zu DXT3/5, %zu uncompressed; %zu with alpha; largest side %u), "
           "%zu skipped, %zu notes\n",
           files, static_cast<double>(bytes) / (1024.0 * 1024.0), textures, dxt1, dxt35, raw, alpha, largestSide, skipped,
           notes);
    // 605 files are named by the script; DS_SIGN.txd is not in the repository.
    ML_CHECK_EQ(files, 604u);
    ML_CHECK_EQ(withoutTextures, 0u);
    ML_CHECK_EQ(skipped, 0u);
    // Four textures (two railings, a handrail, a wire grid) carry the label "DXT3" over DXT1 data; the
    // converter reads them as what they are. Numbers of commit aee29a4 of the repository.
    ML_CHECK_EQ(notes, 4u);
    ML_CHECK_EQ(textures, 8524u);
    ML_CHECK_EQ(raw, 0u);
    ML_CHECK(largestSide <= 1024);
}

// minimap.txd of the repository: the sprites of the minimap text draws ("mdl-1500:<name>"). Every name the
// minimap scripts ask for - the repository's own and server/vc_minimap_037.pwn - has to be in it.
ML_TEST(textures_real_minimap_file) {
    const char* repo = getenv("VC_REPO");
    if (!repo || !*repo) {
        printf("  (skipped: VC_REPO is not set)\n");
        return;
    }
    const std::string path = std::string(repo) + "/models/minimap.txd";
    const std::string data = mltest::ReadFile(path);
    ML_CHECK(!data.empty());
    ml::TxdResult result;
    ML_CHECK(ml::ConvertTxd(reinterpret_cast<const uint8_t*>(data.data()), data.size(), 0, result));
    for (const std::string& s : result.skipped) printf("  skipped %s\n", s.c_str());
    for (const std::string& s : result.notes) printf("  note: %s\n", s.c_str());
    ML_CHECK_EQ(result.skipped.size(), 0u);
    ML_CHECK_EQ(result.textures.size(), 25u);
    uint32_t largestSide = 0;
    size_t compressed = 0;
    for (const ml::TxdTexture& t : result.textures) {
        largestSide = std::max(largestSide, std::max(t.width, t.height));
        compressed += t.compressed ? 1 : 0;
    }
    printf("  %zu textures (%zu compressed), largest side %u\n", result.textures.size(), compressed, largestSide);

    std::vector<std::string> names;
    for (int i = 0; i <= 15; ++i) names.push_back(std::to_string(i));   // 0: open water, 1..15: the sections
    names.push_back("player_icon");
    for (const char* dir : {"n", "nw", "w", "sw", "s", "se", "e", "ne"}) names.push_back(std::string("player_icon_") + dir);
    ML_CHECK_EQ(names.size(), 25u);

    fake::Scope game;
    {
        vc::TextureCache cache(fake::Api());
        const int h = cache.Add(path, true);   // pinned, as the client keeps it
        ML_CHECK(h >= 0);
        for (const std::string& name : names) {
            const bool found = cache.Find(h, name.c_str(), 1000, false) != nullptr;
            if (!found) printf("  not in minimap.txd: %s\n", name.c_str());
            ML_CHECK(found);
        }
        ML_CHECK(cache.Find(h, "16", 1000, false) == nullptr);
        ML_CHECK_EQ(game.game.liveTextures, 25);
        // Never released, however long nobody asks.
        cache.Collect(100000000, 1, nullptr);
        ML_CHECK(cache.Find(h, "7", 100000001, false) != nullptr);
        const vc::TextureCache::Stats st = cache.stats();
        ML_CHECK_EQ(st.loaded, 1u);
        ML_CHECK_EQ(st.failed, 0u);
        ML_CHECK_EQ(st.partial, 0u);
    }
}
