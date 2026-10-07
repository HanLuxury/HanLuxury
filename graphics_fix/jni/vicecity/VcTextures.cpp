#include "VcTextures.h"

#include <cerrno>
#include <new>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../modloader/TxdConvert.h"

namespace vc {
namespace {

// Depth of TextureCache calls on this thread. The game's texture reader runs inside Load(); should it ever ask
// for a texture itself, that request is answered with "not here" instead of waiting for a lock this thread
// already holds.
thread_local int t_depth = 0;

struct DepthGuard {
    DepthGuard() { ++t_depth; }
    ~DepthGuard() { --t_depth; }
};

struct FileDescriptor {
    int fd;
    explicit FileDescriptor(int f) : fd(f) {}
    ~FileDescriptor() {
        if (fd >= 0) close(fd);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
};

// False when the range cannot be read in full; `ioError` is set when that is the storage's fault and not the
// file's end.
bool PreadAll(int fd, uint64_t offset, uint8_t* dst, size_t n, bool& ioError) {
    size_t done = 0;
    while (done < n) {
        const ssize_t r = pread64(fd, dst + done, n - done, static_cast<off64_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            ioError = true;
            return false;
        }
        if (r == 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

// open() failures that say nothing about the file itself.
bool IsPassingError(int error) {
    return error == EMFILE || error == ENFILE || error == ENOMEM || error == EINTR || error == EAGAIN || error == EIO;
}

const char* BaseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

}  // namespace

TextureCache::TextureCache(const TextureApi& api) : m_api(api) {}

TextureCache::~TextureCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int handle : m_loaded) {
        Entry& e = m_entries[static_cast<size_t>(handle)];
        if (e.dictionary && m_api.DestroyDictionary) m_api.DestroyDictionary(e.dictionary);
        e.dictionary = nullptr;
    }
    m_loaded.clear();
}

int TextureCache::Add(std::string path, bool pinned) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry e;
    e.path = std::move(path);
    e.pinned = pinned;
    m_entries.push_back(std::move(e));
    m_stats.files = m_entries.size();
    return static_cast<int>(m_entries.size() - 1);
}

// m_mutex is held. The file is read one texture at a time: each one is converted, handed to the game's reader
// and released before the next one is fetched. Never throws; a dictionary it returns belongs to the caller.
TextureCache::Reading TextureCache::Read(const Entry& e) {
    Reading out;
    const FileDescriptor file(open(e.path.c_str(), O_RDONLY | O_CLOEXEC));
    if (file.fd < 0) {
        out.complete = !IsPassingError(errno);
        out.status = out.complete ? "file tidak bisa dibuka" : "file tidak bisa dibuka saat ini, dicoba lagi nanti";
        return out;
    }
    struct stat st {};
    if (fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        out.status = "bukan file biasa";
        return out;
    }

    bool isDictionary = false;
    bool outOfMemory = false;
    bool noDictionary = false;
    bool ioError = false;
    size_t converted = 0;
    try {
        ml::TxdSource source;
        source.size = static_cast<uint64_t>(st.st_size);
        const int fd = file.fd;
        source.read = [fd, &ioError](uint64_t offset, uint8_t* dst, size_t n) { return PreadAll(fd, offset, dst, n, ioError); };
        const ml::TxdLimits limits;   // the map's textures are at most 1024 pixels wide: no size limit of our own
        ml::TxdReport report;
        isDictionary = ml::ConvertTxd(
            source, limits,
            [&](ml::TxdTexture& t) {
                if (!out.dictionary) out.dictionary = m_api.CreateDictionary();
                if (!out.dictionary) {
                    noDictionary = true;
                    return false;
                }
                void* stream = m_api.OpenMemoryStream(t.chunk.data(), static_cast<uint32_t>(t.chunk.size()));
                void* texture = stream ? m_api.ReadTexture(stream) : nullptr;
                if (stream) m_api.CloseStream(stream);
                if (texture) {
                    m_api.AddTexture(out.dictionary, texture);
                    ++out.textures;
                    out.bytes += t.chunk.size();
                }
                std::vector<uint8_t>().swap(t.chunk);
                return true;
            },
            report);
        converted = report.textures;
        outOfMemory = report.outOfMemory > 0;
    } catch (...) {
        outOfMemory = true;   // whatever is already in the dictionary is complete and usable
    }

    // The game could not allocate its dictionary: the same shortage, seen from the other side.
    const bool shortOfMemory = outOfMemory || noDictionary;
    out.complete = !shortOfMemory && !ioError;
    if (out.textures == 0) {
        if (out.dictionary) m_api.DestroyDictionary(out.dictionary);
        out.dictionary = nullptr;
        if (shortOfMemory) out.status = "memori tidak cukup, dicoba lagi nanti";
        else if (ioError) out.status = "file tidak terbaca, dicoba lagi nanti";
        else if (!isDictionary) out.status = "bukan file TXD";
        else if (converted == 0) out.status = "tidak ada tekstur yang bisa dipakai";
        else out.status = "tidak ada tekstur yang diterima pembaca game";
    } else if (shortOfMemory) {
        out.status = "sebagian tekstur belum termuat karena memori tidak cukup, dicoba lagi nanti";
    } else if (ioError) {
        out.status = "sebagian tekstur belum terbaca, dicoba lagi nanti";
    }
    return out;
}

// m_mutex is held.
void TextureCache::Adopt(Entry& e, const Reading& reading) {
    e.dictionary = reading.dictionary;
    e.textures = reading.textures;
    e.bytes = reading.bytes;
    e.partial = !reading.complete;
    e.status = reading.status;
    m_stats.bytes += e.bytes;
    if (e.partial) ++m_stats.partial;
}

// m_mutex is held. Textures somebody still holds leave the dictionary and live on.
void TextureCache::Drop(Entry& e) {
    m_api.DestroyDictionary(e.dictionary);
    e.dictionary = nullptr;
    m_stats.bytes -= e.bytes;
    if (e.partial) --m_stats.partial;
    e.bytes = 0;
    e.textures = 0;
    e.partial = false;
    e.status = nullptr;
}

// m_mutex is held. The first reading of a file, or another one after a failure that may have passed.
bool TextureCache::Load(Entry& e, uint64_t nowMs) {
    try {
        m_loaded.reserve(m_loaded.size() + 1);   // nothing below may fail once the game has built the dictionary
    } catch (...) {
        e.status = "memori tidak cukup, dicoba lagi nanti";
        e.retryAt = nowMs + kRetryMs;
        ++m_stats.retries;
        return false;
    }
    const Reading reading = Read(e);
    if (!reading.complete) {
        e.retryAt = nowMs + kRetryMs;
        ++m_stats.retries;
    }
    if (!reading.dictionary) {
        e.status = reading.status;
        if (reading.complete) {
            e.failed = true;
            ++m_stats.failed;
        }
        return false;
    }
    Adopt(e, reading);
    m_loaded.push_back(static_cast<int>(&e - m_entries.data()));   // room was reserved above
    ++m_stats.loads;
    m_stats.loaded = m_loaded.size();
    return true;
}

// m_mutex is held. Another reading of a file that is in memory without some of its textures. What it gives
// replaces what is there when it is more; the textures of the old reading that models hold stay with them.
void TextureCache::Reload(Entry& e, uint64_t nowMs) {
    const Reading reading = Read(e);
    if (reading.dictionary && (reading.complete || reading.textures > e.textures)) {
        Drop(e);
        Adopt(e, reading);
        ++m_stats.loads;
    } else if (reading.dictionary) {
        m_api.DestroyDictionary(reading.dictionary);
    }
    if (e.partial) {
        e.retryAt = nowMs + kRetryMs;
        ++m_stats.retries;
    }
}

void* TextureCache::Find(int handle, const char* name, uint64_t nowMs, bool addRef) {
    if (t_depth > 0 || !name || !*name || !m_api.Complete()) return nullptr;
    const DepthGuard depth;
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (handle < 0 || static_cast<size_t>(handle) >= m_entries.size()) return nullptr;
        Entry& e = m_entries[static_cast<size_t>(handle)];
        if (!e.dictionary) {
            if (e.failed || nowMs < e.retryAt || !Load(e, nowMs)) return nullptr;
        }
        e.lastUse = nowMs;
        void* texture = m_api.FindTexture(e.dictionary, name);
        // A pinned file is not read a second time: somebody may hold plain pointers into its dictionary.
        if (!texture && e.partial && !e.pinned && nowMs >= e.retryAt) {
            Reload(e, nowMs);
            texture = m_api.FindTexture(e.dictionary, name);
        }
        if (texture && addRef) m_api.AddTextureRef(texture);
        return texture;
    } catch (...) {
        return nullptr;   // called from a hook, with the game's own frames above
    }
}

size_t TextureCache::Collect(uint64_t nowMs, uint64_t idleMs, const std::function<bool(int handle)>& inUse) {
    if (t_depth > 0 || !m_api.Complete()) return 0;
    const DepthGuard depth;
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t kept = 0, released = 0;
    for (size_t i = 0; i < m_loaded.size(); ++i) {
        const int handle = m_loaded[i];
        Entry& e = m_entries[static_cast<size_t>(handle)];
        if (e.pinned || nowMs - e.lastUse < idleMs || (inUse && inUse(handle))) {
            m_loaded[kept++] = handle;
            continue;
        }
        Drop(e);
        e.retryAt = 0;   // the next request reads the file afresh
        ++released;
    }
    m_loaded.resize(kept);
    m_stats.loaded = kept;
    m_stats.released += released;
    return released;
}

TextureCache::Stats TextureCache::stats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

std::vector<std::string> TextureCache::Failures(size_t limit) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const Entry& e : m_entries) {
        if (!e.status) continue;
        if (out.size() >= limit) break;
        out.push_back(std::string(BaseName(e.path)) + ": " + e.status);
    }
    return out;
}

}  // namespace vc
