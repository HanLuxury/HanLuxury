#include "VcArchive.h"

#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "../modloader/ModIndex.h"

namespace vc {

bool IsArchivePath(const char* gamePath) { return gamePath && ml::PathEquals(gamePath, kArchiveKey); }

bool Archive::Build(const std::vector<MapModel>& models, std::string& why) {
    m_overlay.reset();
    m_files.clear();

    size_t count = 0;
    for (const MapModel& m : models) count += (m.gameId >= 0 && m.dff) ? 1 : 0;
    if (count == 0) {
        why = "tidak ada model untuk arsip";
        return false;
    }
    m_files.reserve(count);   // the overlay keeps pointers to the elements
    for (const MapModel& m : models) {
        if (m.gameId < 0 || !m.dff) continue;
        ml::ModFile file;
        file.path = m.dff->path;
        file.base = std::string(m.name) + ".dff";
        file.rel = file.base;
        file.size = m.dff->size;
        // What the game's directory reader takes: a known extension, at most 20 characters before the only
        // dot, not empty, and no larger than its streaming buffer may become.
        if (ml::ClassifyStreamFile(file, true) != ml::StreamVerdict::Yes) {
            why = "file " + file.path + " tidak bisa dimasukkan ke arsip";
            m_files.clear();
            return false;
        }
        m_files.push_back(std::move(file));
    }

    std::vector<const ml::ModFile*> entries;
    entries.reserve(m_files.size());
    for (const ml::ModFile& f : m_files) entries.push_back(&f);
    std::vector<ml::Rejected> rejected;
    std::vector<const ml::ModFile*> spill;
    auto overlay = std::make_unique<ml::Overlay>();
    // Nothing lies under the overlay: an archive of no bytes, read from a file that always can be opened.
    if (!overlay->Build(kArchiveName, "/dev/null", {}, 0, {}, entries, {}, rejected, spill) || !spill.empty() ||
        overlay->loose().size() != m_files.size()) {
        why = "arsip model tidak bisa disusun (terlalu besar?)";
        m_files.clear();
        return false;
    }
    m_overlay = std::move(overlay);
    return true;
}

FILE* Archive::Open() const { return m_overlay ? ml::OpenOverlayFile(m_overlay.get(), nullptr) : nullptr; }

bool Archive::SelfTest(std::string& why) const {
    why.clear();
    if (!m_overlay) {
        why = "arsip model belum disusun";
        return false;
    }
    FILE* f = Open();
    if (!f) {
        why = "FILE virtual untuk arsip model tidak bisa dibuat";
        return false;
    }
    struct Closer {
        FILE* file;
        ~Closer() { fclose(file); }
    } closer{f};

    // Size, the way the game's OS_FileSize() asks for it.
    if (fseek(f, 0, SEEK_END) != 0 || static_cast<uint64_t>(ftell(f)) != sizeBytes() || fseek(f, 0, SEEK_SET) != 0) {
        why = "ukuran arsip model lewat fseek/ftell tidak cocok";
        return false;
    }
    // Directory, the way CStreaming::LoadCdDirectory() reads it: 4 + 4 + count * 32 bytes.
    const std::vector<uint8_t>& directory = m_overlay->directory();
    const size_t entryBytes = 32 * static_cast<size_t>(m_overlay->entryCount());
    if (directory.size() < 8 + entryBytes) {
        why = "direktori arsip model tidak lengkap";
        return false;
    }
    std::vector<uint8_t> got(8 + entryBytes);
    if (fread(got.data(), 1, 4, f) != 4 || fread(got.data() + 4, 1, 4, f) != 4 ||
        fread(got.data() + 8, 1, entryBytes, f) != entryBytes) {
        why = "direktori arsip model tidak terbaca lewat FILE virtual";
        return false;
    }
    if (memcmp(got.data(), directory.data(), got.size()) != 0) {
        why = "direktori arsip model terbaca berbeda lewat FILE virtual";
        return false;
    }

    // Models: seek + read of one sector, like the game's streaming thread, against the file itself.
    const std::vector<ml::Overlay::Loose>& loose = m_overlay->loose();
    std::vector<uint8_t> read(ml::kSector), want(ml::kSector);
    const auto check = [&](size_t index) {
        const ml::Overlay::Loose& l = loose[index];
        const int fd = open(l.file->path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return true;   // the file is gone: says nothing about the FILE*
        const bool haveSource = ml::detail::PreadFill(fd, 0, want.data(), ml::kSector);
        close(fd);
        if (!haveSource) return true;
        if (fseek(f, static_cast<long>(l.offset) * static_cast<long>(ml::kSector), SEEK_SET) != 0 ||
            fread(read.data(), 1, ml::kSector, f) != ml::kSector) {
            why = "isi arsip model tidak terbaca lewat FILE virtual";
            return false;
        }
        if (read != want) {
            why = "isi arsip model terbaca berbeda dari file " + l.file->path;
            return false;
        }
        return true;
    };
    const size_t step = loose.size() > 16 ? loose.size() / 16 : 1;
    for (size_t i = 0; i < loose.size(); i += step) {
        if (!check(i)) return false;
    }
    return loose.empty() || check(loose.size() - 1);
}

}  // namespace vc
