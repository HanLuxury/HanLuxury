#include "TxdConvert.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace ml {
namespace {

constexpr uint32_t kChunkStruct = 0x01;
constexpr uint32_t kChunkExtension = 0x03;
constexpr uint32_t kChunkTextureNative = 0x15;
constexpr uint32_t kChunkTexDictionary = 0x16;
constexpr uint32_t kLibraryId = 0x1803FFFF;   // RenderWare 3.6.0.3: the game's reader checks the version
constexpr uint32_t kMaxDimension = 4096;       // the largest texture the game takes
constexpr size_t kChunkHeader = 12;           // type, length, library id
constexpr size_t kNativeHeader = 88;          // platform id .. flags byte

constexpr uint32_t kRasterFormatMask = 0x0F00;
constexpr uint32_t kRaster1555 = 0x0100;
constexpr uint32_t kRaster565 = 0x0200;
constexpr uint32_t kRaster4444 = 0x0300;
constexpr uint32_t kRasterLum8 = 0x0400;
constexpr uint32_t kRaster8888 = 0x0500;
constexpr uint32_t kRaster888 = 0x0600;
constexpr uint32_t kRaster555 = 0x0A00;
constexpr uint32_t kRasterPal8 = 0x2000;
constexpr uint32_t kRasterPal4 = 0x4000;
constexpr uint32_t kRasterMipmap = 0x8000;

constexpr uint32_t kD3dA8R8G8B8 = 21;

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

enum class Src { Dxt1, Dxt3, Dxt5, Bgra8888, Bgrx8888, Bgr888, Rgb565, Argb1555, Xrgb1555, Argb4444, Lum8, AlphaLum8, Pal8, Pal4 };

struct Format {
    Src src = Src::Bgra8888;
    bool alpha = false;
    bool paletteAlpha = false;
};

bool IsDxt(Src s) { return s == Src::Dxt1 || s == Src::Dxt3 || s == Src::Dxt5; }

const char* SrcName(Src s) {
    switch (s) {
        case Src::Dxt1: return "DXT1";
        case Src::Dxt3: return "DXT3";
        case Src::Dxt5: return "DXT5";
        case Src::Bgra8888: return "8888";
        case Src::Bgrx8888: return "888";
        case Src::Bgr888: return "888 (24-bit)";
        case Src::Rgb565: return "565";
        case Src::Argb1555: return "1555";
        case Src::Xrgb1555: return "555";
        case Src::Argb4444: return "4444";
        case Src::Lum8: return "LUM8";
        case Src::AlphaLum8: return "A8L8";
        case Src::Pal8: return "PAL8";
        case Src::Pal4: return "PAL4";
    }
    return "?";
}

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint32_t U16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void Put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

struct Chunk {
    uint32_t type = 0;
    size_t body = 0;
    size_t length = 0;
};

// Chunk header at `at`; the whole chunk must lie inside [0, limit).
bool ReadChunk(const uint8_t* data, size_t limit, size_t at, Chunk& c) {
    if (at > limit || limit - at < 12) return false;
    c.type = U32(data + at);
    const uint32_t length = U32(data + at + 4);
    c.body = at + 12;
    if (length > limit - c.body) return false;
    c.length = length;
    return true;
}

size_t LevelBytes(Src s, uint32_t w, uint32_t h) {
    const size_t blocks = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4);
    const size_t pixels = static_cast<size_t>(w) * h;
    switch (s) {
        case Src::Dxt1: return blocks * 8;
        case Src::Dxt3:
        case Src::Dxt5: return blocks * 16;
        case Src::Bgra8888:
        case Src::Bgrx8888: return pixels * 4;
        case Src::Bgr888: return pixels * 3;
        case Src::Rgb565:
        case Src::Argb1555:
        case Src::Xrgb1555:
        case Src::Argb4444:
        case Src::AlphaLum8: return pixels * 2;
        case Src::Lum8:
        case Src::Pal8:
        case Src::Pal4: return pixels;
    }
    return 0;
}

uint8_t Expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t Expand6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

// One level to B,G,R,A bytes (the order the game's reader expects for D3DFMT_A8R8G8B8).
void ToBgra(const Format& f, const uint8_t* in, size_t pixels, const uint8_t* palette, uint8_t* out) {
    for (size_t i = 0; i < pixels; ++i, out += 4) {
        switch (f.src) {
            case Src::Bgra8888:
                memcpy(out, in + i * 4, 4);
                break;
            case Src::Bgrx8888:
                memcpy(out, in + i * 4, 3);
                out[3] = 255;
                break;
            case Src::Bgr888:
                memcpy(out, in + i * 3, 3);
                out[3] = 255;
                break;
            case Src::Rgb565: {
                const uint32_t v = U16(in + i * 2);
                out[0] = Expand5(v & 31);
                out[1] = Expand6((v >> 5) & 63);
                out[2] = Expand5((v >> 11) & 31);
                out[3] = 255;
                break;
            }
            case Src::Argb1555:
            case Src::Xrgb1555: {
                const uint32_t v = U16(in + i * 2);
                out[0] = Expand5(v & 31);
                out[1] = Expand5((v >> 5) & 31);
                out[2] = Expand5((v >> 10) & 31);
                out[3] = (f.src == Src::Xrgb1555 || (v & 0x8000)) ? 255 : 0;
                break;
            }
            case Src::Argb4444: {
                const uint32_t v = U16(in + i * 2);
                out[0] = static_cast<uint8_t>((v & 15) * 17);
                out[1] = static_cast<uint8_t>(((v >> 4) & 15) * 17);
                out[2] = static_cast<uint8_t>(((v >> 8) & 15) * 17);
                out[3] = static_cast<uint8_t>(((v >> 12) & 15) * 17);
                break;
            }
            case Src::Lum8:
                out[0] = out[1] = out[2] = in[i];
                out[3] = 255;
                break;
            case Src::AlphaLum8:
                out[0] = out[1] = out[2] = in[i * 2];
                out[3] = in[i * 2 + 1];
                break;
            case Src::Pal8:
            case Src::Pal4: {
                const uint8_t* c = palette + 4 * static_cast<size_t>(f.src == Src::Pal4 ? (in[i] & 15) : in[i]);   // R,G,B,A
                out[0] = c[2];
                out[1] = c[1];
                out[2] = c[0];
                out[3] = f.paletteAlpha ? c[3] : 255;
                break;
            }
            default:
                out[0] = out[1] = out[2] = out[3] = 255;
                break;
        }
    }
}

// Halves a B,G,R,A image with a 2x2 box filter; each side stays at least 1.
std::vector<uint8_t> Halve(const std::vector<uint8_t>& src, uint32_t w, uint32_t h) {
    const uint32_t nw = std::max<uint32_t>(1, w / 2), nh = std::max<uint32_t>(1, h / 2);
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
        const size_t y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
        for (uint32_t x = 0; x < nw; ++x) {
            const size_t x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
            const uint8_t* a = &src[(y0 * w + x0) * 4];
            const uint8_t* b = &src[(y0 * w + x1) * 4];
            const uint8_t* c = &src[(y1 * w + x0) * 4];
            const uint8_t* d = &src[(y1 * w + x1) * 4];
            uint8_t* o = &out[(static_cast<size_t>(y) * nw + x) * 4];
            for (int k = 0; k < 4; ++k) o[k] = static_cast<uint8_t>((a[k] + b[k] + c[k] + d[k] + 2) / 4);
        }
    }
    return out;
}

// The game's reader allocates width * height / 2 bytes (DXT1) or width * height bytes (DXT3/5), with each
// side raised to at least 4, and copies a level into that buffer without checking its size. Partial 4x4
// blocks do not fit, so a side must be a multiple of 4 (or 1 or 2, which the game rounds up itself).
bool DxtSideFits(uint32_t v) { return v % 4 == 0 || v == 1 || v == 2; }

void Rgb565ToBgr(uint32_t v, int out[3]) {
    out[0] = Expand5(v & 31);
    out[1] = Expand6((v >> 5) & 63);
    out[2] = Expand5((v >> 11) & 31);
}

// One DXT1/DXT3/DXT5 level (LevelBytes() bytes) to B,G,R,A; `out` holds w * h pixels.
void DecodeDxt(const Format& f, const uint8_t* in, uint32_t w, uint32_t h, uint8_t* out) {
    const uint32_t blocksX = (w + 3) / 4, blocksY = (h + 3) / 4;
    const size_t blockBytes = f.src == Src::Dxt1 ? 8 : 16;
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            const uint8_t* block = in + (static_cast<size_t>(by) * blocksX + bx) * blockBytes;
            const uint8_t* colour = f.src == Src::Dxt1 ? block : block + 8;
            const uint32_t c0 = U16(colour), c1 = U16(colour + 2);
            int pal[4][4];
            Rgb565ToBgr(c0, pal[0]);
            Rgb565ToBgr(c1, pal[1]);
            pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
            if (f.src != Src::Dxt1 || c0 > c1) {
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
                    pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
                }
            } else {   // three colours and "transparent black"
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = (pal[0][k] + pal[1][k] + 1) / 2;
                    pal[3][k] = 0;
                }
                pal[3][3] = f.alpha ? 0 : 255;
            }

            int alpha[16];
            if (f.src == Src::Dxt3) {
                for (int i = 0; i < 16; ++i) alpha[i] = ((block[i / 2] >> ((i & 1) * 4)) & 15) * 17;
            } else if (f.src == Src::Dxt5) {
                int table[8];
                table[0] = block[0];
                table[1] = block[1];
                if (table[0] > table[1]) {
                    for (int k = 2; k < 8; ++k) table[k] = ((8 - k) * table[0] + (k - 1) * table[1] + 3) / 7;
                } else {
                    for (int k = 2; k < 6; ++k) table[k] = ((6 - k) * table[0] + (k - 1) * table[1] + 2) / 5;
                    table[6] = 0;
                    table[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = table[(bits >> (3 * i)) & 7];
            }

            const uint32_t index = U32(colour + 4);
            for (uint32_t py = 0; py < 4; ++py) {
                const uint32_t y = by * 4 + py;
                if (y >= h) break;
                for (uint32_t px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4 + px;
                    if (x >= w) break;
                    const uint32_t i = py * 4 + px;
                    const int* c = pal[(index >> (2 * i)) & 3];
                    uint8_t* o = out + (static_cast<size_t>(y) * w + x) * 4;
                    o[0] = static_cast<uint8_t>(c[0]);
                    o[1] = static_cast<uint8_t>(c[1]);
                    o[2] = static_cast<uint8_t>(c[2]);
                    o[3] = static_cast<uint8_t>(f.src == Src::Dxt1 ? c[3] : alpha[i]);
                }
            }
        }
    }
}

bool FromRaster(uint32_t rasterFormat, uint8_t depth, Format& f) {
    const uint32_t base = rasterFormat & kRasterFormatMask;
    if (rasterFormat & (kRasterPal8 | kRasterPal4)) {
        f.src = (rasterFormat & kRasterPal8) ? Src::Pal8 : Src::Pal4;
        f.paletteAlpha = base == kRaster8888;
        f.alpha = f.paletteAlpha;
        return true;
    }
    switch (base) {
        case kRaster8888: f.src = Src::Bgra8888; f.alpha = true; return true;
        case kRaster888: f.src = depth == 24 ? Src::Bgr888 : Src::Bgrx8888; return true;
        case kRaster1555: f.src = Src::Argb1555; f.alpha = true; return true;
        case kRaster565: f.src = Src::Rgb565; return true;
        case kRaster4444: f.src = Src::Argb4444; f.alpha = true; return true;
        case kRasterLum8: f.src = Src::Lum8; return true;
        case kRaster555: f.src = Src::Xrgb1555; return true;
        default: return false;
    }
}

bool PickFormat(uint32_t platform, uint32_t rasterFormat, uint32_t formatOrAlpha, uint8_t depth, uint8_t last, Format& f,
                std::string& why) {
    f = Format();
    if (platform == 9) {
        if (last & 2) {
            why = "cube map tidak didukung";
            return false;
        }
        if (rasterFormat & (kRasterPal8 | kRasterPal4)) return FromRaster(rasterFormat, depth, f);
        switch (formatOrAlpha) {
            case FourCC('D', 'X', 'T', '1'): f.src = Src::Dxt1; f.alpha = (last & 1) != 0; return true;
            case FourCC('D', 'X', 'T', '2'):
            case FourCC('D', 'X', 'T', '3'): f.src = Src::Dxt3; f.alpha = true; return true;
            case FourCC('D', 'X', 'T', '4'):
            case FourCC('D', 'X', 'T', '5'): f.src = Src::Dxt5; f.alpha = true; return true;
            case 20: f.src = Src::Bgr888; return true;
            case 21: f.src = Src::Bgra8888; f.alpha = true; return true;
            case 22: f.src = Src::Bgrx8888; return true;
            case 23: f.src = Src::Rgb565; return true;
            case 24: f.src = Src::Xrgb1555; return true;
            case 25: f.src = Src::Argb1555; f.alpha = true; return true;
            case 26: f.src = Src::Argb4444; f.alpha = true; return true;
            case 50: f.src = Src::Lum8; return true;
            case 51: f.src = Src::AlphaLum8; f.alpha = true; return true;
            case 0:   // some tools leave the D3D format empty; the raster format still says what it is
                if (FromRaster(rasterFormat, depth, f)) return true;
                break;
            default:
                break;
        }
        why = "format D3D9 " + std::to_string(formatOrAlpha) + " tidak didukung";
        return false;
    }
    switch (last) {   // Direct3D 8: the last byte is the DXT type
        case 0: break;
        case 1: f.src = Src::Dxt1; f.alpha = formatOrAlpha != 0; return true;
        case 2:
        case 3: f.src = Src::Dxt3; f.alpha = true; return true;
        case 4:
        case 5: f.src = Src::Dxt5; f.alpha = true; return true;
        default:
            why = "kompresi D3D8 " + std::to_string(last) + " tidak didukung";
            return false;
    }
    if (FromRaster(rasterFormat, depth, f)) return true;
    why = "format raster tidak dikenal";
    return false;
}

bool IsPow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1, m = std::max(w, h);
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

struct Level {
    const uint8_t* data;
    uint32_t w, h;
};

// `p` .. `p + n` is the payload of the STRUCT chunk of one native texture. On success `t` is the texture;
// otherwise `why` says why it is skipped. `label` is the name for the report as soon as it is known.
bool ConvertNative(const uint8_t* p, size_t n, int maxSize, TxdTexture& t, std::string& label, std::string& why,
                   std::vector<std::string>& notes) {
    label = "(tanpa nama)";
    if (n < kNativeHeader) {
        why = "kepala tekstur terpotong";
        return false;
    }
    char name[32], mask[32];
    memcpy(name, p + 8, 32);
    memcpy(mask, p + 40, 32);
    name[31] = mask[31] = '\0';
    if (name[0]) label = PrintableName(name);

    const uint32_t platform = U32(p);
    if (platform != 8 && platform != 9) {
        why = "bukan tekstur PC (platform " + std::to_string(platform) + ")";
        return false;
    }
    const uint32_t filterAddressing = U32(p + 4);
    const uint32_t rasterFormat = U32(p + 72);
    const uint32_t formatOrAlpha = U32(p + 76);
    uint32_t w = U16(p + 80), h = U16(p + 82);
    const uint8_t depth = p[84];
    const uint32_t numLevels = p[85];
    const uint8_t last = p[87];

    Format fmt;
    if (!PickFormat(platform, rasterFormat, formatOrAlpha, depth, last, fmt, why)) return false;
    if (w == 0 || h == 0) {
        why = "ukuran 0";
        return false;
    }
    if (numLevels == 0) {
        why = "tidak punya level";
        return false;
    }

    size_t at = kNativeHeader;
    const uint8_t* palette = nullptr;
    if (fmt.src == Src::Pal8 || fmt.src == Src::Pal4) {
        const size_t paletteBytes = fmt.src == Src::Pal8 ? 1024 : 128;
        if (n - at < paletteBytes) {
            why = "palet terpotong";
            return false;
        }
        palette = p + at;
        at += paletteBytes;
    }

    // Some tools write "DXT3" or "DXT5" over a texture whose data is DXT1: exactly half the bytes those
    // formats need for its size. The PC game shows such a texture torn (it fills half a DXT3 surface with
    // DXT1 blocks); read as what it is, the picture is whole.
    if ((fmt.src == Src::Dxt3 || fmt.src == Src::Dxt5) && n - at >= 4) {
        const uint32_t firstSize = U32(p + at);
        if (firstSize == LevelBytes(Src::Dxt1, w, h) && firstSize <= n - at - 4) {
            notes.push_back(label + ": tertulis " + SrcName(fmt.src) + " tetapi datanya berukuran DXT1, dibaca sebagai DXT1");
            fmt.src = Src::Dxt1;
            fmt.alpha = platform == 9 ? (last & 1) != 0 : formatOrAlpha != 0;
        }
    }

    // Levels that are completely present. A zero-size or short level ends the list.
    std::vector<Level> levels;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < numLevels; ++i) {
        if (n - at < 4) break;
        const uint32_t size = U32(p + at);
        at += 4;
        if (size > n - at || size < LevelBytes(fmt.src, lw, lh)) break;
        levels.push_back({p + at, lw, lh});
        at += size;
        if (lw == 1 && lh == 1) break;
        lw = std::max<uint32_t>(1, lw / 2);
        lh = std::max<uint32_t>(1, lh / 2);
    }
    if (levels.empty()) {
        why = "data tekstur terpotong";
        return false;
    }

    // TxdMaxSize, and never more than the game can take: start from the first stored level that is small enough.
    const uint32_t limit = maxSize > 0 ? std::min<uint32_t>(static_cast<uint32_t>(maxSize), kMaxDimension) : kMaxDimension;
    size_t first = 0;
    while (first + 1 < levels.size() && std::max(levels[first].w, levels[first].h) > limit) ++first;
    w = levels[first].w;
    h = levels[first].h;

    // Nothing larger than this is converted or handed to the game (and nothing larger is allocated here).
    if (w > kMaxDimension || h > kMaxDimension) {
        why = "lebih besar dari 4096 piksel";
        return false;
    }

    bool dxt = IsDxt(fmt.src);
    std::vector<std::vector<uint8_t>> lv;
    if (dxt && !(DxtSideFits(w) && DxtSideFits(h))) {
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        DecodeDxt(fmt, levels[first].data, w, h, bgra.data());
        lv.push_back(std::move(bgra));
        dxt = false;
        notes.push_back(label + ": DXT dengan sisi bukan kelipatan 4, diubah ke 32-bit");
    } else if (dxt) {
        for (size_t i = first; i < levels.size(); ++i) {
            lv.emplace_back(levels[i].data, levels[i].data + LevelBytes(fmt.src, levels[i].w, levels[i].h));
        }
        if (std::max(w, h) > limit) {
            notes.push_back(label + ": lebih besar dari TxdMaxSize, dibiarkan (DXT tanpa mipmap tidak bisa diperkecil)");
        }
    } else {
        for (size_t i = first; i < levels.size(); ++i) {
            const size_t pixels = static_cast<size_t>(levels[i].w) * levels[i].h;
            std::vector<uint8_t> bgra(pixels * 4);
            ToBgra(fmt, levels[i].data, pixels, palette, bgra.data());
            lv.push_back(std::move(bgra));
        }
    }
    while (!dxt && std::max(w, h) > limit) {   // no stored level is small enough: shrink the smallest one
        lv[0] = Halve(lv[0], w, h);
        lv.resize(1);
        w = std::max<uint32_t>(1, w / 2);
        h = std::max<uint32_t>(1, h / 2);
    }

    // The game's reader takes either one level or the full chain down to 1x1.
    const uint32_t full = FullChain(w, h);
    if (!IsPow2(w) || !IsPow2(h) || lv.size() == 1) {
        lv.resize(1);
    } else if (lv.size() >= full) {
        lv.resize(full);
    } else {
        uint32_t cw = w, ch = h;   // size of the last stored level
        for (size_t i = 1; i < lv.size(); ++i) {
            cw = std::max<uint32_t>(1, cw / 2);
            ch = std::max<uint32_t>(1, ch / 2);
        }
        if (!dxt) {
            while (lv.size() < full) {
                lv.push_back(Halve(lv.back(), cw, ch));
                cw = std::max<uint32_t>(1, cw / 2);
                ch = std::max<uint32_t>(1, ch / 2);
            }
        } else if (std::max(cw, ch) <= 4) {
            lv.resize(full);       // zero-size levels: the reader reuses the last 4x4 block for them
        } else {
            lv.resize(1);          // the missing levels cannot be made without re-encoding
        }
    }

    uint32_t fa = filterAddressing;
    if (lv.size() == 1 && (fa & 0xFF) > 2) fa = (fa & ~0xFFu) | 2u;   // a mip filter without mip levels samples black

    t = TxdTexture();
    t.name = name;
    t.width = w;
    t.height = h;
    t.levels = static_cast<uint32_t>(lv.size());
    t.alpha = fmt.alpha;
    t.compressed = dxt;
    t.format = SrcName(fmt.src);

    size_t payload = kNativeHeader;
    for (const std::vector<uint8_t>& l : lv) payload += 4 + l.size();
    std::vector<uint8_t>& c = t.chunk;
    c.reserve(kChunkHeader + kChunkHeader + payload + kChunkHeader);
    Put32(c, kChunkTextureNative);
    Put32(c, static_cast<uint32_t>(kChunkHeader + payload + kChunkHeader));
    Put32(c, kLibraryId);
    Put32(c, kChunkStruct);
    Put32(c, static_cast<uint32_t>(payload));
    Put32(c, kLibraryId);
    Put32(c, 9);
    Put32(c, fa);
    c.insert(c.end(), name, name + 32);
    c.insert(c.end(), mask, mask + 32);
    // The game's reader treats every 32-bit texture as having alpha; for DXT1 the flag below decides.
    const bool rasterAlpha = dxt ? fmt.alpha : true;
    Put32(c, (rasterAlpha ? kRaster8888 : kRaster888) | (lv.size() > 1 ? kRasterMipmap : 0));
    if (!dxt) Put32(c, kD3dA8R8G8B8);
    else Put32(c, FourCC('D', 'X', 'T', fmt.src == Src::Dxt1 ? '1' : fmt.src == Src::Dxt3 ? '3' : '5'));
    Put16(c, w);
    Put16(c, h);
    c.push_back(dxt ? 16 : 32);
    c.push_back(static_cast<uint8_t>(lv.size()));
    c.push_back(4);   // rwRASTERTYPETEXTURE
    // Only "has alpha" (1) and "compressed" (8). The cube map bit (2) must never get here: the game's reader
    // would read six faces.
    c.push_back(static_cast<uint8_t>((fmt.alpha ? 1 : 0) | (dxt ? 8 : 0)));
    for (const std::vector<uint8_t>& l : lv) {
        Put32(c, static_cast<uint32_t>(l.size()));
        c.insert(c.end(), l.begin(), l.end());
    }
    Put32(c, kChunkExtension);
    Put32(c, 0);
    Put32(c, kLibraryId);
    return true;
}

}  // namespace

std::string PrintableName(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) c = '?';
    }
    return out;
}

bool ConvertTxd(const TxdSource& source, const TxdLimits& limits, const TxdSink& sink, TxdReport& report) {
    report = TxdReport();
    uint8_t head[kChunkHeader];
    if (!source.read || !sink || source.size < kChunkHeader || !source.read(0, head, kChunkHeader) ||
        U32(head) != kChunkTexDictionary) {
        return false;
    }
    // A file that is shorter than its own header says is read as far as it goes.
    const uint64_t claimed = kChunkHeader + static_cast<uint64_t>(U32(head + 4));
    const uint64_t end = std::min<uint64_t>(claimed, source.size);
    if (claimed > source.size) report.notes.push_back("(file): lebih pendek dari yang tertulis di kepalanya (terpotong?)");

    uint64_t at = kChunkHeader;
    uint64_t produced = 0;
    size_t seen = 0;
    while (end - at >= kChunkHeader) {
        if (!source.read(at, head, kChunkHeader)) {
            report.skipped.push_back("(sisanya): file tidak terbaca");
            break;
        }
        const uint32_t type = U32(head);
        const uint64_t length = U32(head + 4);
        const uint64_t bodyAt = at + kChunkHeader;
        if (length > end - bodyAt) {   // claims more than the dictionary holds: nothing after it can be trusted
            report.skipped.push_back("(sisanya): file terpotong atau rusak");
            break;
        }
        at = bodyAt + length;
        if (type != kChunkTextureNative) continue;

        if (seen >= limits.maxTextures) {
            report.skipped.push_back("(sisanya): terlalu banyak tekstur dalam satu TXD");
            break;
        }
        ++seen;
        const std::string ordinal = "(tekstur ke-" + std::to_string(seen) + ")";
        if (length > limits.maxTextureBytes) {
            report.skipped.push_back(ordinal + ": terlalu besar");
            continue;
        }
        try {
            // Exactly the bytes of this texture and nothing else: whatever its inner lengths claim, the parser
            // cannot leave this buffer.
            std::vector<uint8_t> body(static_cast<size_t>(length));
            if (length != 0 && !source.read(bodyAt, body.data(), body.size())) {
                report.skipped.push_back("(sisanya): file tidak terbaca");
                break;
            }
            // The texture itself is the first STRUCT chunk inside the native texture chunk.
            Chunk part;
            size_t inner = 0;
            bool found = false;
            while (!found && ReadChunk(body.data(), body.size(), inner, part)) {
                inner = part.body + part.length;
                found = part.type == kChunkStruct;
            }
            if (!found) {
                report.skipped.push_back(ordinal + ": tekstur tanpa data");
                continue;
            }
            TxdTexture texture;
            std::string label, why;
            if (!ConvertNative(body.data() + part.body, part.length, limits.maxSize, texture, label, why, report.notes)) {
                report.skipped.push_back(label + ": " + why);
                continue;
            }
            if (texture.chunk.size() > limits.maxOutputBytes - std::min(produced, limits.maxOutputBytes)) {
                report.skipped.push_back("(sisanya): TXD terlalu besar setelah dikonversi");
                break;
            }
            produced += texture.chunk.size();
            std::vector<uint8_t>().swap(body);   // the source bytes are no longer needed while the sink works
            ++report.textures;
            if (!sink(texture)) break;
        } catch (const std::bad_alloc&) {
            ++report.outOfMemory;
            report.skipped.push_back(ordinal + ": memori tidak cukup untuk mengonversi");
        }
    }
    return true;
}

bool ConvertTxd(const uint8_t* data, size_t size, int maxSize, TxdResult& out) {
    out = TxdResult();
    if (!data) return false;
    TxdSource source;
    source.size = size;
    source.read = [data, size](uint64_t offset, uint8_t* dst, size_t n) {
        if (offset > size || n > size - offset) return false;
        if (n != 0) memcpy(dst, data + offset, n);
        return true;
    };
    TxdLimits limits;
    limits.maxSize = maxSize;
    TxdReport report;
    const bool ok = ConvertTxd(
        source, limits,
        [&out](TxdTexture& texture) {
            out.textures.push_back(std::move(texture));
            return true;
        },
        report);
    out.skipped = std::move(report.skipped);
    out.notes = std::move(report.notes);
    return ok;
}

}  // namespace ml
