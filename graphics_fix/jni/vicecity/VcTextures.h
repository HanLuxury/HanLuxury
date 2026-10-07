#pragma once
// Textures of the map models. The models come with PC texture dictionaries (.txd, Direct3D 9); a file is read
// when the game loads the first model that uses it, converted with the modloader's TxdConvert, handed to the
// game's own reader, and released again some time after the last such model has been unloaded.
//
// Everything the cache needs from the game comes in through TextureApi, so it runs in host tests against a
// fake game. ViceCity.cpp binds it to the real one.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vc {

// Game objects are opaque pointers. All entries are needed.
struct TextureApi {
    void* (*OpenMemoryStream)(const uint8_t* data, uint32_t size) = nullptr;   // RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD)
    void (*CloseStream)(void* stream) = nullptr;
    void* (*ReadTexture)(void* stream) = nullptr;                              // RwTextureGtaStreamRead
    void (*AddTextureRef)(void* texture) = nullptr;                            // ++RwTexture::refCount
    void* (*CreateDictionary)() = nullptr;                                     // RwTexDictionaryCreate
    void (*AddTexture)(void* dictionary, void* texture) = nullptr;             // RwTexDictionaryAddTexture
    void* (*FindTexture)(void* dictionary, const char* name) = nullptr;        // RwTexDictionaryFindNamedTexture
    void (*DestroyDictionary)(void* dictionary) = nullptr;                     // the way CTxdStore::RemoveTxd does it

    bool Complete() const {
        return OpenMemoryStream && CloseStream && ReadTexture && AddTextureRef && CreateDictionary && AddTexture &&
               FindTexture && DestroyDictionary;
    }
};

class TextureCache {
public:
    struct Stats {
        size_t files = 0;       // registered
        size_t loaded = 0;      // in memory now
        size_t failed = 0;      // could not be loaded; not tried again
        size_t partial = 0;     // in memory now without some of their textures (memory ran out, or a read error)
        uint64_t bytes = 0;     // texture data in memory now, as stored in the files
        uint64_t loads = 0;     // files read since the start
        uint64_t released = 0;  // files released since the start
        uint64_t retries = 0;   // readings given up or cut short for lack of memory or a read error
    };

    explicit TextureCache(const TextureApi& api);
    ~TextureCache();   // releases what is loaded; the game-side cache is never destroyed
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // A TXD file textures may be asked from; returns its handle. `pinned`: never released (for textures
    // somebody keeps plain pointers to).
    int Add(std::string path, bool pinned = false);

    // A file that could not be read in full because memory ran out, or because the storage reported an error,
    // is read again once this much time has passed and somebody asks for a texture that is not there. Every
    // other failure is final.
    static constexpr uint64_t kRetryMs = 5000;

    // Texture `name` of that file, or nullptr. The file is read the first time it is asked for. With `addRef`
    // the caller receives a reference of its own, the way RwTextureRead hands textures out. Never throws.
    void* Find(int handle, const char* name, uint64_t nowMs, bool addRef);

    // Releases every file that was not asked for during `idleMs` and that `inUse(handle)` does not claim.
    // Textures somebody still holds a reference to live on. Returns how many files were released.
    size_t Collect(uint64_t nowMs, uint64_t idleMs, const std::function<bool(int handle)>& inUse);

    Stats stats();
    // "<file>: <reason>" for the files that could not be loaded, or not in full, at most `limit`.
    std::vector<std::string> Failures(size_t limit);

private:
    struct Entry {
        std::string path;
        void* dictionary = nullptr;
        bool pinned = false;
        bool failed = false;            // for good
        bool partial = false;           // loaded, but textures are missing that another reading may bring
        uint64_t retryAt = 0;           // not to be read (again) before this time
        uint64_t lastUse = 0;
        uint64_t bytes = 0;
        size_t textures = 0;
        const char* status = nullptr;   // what went wrong; a literal, so that noting it needs no memory
    };
    // What one reading of a file gave.
    struct Reading {
        void* dictionary = nullptr;     // nullptr: nothing usable
        size_t textures = 0;
        uint64_t bytes = 0;
        bool complete = true;           // false: memory or the storage got in the way, another reading may do better
        const char* status = nullptr;
    };
    Reading Read(const Entry& e);
    void Adopt(Entry& e, const Reading& reading);
    void Drop(Entry& e);
    bool Load(Entry& e, uint64_t nowMs);
    void Reload(Entry& e, uint64_t nowMs);

    TextureApi m_api;
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
    std::vector<int> m_loaded;
    Stats m_stats;
};

}  // namespace vc
