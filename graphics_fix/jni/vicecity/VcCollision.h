#pragma once
// Collision of a SA-MP 0.3.DL model. SA-MP keeps it inside the DFF, as a COL file in an extension chunk of the
// clump (plugin id 0x253F2FF); the game itself only knows Rockstar's own chunk (0x253F2FA) and skips this one.
//
// The functions here find that chunk and decide whether the game's COL3 loader can be given the file as it
// is. No game dependencies. The input is untrusted: every read is bounds-checked, and a file passes only when
// every offset and count the loader follows stays inside it.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vc {

constexpr uint32_t kSampCollisionChunk = 0x253F2FF;
constexpr size_t kColFileHeader = 0x20;        // fourcc, size, name[22], model id
constexpr size_t kCol3Info = 0x58;             // bounds, counts, flags, offsets: what the loader copies first
constexpr size_t kMaxCollisionBytes = 8u * 1024 * 1024;
constexpr uint32_t kSurfaceTypes = 179;        // material ids the game has a table entry for

// Random access to a model file.
struct FileSource {
    uint64_t size = 0;
    // Fills `dst` with `n` bytes from `offset`. False when that range cannot be read in full.
    std::function<bool(uint64_t offset, uint8_t* dst, size_t n)> read;
};

enum class ColResult {
    Loadable,      // `file` is a COL3 file the game's loader can take
    Empty,         // a collision file without anything to collide with
    NoChunk,       // the model carries no collision
    Unsupported,   // a collision version this client does not load (the bounds are still read)
    Bad            // damaged; `detail` says how
};

struct Collision {
    std::vector<uint8_t> file;   // Loadable: the whole COL file ("COL3", size, name, ...), made safe
    bool hasBox = false;
    float box[6] = {};           // min xyz, max xyz as stored in the file
    uint32_t spheres = 0, boxes = 0, faces = 0, vertices = 0, faceGroups = 0;
    std::string detail;          // for the log, when the result is not Loadable
};

// Where SA-MP's collision chunk lies in a DFF: offset and size of its payload. False when there is none
// (`why` empty) or the file is damaged (`why` says how).
bool FindSampCollision(const FileSource& dff, uint64_t& offset, uint32_t& size, std::string& why);

// Checks a COL3 file and makes it harmless where it can: what the loader would not survive fails the check,
// what it does not need (shadow mesh, lines, face groups that do not add up, unknown surface ids) is removed.
// `file` .. `file + size` is the whole COL file; it is modified in place and `size` may shrink to the part
// the file itself declares.
ColResult CheckCol3(uint8_t* file, size_t& size, Collision& out);

// The collision of a model file, ready for the loader.
ColResult ReadCollision(const FileSource& dff, Collision& out);
ColResult ReadCollisionFile(const char* path, Collision& out);

// A FileSource over a file descriptor (pread); the descriptor stays owned by the caller.
FileSource FdSource(int fd, uint64_t size);

}  // namespace vc
