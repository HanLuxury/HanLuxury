#include "VcCollision.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vc {
namespace {

constexpr uint32_t kChunkExtension = 0x03;
constexpr uint32_t kChunkClump = 0x10;
constexpr int kMaxTopLevelChunks = 16;      // a UV animation dictionary may come before the clump
constexpr int kMaxClumpChildren = 4096;
constexpr int kMaxPlugins = 1024;
constexpr float kMaxCoordinate = 100000.0f;

// What the game's loader (CFileLoader::LoadCollisionModelVer3, libGTASA.so 2.10 arm64) reads, relative to the
// start of the "info" block that follows the 32-byte file header.
constexpr size_t kOffSpheres = 0x28;        // uint16 counts, read as signed
constexpr size_t kOffBoxes = 0x2A;
constexpr size_t kOffFaces = 0x2C;
constexpr size_t kOffLines = 0x2E;          // uint8
constexpr size_t kOffFlags = 0x30;
constexpr size_t kOffSphereData = 0x34;     // offsets, counted from byte 4 of the file
constexpr size_t kOffBoxData = 0x38;
constexpr size_t kOffLineData = 0x3C;
constexpr size_t kOffVertexData = 0x40;
constexpr size_t kOffFaceData = 0x44;
constexpr size_t kOffPlaneData = 0x48;
constexpr size_t kOffShadowFaces = 0x4C;
constexpr size_t kOffShadowVertexData = 0x50;
constexpr size_t kOffShadowFaceData = 0x54;

constexpr uint32_t kFlagNotEmpty = 0x02;
constexpr uint32_t kFlagFaceGroups = 0x08;

constexpr size_t kSphereBytes = 20;         // centre, radius, surface
constexpr size_t kBoxBytes = 28;            // min, max, surface
constexpr size_t kFaceBytes = 8;            // a, b, c, material, light
constexpr size_t kVertexBytes = 6;          // 3 x int16
constexpr size_t kGroupBytes = 28;          // min, max, first face, last face
constexpr uint32_t kMaxCount = 0x7FFF;      // the loader reads the counts as int16

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint32_t U16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

float F32(const uint8_t* p) {
    const uint32_t bits = U32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

bool Sane(float v) { return std::isfinite(v) && std::fabs(v) <= kMaxCoordinate; }

bool SaneFloats(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!Sane(F32(p + 4 * i))) return false;
    }
    return true;
}

struct Chunk {
    uint32_t type = 0;
    uint64_t body = 0;
    uint32_t size = 0;
};

// Chunk header at `at`; the whole chunk must lie inside [at, end).
bool ReadChunk(const FileSource& f, uint64_t at, uint64_t end, Chunk& c) {
    if (at > end || end - at < 12) return false;
    uint8_t header[12];
    if (!f.read(at, header, sizeof(header))) return false;
    c.type = U32(header);
    c.size = U32(header + 4);
    c.body = at + 12;
    return c.size <= end - c.body;
}

void ReadBox(const uint8_t* minMax, Collision& out) {
    for (int i = 0; i < 6; ++i) out.box[i] = F32(minMax + 4 * i);
    out.hasBox = true;
}

// Version 1 ("COLL"): radius, centre, min, max, then every section with its own count in front.
ColResult CheckColl(const uint8_t* file, size_t size, Collision& out) {
    const size_t bounds = kColFileHeader;
    if (size < bounds + 40 || !SaneFloats(file + bounds, 10)) {
        out.detail = "collision versi 1 rusak";
        return ColResult::Bad;
    }
    ReadBox(file + bounds + 16, out);
    size_t at = bounds + 40;
    const auto count = [&](size_t itemBytes, uint32_t& n) {
        if (size - at < 4) return false;
        n = U32(file + at);
        at += 4;
        if (static_cast<uint64_t>(n) * itemBytes > size - at) return false;
        at += static_cast<size_t>(n) * itemBytes;
        return true;
    };
    uint32_t spheres = 0, unused = 0, boxes = 0, vertices = 0, faces = 0;
    if (!count(20, spheres) || !count(0, unused) || !count(28, boxes) || !count(12, vertices) || !count(16, faces)) {
        out.detail = "collision versi 1 terpotong";
        return ColResult::Bad;
    }
    if (spheres == 0 && boxes == 0 && faces == 0) return ColResult::Empty;
    out.detail = "collision versi 1 (COLL) berisi data: tidak didukung";
    return ColResult::Unsupported;
}

}  // namespace

bool FindSampCollision(const FileSource& dff, uint64_t& offset, uint32_t& size, std::string& why) {
    why.clear();
    if (!dff.read || dff.size < 12) {
        why = "bukan file DFF";
        return false;
    }
    Chunk clump;
    bool haveClump = false;
    uint64_t at = 0;
    for (int i = 0; i < kMaxTopLevelChunks && at < dff.size; ++i) {
        if (!ReadChunk(dff, at, dff.size, clump)) break;
        if (clump.type == kChunkClump) {
            haveClump = true;
            break;
        }
        at = clump.body + clump.size;
    }
    if (!haveClump) {
        why = "tidak ada clump";
        return false;
    }
    const uint64_t clumpEnd = clump.body + clump.size;
    at = clump.body;
    for (int i = 0; i < kMaxClumpChildren && at < clumpEnd; ++i) {
        Chunk child;
        if (!ReadChunk(dff, at, clumpEnd, child)) {
            why = "isi clump terpotong";
            return false;
        }
        if (child.type == kChunkExtension) {
            const uint64_t end = child.body + child.size;
            uint64_t p = child.body;
            for (int j = 0; j < kMaxPlugins && p < end; ++j) {
                Chunk plugin;
                if (!ReadChunk(dff, p, end, plugin)) {
                    why = "extension clump terpotong";
                    return false;
                }
                if (plugin.type == kSampCollisionChunk) {
                    offset = plugin.body;
                    size = plugin.size;
                    return true;
                }
                p = plugin.body + plugin.size;
            }
        }
        at = child.body + child.size;
    }
    return false;
}

ColResult CheckCol3(uint8_t* file, size_t& size, Collision& out) {
    out.spheres = out.boxes = out.faces = out.vertices = out.faceGroups = 0;
    if (size < kColFileHeader + kCol3Info || memcmp(file, "COL3", 4) != 0) {
        out.detail = "bukan file COL3 yang utuh";
        return ColResult::Bad;
    }
    // The file says how long it is; nothing behind that is looked at.
    const uint64_t declared = static_cast<uint64_t>(U32(file + 4)) + 8;
    if (declared > size || declared < kColFileHeader + kCol3Info) {
        out.detail = "ukuran di kepala COL3 tidak cocok dengan datanya";
        return ColResult::Bad;
    }
    const size_t total = static_cast<size_t>(declared);
    const size_t dataStart = kColFileHeader + kCol3Info;
    uint8_t* info = file + kColFileHeader;

    if (!SaneFloats(info, 10)) {
        out.detail = "batas (bounding box) tidak wajar";
        return ColResult::Bad;
    }
    ReadBox(info, out);

    const uint32_t spheres = U16(info + kOffSpheres);
    const uint32_t boxes = U16(info + kOffBoxes);
    const uint32_t faces = U16(info + kOffFaces);
    uint32_t flags = U32(info + kOffFlags);
    const uint32_t sphereData = U32(info + kOffSphereData);
    const uint32_t boxData = U32(info + kOffBoxData);
    const uint32_t vertexData = U32(info + kOffVertexData);
    const uint32_t faceData = U32(info + kOffFaceData);

    // What the game does not need from these files: lines, the unused plane table, the shadow mesh.
    info[kOffLines] = 0;
    PutU32(info + kOffLineData, 0);
    PutU32(info + kOffPlaneData, 0);
    PutU32(info + kOffShadowFaces, 0);
    PutU32(info + kOffShadowVertexData, 0);
    PutU32(info + kOffShadowFaceData, 0);
    flags &= kFlagNotEmpty | kFlagFaceGroups;

    if (spheres > kMaxCount || boxes > kMaxCount || faces > kMaxCount) {
        out.detail = "jumlah bola/kotak/segitiga melebihi 32767";
        return ColResult::Bad;
    }
    if (!(flags & kFlagNotEmpty) || (spheres == 0 && boxes == 0 && faces == 0)) {
        size = total;
        return ColResult::Empty;
    }

    // An offset counts from byte 4 of the file and has to point behind the info block: the loader turns it
    // into a pointer into its own copy of the data, which starts there.
    const auto inside = [&](uint32_t off, uint64_t bytes) {
        const uint64_t at = static_cast<uint64_t>(off) + 4;
        return off != 0 && at >= dataStart && at <= total && bytes <= total - at;
    };

    if (spheres > 0) {
        if (!inside(sphereData, static_cast<uint64_t>(spheres) * kSphereBytes)) {
            out.detail = "data bola di luar file";
            return ColResult::Bad;
        }
        uint8_t* p = file + sphereData + 4;
        for (uint32_t i = 0; i < spheres; ++i, p += kSphereBytes) {
            if (!SaneFloats(p, 4)) {
                out.detail = "bola collision tidak wajar";
                return ColResult::Bad;
            }
            if (p[16] >= kSurfaceTypes) p[16] = 0;
        }
    } else {
        PutU32(info + kOffSphereData, 0);
    }

    if (boxes > 0) {
        if (!inside(boxData, static_cast<uint64_t>(boxes) * kBoxBytes)) {
            out.detail = "data kotak di luar file";
            return ColResult::Bad;
        }
        uint8_t* p = file + boxData + 4;
        for (uint32_t i = 0; i < boxes; ++i, p += kBoxBytes) {
            if (!SaneFloats(p, 6)) {
                out.detail = "kotak collision tidak wajar";
                return ColResult::Bad;
            }
            if (p[24] >= kSurfaceTypes) p[24] = 0;
        }
    } else {
        PutU32(info + kOffBoxData, 0);
    }

    uint32_t vertices = 0;
    if (faces > 0) {
        if (!inside(faceData, static_cast<uint64_t>(faces) * kFaceBytes)) {
            out.detail = "data segitiga di luar file";
            return ColResult::Bad;
        }
        // The loader takes the number of vertices from the largest index the triangles use.
        uint8_t* p = file + faceData + 4;
        uint32_t highest = 0;
        for (uint32_t i = 0; i < faces; ++i, p += kFaceBytes) {
            highest = std::max(highest, std::max(U16(p), std::max(U16(p + 2), U16(p + 4))));
            if (p[6] >= kSurfaceTypes) p[6] = 0;
        }
        vertices = highest + 1;
        if (!inside(vertexData, static_cast<uint64_t>(vertices) * kVertexBytes)) {
            out.detail = "data titik di luar file";
            return ColResult::Bad;
        }
    } else {
        PutU32(info + kOffVertexData, 0);   // with an offset the loader would read one vertex anyway
        PutU32(info + kOffFaceData, 0);
        flags &= ~kFlagFaceGroups;
    }

    // Face groups: a count right in front of the triangles and, in front of that, one bounding box with a
    // first and a last triangle per group. The game walks them backwards from the count and reads the two
    // triangle numbers as signed. Groups that do not add up are switched off; every triangle is tested then.
    uint32_t groups = 0;
    if (flags & kFlagFaceGroups) {
        const uint64_t facesAt = static_cast<uint64_t>(faceData) + 4;
        bool ok = facesAt >= dataStart + 4 + kGroupBytes;
        if (ok) {
            groups = U32(file + facesAt - 4);
            ok = groups >= 1 && groups <= faces &&
                 static_cast<uint64_t>(groups) * kGroupBytes <= facesAt - 4 - dataStart;
        }
        if (ok) {
            const uint8_t* g = file + facesAt - 4 - static_cast<size_t>(groups) * kGroupBytes;
            for (uint32_t i = 0; ok && i < groups; ++i, g += kGroupBytes) {
                const uint32_t first = U16(g + 24), last = U16(g + 26);
                ok = SaneFloats(g, 6) && first <= last && last < faces;
            }
        }
        if (!ok) {
            flags &= ~kFlagFaceGroups;
            groups = 0;
        }
    }

    PutU32(info + kOffFlags, flags);
    out.spheres = spheres;
    out.boxes = boxes;
    out.faces = faces;
    out.vertices = vertices;
    out.faceGroups = groups;
    size = total;
    return ColResult::Loadable;
}

ColResult ReadCollision(const FileSource& dff, Collision& out) {
    out = Collision();
    uint64_t offset = 0;
    uint32_t chunkSize = 0;
    std::string why;
    if (!FindSampCollision(dff, offset, chunkSize, why)) {
        if (why.empty()) return ColResult::NoChunk;
        out.detail = why;
        return ColResult::Bad;
    }
    if (chunkSize < kColFileHeader || chunkSize > kMaxCollisionBytes) {
        out.detail = "ukuran chunk collision tidak wajar";
        return ColResult::Bad;
    }
    try {
        out.file.resize(chunkSize);
    } catch (const std::bad_alloc&) {
        out.detail = "memori tidak cukup";
        return ColResult::Bad;
    }
    if (!dff.read(offset, out.file.data(), chunkSize)) {
        out.file.clear();
        out.detail = "file tidak bisa dibaca";
        return ColResult::Bad;
    }

    ColResult result;
    if (memcmp(out.file.data(), "COL3", 4) == 0) {
        size_t size = out.file.size();
        result = CheckCol3(out.file.data(), size, out);
        if (result == ColResult::Loadable) out.file.resize(size);
    } else if (memcmp(out.file.data(), "COLL", 4) == 0) {
        result = CheckColl(out.file.data(), out.file.size(), out);
    } else if (memcmp(out.file.data(), "COL2", 4) == 0 || memcmp(out.file.data(), "COL4", 4) == 0) {
        if (out.file.size() >= kColFileHeader + 40 && SaneFloats(out.file.data() + kColFileHeader, 10)) {
            ReadBox(out.file.data() + kColFileHeader, out);
        }
        out.detail = "collision versi COL2/COL4: tidak didukung";
        result = ColResult::Unsupported;
    } else {
        out.detail = "chunk collision tidak dikenal";
        result = ColResult::Bad;
    }
    if (result != ColResult::Loadable) {
        out.file.clear();
        out.file.shrink_to_fit();
    }
    return result;
}

FileSource FdSource(int fd, uint64_t size) {
    FileSource source;
    source.size = size;
    source.read = [fd, size](uint64_t offset, uint8_t* dst, size_t n) {
        if (offset > size || n > size - offset) return false;
        size_t done = 0;
        while (done < n) {
            const ssize_t r = pread64(fd, dst + done, n - done, static_cast<off64_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (r == 0) return false;
            done += static_cast<size_t>(r);
        }
        return true;
    };
    return source;
}

ColResult ReadCollisionFile(const char* path, Collision& out) {
    out = Collision();
    const int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        out.detail = "file tidak bisa dibuka";
        return ColResult::Bad;
    }
    struct stat st {};
    ColResult result;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        out.detail = "bukan file biasa";
        result = ColResult::Bad;
    } else {
        result = ReadCollision(FdSource(fd, static_cast<uint64_t>(st.st_size)), out);
    }
    close(fd);
    return result;
}

}  // namespace vc
