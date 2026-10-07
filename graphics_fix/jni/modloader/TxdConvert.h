#pragma once
// Turns a PC texture dictionary (.txd with Direct3D 8/9 native textures) into one normalized Direct3D 9
// "texture native" chunk per texture, in exactly the shape the game's own D3D9 reader accepts.
// No game dependencies. The input is untrusted: every read is bounds-checked.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ml {

struct TxdTexture {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    bool alpha = false;
    bool compressed = false;
    std::string format;           // source format for the report: "DXT1", "8888", "PAL8", ...
    std::vector<uint8_t> chunk;   // one complete rwID_TEXTURENATIVE chunk
};

struct TxdLimits {
    int maxSize = 0;                                  // longest side allowed for a texture; 0 = no limit
    size_t maxTextures = 4096;                        // native textures looked at in one dictionary
    uint64_t maxOutputBytes = 256ull * 1024 * 1024;   // converted chunks handed out for one dictionary
    uint32_t maxTextureBytes = 128u * 1024 * 1024;    // one native texture as it is stored in the file
};

// Random access to a .txd file: a dictionary is read one texture at a time, never as a whole.
struct TxdSource {
    uint64_t size = 0;
    // Fills `dst` with `n` bytes from `offset`. False when that range cannot be read in full.
    std::function<bool(uint64_t offset, uint8_t* dst, size_t n)> read;
};

struct TxdReport {
    std::vector<std::string> skipped;   // "<name>: <reason>"
    std::vector<std::string> notes;     // "<name>: <remark>"
    size_t textures = 0;                // handed to the sink
    size_t outOfMemory = 0;             // left out because memory ran out while they were converted (also in `skipped`)
};

// Receives the textures one at a time, in file order, each right after it was converted; it may move from
// the texture. Returning false ends the conversion.
using TxdSink = std::function<bool(TxdTexture& texture)>;

// False when `source` is not a texture dictionary. A file that ends early still yields its complete textures.
bool ConvertTxd(const TxdSource& source, const TxdLimits& limits, const TxdSink& sink, TxdReport& report);

struct TxdResult {
    std::vector<TxdTexture> textures;
    std::vector<std::string> skipped;
    std::vector<std::string> notes;
};

// A whole dictionary from memory, with the default limits. `maxSize` as in TxdLimits. `out` is reset.
bool ConvertTxd(const uint8_t* data, size_t size, int maxSize, TxdResult& out);

// A name from a file as it may be written into a report: control characters and bytes outside ASCII become '?'.
std::string PrintableName(std::string_view name);

}  // namespace ml
