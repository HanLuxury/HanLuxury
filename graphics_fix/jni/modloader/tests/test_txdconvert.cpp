#include "mltest.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "TxdConvert.h"

namespace {

void Put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}

void Put16(std::string& s, uint16_t v) {
    for (int i = 0; i < 2; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}

uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

std::string Bytes(size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 99u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>(x >> 24);
    }
    return s;
}

std::string Chunk(uint32_t type, const std::string& body, uint32_t lib = 0x1803FFFF) {
    std::string s;
    Put32(s, type);
    Put32(s, static_cast<uint32_t>(body.size()));
    Put32(s, lib);
    return s + body;
}

struct Tex {
    uint32_t platform = 9;
    std::string name = "tex";
    std::string mask;
    uint32_t filterAddressing = 0x1102;   // linear, wrap, wrap
    uint32_t rasterFormat = 0x0500;
    uint32_t formatOrAlpha = 21;          // D3D9: D3DFORMAT, D3D8: hasAlpha
    uint16_t width = 4, height = 4;
    uint8_t depth = 32;
    uint8_t last = 0;                     // D3D9: flags, D3D8: compression
    std::string palette;                  // 1024 (PAL8) or 128 (PAL4) bytes
    std::vector<std::string> levels;
    int headerLevels = -1;                // numLevels field when it must differ from levels.size()
    bool bare = false;                    // the STRUCT is the last thing in the texture chunk: no extension after it
    int cutStruct = -1;                   // keep only this many bytes of the STRUCT payload
};

std::string NativeStruct(const Tex& t) {
    std::string s;
    Put32(s, t.platform);
    Put32(s, t.filterAddressing);
    std::string name = t.name, mask = t.mask;
    name.resize(32, '\0');
    mask.resize(32, '\0');
    s += name + mask;
    Put32(s, t.rasterFormat);
    Put32(s, t.formatOrAlpha);
    Put16(s, t.width);
    Put16(s, t.height);
    s.push_back(static_cast<char>(t.depth));
    s.push_back(static_cast<char>(t.headerLevels >= 0 ? t.headerLevels : static_cast<int>(t.levels.size())));
    s.push_back(4);
    s.push_back(static_cast<char>(t.last));
    s += t.palette;
    for (const std::string& l : t.levels) {
        Put32(s, static_cast<uint32_t>(l.size()));
        s += l;
    }
    return s;
}

std::string Native(const Tex& t, uint32_t lib = 0x1803FFFF) {
    std::string payload = NativeStruct(t);
    if (t.cutStruct >= 0) payload.resize(static_cast<size_t>(t.cutStruct));
    return Chunk(0x15, Chunk(0x01, payload, lib) + (t.bare ? std::string() : Chunk(0x03, "", lib)), lib);
}

std::string Txd(const std::vector<Tex>& textures, uint32_t lib = 0x1803FFFF) {
    std::string head;
    Put16(head, static_cast<uint16_t>(textures.size()));
    Put16(head, 2);
    std::string body = Chunk(0x01, head, lib);
    for (const Tex& t : textures) body += Native(t, lib);
    body += Chunk(0x03, "", lib);
    return Chunk(0x16, body, lib);
}

bool Convert(const std::string& txd, int maxSize, ml::TxdResult& out) {
    // Exact-size heap copy: ASan reports any read past the end of the input.
    std::vector<uint8_t> copy(txd.begin(), txd.end());
    return ml::ConvertTxd(copy.data(), copy.size(), maxSize, out);
}

// The converter as the loader uses it: through a source that is read piece by piece, with explicit limits.
struct Piecewise {
    bool ok = false;
    ml::TxdReport report;
    std::vector<ml::TxdTexture> textures;
    size_t reads = 0;
    size_t largestRead = 0;
    bool readPastEnd = false;
};

Piecewise ConvertPiecewise(const std::string& txd, const ml::TxdLimits& limits, size_t stopAfter = static_cast<size_t>(-1),
                           uint64_t failReadsFrom = static_cast<uint64_t>(-1)) {
    Piecewise p;
    ml::TxdSource source;
    source.size = txd.size();
    source.read = [&](uint64_t offset, uint8_t* dst, size_t n) {
        ++p.reads;
        p.largestRead = std::max(p.largestRead, n);
        if (offset > txd.size() || n > txd.size() - offset) {
            p.readPastEnd = true;
            return false;
        }
        if (offset + n > failReadsFrom) return false;   // an I/O error from here on
        memcpy(dst, txd.data() + offset, n);
        return true;
    };
    p.ok = ml::ConvertTxd(
        source, limits,
        [&](ml::TxdTexture& t) {
            p.textures.push_back(std::move(t));
            return p.textures.size() < stopAfter;
        },
        p.report);
    return p;
}

struct Out {
    bool ok = false;
    uint32_t platform = 0, fa = 0, rasterFormat = 0, d3dFormat = 0;
    uint32_t w = 0, h = 0, depth = 0, levels = 0, type = 0, flags = 0;
    std::string name;
    std::vector<std::string> data;
};

// Parses a produced chunk the way the game's reader walks it and checks the framing.
Out ParseOut(const std::vector<uint8_t>& c) {
    Out o;
    if (c.size() < 12 + 12 + 88 + 12) return o;
    const uint8_t* p = c.data();
    if (Get32(p) != 0x15 || Get32(p + 4) != c.size() - 12 || Get32(p + 8) != 0x1803FFFF) return o;
    if (Get32(p + 12) != 0x01 || Get32(p + 20) != 0x1803FFFF) return o;
    const size_t structLen = Get32(p + 16);
    if (structLen < 88 || 24 + structLen + 12 != c.size()) return o;
    const uint8_t* s = p + 24;
    o.platform = Get32(s);
    o.fa = Get32(s + 4);
    o.name.assign(reinterpret_cast<const char*>(s + 8), strnlen(reinterpret_cast<const char*>(s + 8), 32));
    o.rasterFormat = Get32(s + 72);
    o.d3dFormat = Get32(s + 76);
    o.w = static_cast<uint32_t>(s[80] | (s[81] << 8));
    o.h = static_cast<uint32_t>(s[82] | (s[83] << 8));
    o.depth = s[84];
    o.levels = s[85];
    o.type = s[86];
    o.flags = s[87];
    size_t at = 88;
    for (uint32_t i = 0; i < o.levels; ++i) {
        if (structLen - at < 4) return o;
        const uint32_t n = Get32(s + at);
        at += 4;
        if (n > structLen - at) return o;
        o.data.emplace_back(reinterpret_cast<const char*>(s + at), n);
        at += n;
    }
    if (at != structLen) return o;
    const uint8_t* e = s + structLen;
    if (Get32(e) != 0x03 || Get32(e + 4) != 0 || Get32(e + 8) != 0x1803FFFF) return o;
    o.ok = true;
    return o;
}

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1, m = std::max(w, h);
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

size_t DxtBytes(uint32_t w, uint32_t h, size_t block) { return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * block; }

// What the game's reader allocates for one level before it copies the level data into it without a
// size check: RQTexture::GetTextureSize(width, height, format) of the top level.
size_t GameBufferBytes(bool dxt1, bool dxt35, uint32_t w, uint32_t h) {
    const size_t cw = std::max<uint32_t>(w, 4), ch = std::max<uint32_t>(h, 4);
    if (dxt1) return cw * ch / 2;
    if (dxt35) return cw * ch;
    return static_cast<size_t>(w) * h * 4;
}

// Everything the game's reader relies on must hold for whatever ConvertTxd() produced.
void CheckOutputs(const ml::TxdResult& r) {
    for (const ml::TxdTexture& t : r.textures) {
        const Out o = ParseOut(t.chunk);
        if (!o.ok) {
            ML_CHECK(o.ok);
            return;
        }
        const bool dxt1 = o.d3dFormat == FourCC('D', 'X', 'T', '1');
        const bool dxt35 = o.d3dFormat == FourCC('D', 'X', 'T', '3') || o.d3dFormat == FourCC('D', 'X', 'T', '5');
        bool good = o.platform == 9 && o.w >= 1 && o.h >= 1 && o.w <= 4096 && o.h <= 4096 &&
                    (o.levels == 1 || o.levels == FullChain(o.w, o.h)) && (dxt1 || dxt35 || o.d3dFormat == 21) &&
                    (o.levels > 1 || (o.fa & 0xFF) <= 2) && o.w == t.width && o.h == t.height && o.levels == t.levels &&
                    t.compressed == (dxt1 || dxt35);
        // A mip chain only for sides that halve cleanly: the reader walks it down to 1x1 on its own.
        const auto pow2 = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
        good = good && (o.levels == 1 || (pow2(o.w) && pow2(o.h)));
        // Flags: "has alpha" (1) and "compressed" (8), nothing else. Bit 2 is "cube map": the reader would
        // go on to read six faces. Type 4 is a texture raster.
        good = good && (o.flags & ~9u) == 0 && ((o.flags & 8u) != 0) == (dxt1 || dxt35) && o.type == 4 &&
               o.depth == ((dxt1 || dxt35) ? 16u : 32u) && ((o.rasterFormat & 0x8000u) != 0) == (o.levels > 1);
        const size_t buffer = GameBufferBytes(dxt1, dxt35, o.w, o.h);
        uint32_t w = o.w, h = o.h;
        bool tail = false;
        for (uint32_t i = 0; i < o.levels; ++i) {
            const size_t expected = dxt1 ? DxtBytes(w, h, 8) : dxt35 ? DxtBytes(w, h, 16) : static_cast<size_t>(w) * h * 4;
            if (o.data[i].empty() && (dxt1 || dxt35) && i > 0) tail = true;
            else good = good && !tail && o.data[i].size() == expected;
            good = good && o.data[i].size() <= buffer;   // anything larger overflows the game's heap buffer
            w = std::max<uint32_t>(1, w / 2);
            h = std::max<uint32_t>(1, h / 2);
        }
        ML_CHECK(good);
        if (!good) return;
    }
}

}  // namespace

ML_TEST(txd_rejects_non_dictionaries) {
    ml::TxdResult r;
    ML_CHECK(!Convert("", 0, r));
    ML_CHECK(!Convert("short", 0, r));
    ML_CHECK(!Convert(Chunk(0x10, Bytes(64, 1)), 0, r));          // a clump, not a dictionary
    ML_CHECK(!ml::ConvertTxd(nullptr, 100, 0, r));
    ML_CHECK(Convert(Txd({}), 0, r));                             // empty dictionary is still a dictionary
    ML_CHECK(r.textures.empty());
    ML_CHECK(r.skipped.empty());
}

ML_TEST(txd_d3d9_dxt1_passes_through) {
    Tex t;
    t.name = "body";
    t.mask = "bodya";
    t.rasterFormat = 0x0200;
    t.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    t.width = 8;
    t.height = 8;
    t.depth = 16;
    t.last = 8;   // compressed, no alpha
    t.levels = {Bytes(32, 1)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const ml::TxdTexture& x = r.textures[0];
    ML_CHECK_EQ(x.name, std::string("body"));
    ML_CHECK(x.compressed && !x.alpha && x.format == "DXT1");
    const Out o = ParseOut(x.chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.platform, 9u);
    ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '1'));
    ML_CHECK_EQ(o.w, 8u);
    ML_CHECK_EQ(o.h, 8u);
    ML_CHECK_EQ(o.levels, 1u);
    ML_CHECK_EQ(o.type, 4u);
    ML_CHECK_EQ(o.flags, 8u);
    ML_CHECK_EQ(o.fa, 0x1102u);
    ML_CHECK_EQ(o.name, std::string("body"));
    ML_CHECK(o.data.size() == 1 && o.data[0] == Bytes(32, 1));
    ML_CHECK_EQ(o.rasterFormat, 0x0600u);

    t.last = 9;   // compressed + alpha
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].alpha);
    if (r.textures.size() == 1) ML_CHECK_EQ(ParseOut(r.textures[0].chunk).flags, 9u);
}

ML_TEST(txd_d3d8_dxt_becomes_d3d9) {
    Tex t;
    t.platform = 8;
    t.rasterFormat = 0x0300;
    t.formatOrAlpha = 1;   // hasAlpha
    t.width = 16;
    t.height = 8;
    t.depth = 16;
    t.last = 3;            // DXT3
    t.levels = {Bytes(DxtBytes(16, 8, 16), 2)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}, 0x0C02FFFF), 0, r));   // a Vice City era library id is fine on input
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.platform, 9u);
    ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '3'));
    ML_CHECK_EQ(o.flags, 9u);
    ML_CHECK_EQ(o.rasterFormat, 0x0500u);
    ML_CHECK(o.data.size() == 1 && o.data[0] == t.levels[0]);

    t.last = 1;            // DXT1, alpha taken from the hasAlpha field
    t.formatOrAlpha = 0;
    t.levels = {Bytes(DxtBytes(16, 8, 8), 3)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '1'));
        ML_CHECK_EQ(o.flags, 8u);
    }
    t.last = 5;            // DXT5, and DXT4 maps to it as well
    t.levels = {Bytes(DxtBytes(16, 8, 16), 4)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    if (r.textures.size() == 1) ML_CHECK_EQ(ParseOut(r.textures[0].chunk).d3dFormat, FourCC('D', 'X', 'T', '5'));
    t.last = 4;
    ML_CHECK(Convert(Txd({t}), 0, r));
    if (r.textures.size() == 1) ML_CHECK_EQ(ParseOut(r.textures[0].chunk).d3dFormat, FourCC('D', 'X', 'T', '5'));
    t.last = 7;            // unknown compression
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.empty());
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
}

// Textures of real maps (two in the Vice City conversion for SA-MP): the Direct3D format says DXT3, the data
// is DXT1. Half a DXT3 surface is not a texture; the same bytes read as DXT1 are the picture that was meant.
ML_TEST(txd_dxt3_label_over_dxt1_data_is_read_as_dxt1) {
    Tex t;
    t.name = "rail";
    t.rasterFormat = 0x0100;
    t.formatOrAlpha = FourCC('D', 'X', 'T', '3');
    t.width = 128;
    t.height = 128;
    t.depth = 16;
    t.last = 9;   // compressed + alpha
    t.levels = {Bytes(DxtBytes(128, 128, 8), 7)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.skipped.empty());
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(r.notes.size(), static_cast<size_t>(1));
    if (r.notes.size() == 1) ML_CHECK(r.notes[0].find("rail: tertulis DXT3") == 0);
    if (r.textures.size() == 1) {
        const ml::TxdTexture& x = r.textures[0];
        ML_CHECK(x.compressed && x.alpha && x.format == "DXT1");
        const Out o = ParseOut(x.chunk);
        ML_CHECK(o.ok);
        ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '1'));
        ML_CHECK_EQ(o.flags, 9u);
        ML_CHECK_EQ(o.levels, 1u);
        ML_CHECK(o.data.size() == 1 && o.data[0] == t.levels[0]);
        ML_CHECK_EQ(o.rasterFormat, 0x0500u);
    }
    CheckOutputs(r);

    // Without the alpha flag the DXT1 data is opaque, whatever the label implied.
    t.last = 8;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 && !r.textures[0].alpha);
    if (r.textures.size() == 1) ML_CHECK_EQ(ParseOut(r.textures[0].chunk).flags, 8u);
    CheckOutputs(r);

    // The same mistake with "DXT5", with a mip chain, and in a Direct3D 8 file.
    t.last = 9;
    t.formatOrAlpha = FourCC('D', 'X', 'T', '5');
    t.width = t.height = 8;
    t.rasterFormat = 0x8100;
    t.levels = {Bytes(DxtBytes(8, 8, 8), 1), Bytes(8, 2), Bytes(8, 3), Bytes(8, 4)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '1'));
        ML_CHECK_EQ(o.levels, 4u);
        ML_CHECK(o.data.size() == 4 && o.data[0] == t.levels[0] && o.data[1] == t.levels[1]);
    }
    CheckOutputs(r);

    Tex d8;
    d8.platform = 8;
    d8.rasterFormat = 0x0100;
    d8.formatOrAlpha = 1;   // hasAlpha
    d8.width = 16;
    d8.height = 8;
    d8.depth = 16;
    d8.last = 3;            // says DXT3
    d8.levels = {Bytes(DxtBytes(16, 8, 8), 5)};
    ML_CHECK(Convert(Txd({d8}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.d3dFormat, FourCC('D', 'X', 'T', '1'));
        ML_CHECK_EQ(o.flags, 9u);
    }
    CheckOutputs(r);

    // Only the exact DXT1 size is taken for it. One byte less or more is the damaged DXT3 texture it was before.
    for (const int delta : {-1, 1}) {
        t = Tex();
        t.rasterFormat = 0x0300;
        t.formatOrAlpha = FourCC('D', 'X', 'T', '3');
        t.width = t.height = 16;
        t.depth = 16;
        t.last = 9;
        t.levels = {Bytes(DxtBytes(16, 16, 8) + static_cast<size_t>(delta), 6)};
        ML_CHECK(Convert(Txd({t}), 0, r));
        ML_CHECK(r.textures.empty());
        ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
    }
    // And a level that claims the DXT1 size without having the bytes is not believed.
    t.levels = {Bytes(DxtBytes(16, 16, 8), 6)};
    t.cutStruct = 88 + 4 + 100;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.empty());
    // A real DXT3 texture is left alone.
    t.cutStruct = -1;
    t.levels = {Bytes(DxtBytes(16, 16, 16), 6)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].format == "DXT3" && r.notes.empty());
}

ML_TEST(txd_raw_32bit) {
    const std::string bgra("\x01\x02\x03\x04\x11\x12\x13\x14\x21\x22\x23\x24\x31\x32\x33\x34", 16);
    Tex t;
    t.width = 2;
    t.height = 2;
    t.levels = {bgra};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.d3dFormat, 21u);
    ML_CHECK_EQ(o.depth, 32u);
    ML_CHECK_EQ(o.rasterFormat, 0x0500u);
    ML_CHECK(o.data.size() == 1 && o.data[0] == bgra);
    ML_CHECK(r.textures[0].alpha && !r.textures[0].compressed);

    std::string opaque = bgra;
    opaque[3] = opaque[7] = opaque[11] = opaque[15] = '\xFF';
    t.formatOrAlpha = 22;   // X8R8G8B8: whatever sits in the fourth byte is not alpha
    ML_CHECK(Convert(Txd({t}), 0, r));
    if (r.textures.size() == 1) {
        o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.d3dFormat, 21u);
        ML_CHECK(o.data.size() == 1 && o.data[0] == opaque);
        ML_CHECK(!r.textures[0].alpha);
    } else {
        ML_CHECK(false);
    }

    Tex d8;                 // Direct3D 8, raster 888 stored as 32 bits
    d8.platform = 8;
    d8.rasterFormat = 0x0600;
    d8.formatOrAlpha = 0;
    d8.width = 2;
    d8.height = 2;
    d8.levels = {bgra};
    ML_CHECK(Convert(Txd({d8}), 0, r));
    if (r.textures.size() == 1) ML_CHECK(ParseOut(r.textures[0].chunk).data[0] == opaque);
    else ML_CHECK(false);

    d8.depth = 24;          // and stored as 24 bits
    d8.levels = {std::string("\x01\x02\x03\x11\x12\x13\x21\x22\x23\x31\x32\x33", 12)};
    ML_CHECK(Convert(Txd({d8}), 0, r));
    if (r.textures.size() == 1) ML_CHECK(ParseOut(r.textures[0].chunk).data[0] == opaque);
    else ML_CHECK(false);
}

ML_TEST(txd_16bit_and_luminance) {
    ml::TxdResult r;
    Tex t;
    t.width = 4;
    t.height = 1;
    t.depth = 16;

    t.formatOrAlpha = 23;   // R5G6B5: red, green, blue, white
    t.levels = {std::string("\x00\xF8\xE0\x07\x1F\x00\xFF\xFF", 8)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x00\x00\xFF\xFF\x00\xFF\x00\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF", 16));

    t.formatOrAlpha = 25;   // A1R5G5B5: opaque red, transparent red, opaque blue, transparent black
    t.levels = {std::string("\x00\xFC\x00\x7C\x1F\x80\x00\x00", 8)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x00\x00\xFF\xFF\x00\x00\xFF\x00\xFF\x00\x00\xFF\x00\x00\x00\x00", 16));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].alpha);

    t.formatOrAlpha = 24;   // X1R5G5B5: same bits, always opaque
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x00\x00\xFF\xFF\x00\x00\xFF\xFF\xFF\x00\x00\xFF\x00\x00\x00\xFF", 16));

    t.formatOrAlpha = 26;   // A4R4G4B4: opaque blue, transparent red, half green, white
    t.levels = {std::string("\x0F\xF0\x00\x0F\xF0\x80\xFF\xFF", 8)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\xFF\x00\x00\xFF\x00\x00\xFF\x00\x00\xFF\x00\x88\xFF\xFF\xFF\xFF", 16));

    t.depth = 8;
    t.formatOrAlpha = 50;   // L8
    t.levels = {std::string("\x00\x80\xFF\x10", 4)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x00\x00\x00\xFF\x80\x80\x80\xFF\xFF\xFF\xFF\xFF\x10\x10\x10\xFF", 16));

    t.depth = 16;
    t.formatOrAlpha = 51;   // A8L8: luminance byte first, alpha second
    t.levels = {std::string("\x40\x80\xFF\x00\x00\xFF\x10\x20", 8)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x40\x40\x40\x80\xFF\xFF\xFF\x00\x00\x00\x00\xFF\x10\x10\x10\x20", 16));

    Tex d8;                 // Direct3D 8 names the format through the raster format
    d8.platform = 8;
    d8.rasterFormat = 0x0200;
    d8.formatOrAlpha = 0;
    d8.width = 4;
    d8.height = 1;
    d8.depth = 16;
    d8.levels = {std::string("\x00\xF8\xE0\x07\x1F\x00\xFF\xFF", 8)};
    ML_CHECK(Convert(Txd({d8}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x00\x00\xFF\xFF\x00\xFF\x00\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF", 16));
}

ML_TEST(txd_palettes) {
    std::string pal;
    for (int i = 0; i < 256; ++i) {   // R = i, G = 255 - i, B = 7, A = i / 2
        pal.push_back(static_cast<char>(i));
        pal.push_back(static_cast<char>(255 - i));
        pal.push_back(7);
        pal.push_back(static_cast<char>(i / 2));
    }
    Tex t;
    t.platform = 8;
    t.rasterFormat = 0x2500;   // PAL8 | 8888: the palette carries alpha
    t.formatOrAlpha = 1;
    t.width = 4;
    t.height = 1;
    t.depth = 8;
    t.palette = pal;
    t.levels = {std::string("\x00\x01\xFF\x10", 4)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x07\xFF\x00\x00\x07\xFE\x01\x00\x07\x00\xFF\x7F\x07\xEF\x10\x08", 16));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].format == "PAL8");

    t.rasterFormat = 0x2600;   // PAL8 | 888: opaque
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x07\xFF\x00\xFF\x07\xFE\x01\xFF\x07\x00\xFF\xFF\x07\xEF\x10\xFF", 16));

    t.platform = 9;            // Direct3D 9 with D3DFMT_P8 uses the same layout
    t.formatOrAlpha = 41;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 && ParseOut(r.textures[0].chunk).d3dFormat == 21u);

    t.platform = 8;            // PAL4: 32 stored entries, 4-bit indices
    t.rasterFormat = 0x4500;
    t.palette = pal.substr(0, 128);
    t.levels = {std::string("\x13\x02\x1F\x00", 4)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.size() == 1 &&
             ParseOut(r.textures[0].chunk).data[0] ==
                 std::string("\x07\xFC\x03\x01\x07\xFD\x02\x01\x07\xF0\x0F\x07\x07\xFF\x00\x00", 16));

    t.palette = pal.substr(0, 100);   // palette shorter than the format needs
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.empty());
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
}

ML_TEST(txd_full_mip_chain_is_kept) {
    Tex t;
    t.width = 8;
    t.height = 8;
    t.filterAddressing = 0x1106;   // linear-mip-linear
    t.rasterFormat = 0x8500;
    t.levels = {Bytes(256, 1), Bytes(64, 2), Bytes(16, 3), Bytes(4, 4)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.levels, 4u);
    ML_CHECK_EQ(o.fa, 0x1106u);
    ML_CHECK_EQ(o.rasterFormat, 0x8500u);
    ML_CHECK(o.data.size() == 4 && o.data[0] == t.levels[0] && o.data[3] == t.levels[3]);

    Tex rect;                       // 8x2: the chain continues until both sides are 1
    rect.width = 8;
    rect.height = 2;
    rect.levels = {Bytes(64, 5), Bytes(16, 6), Bytes(8, 7), Bytes(4, 8)};
    ML_CHECK(Convert(Txd({rect}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].levels == 4);
    CheckOutputs(r);
}

ML_TEST(txd_incomplete_raw_chain_is_generated) {
    Tex t;
    t.width = 4;
    t.height = 4;
    t.filterAddressing = 0x1106;
    std::string l1;                 // 2x2, B = 10, 20, 30, 40; G = 0; R = 255; A = 1, 2, 3, 4
    const int b[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; ++i) {
        l1.push_back(static_cast<char>(b[i]));
        l1.push_back(0);
        l1.push_back(static_cast<char>(255));
        l1.push_back(static_cast<char>(i + 1));
    }
    t.levels = {Bytes(64, 1), l1};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.levels, 3u);
    ML_CHECK(o.data.size() == 3 && o.data[1] == l1);
    // 2x2 box filter with rounding: (10+20+30+40+2)/4 = 25, 0, 255, (1+2+3+4+2)/4 = 3
    ML_CHECK(o.data.size() == 3 && o.data[2] == std::string("\x19\x00\xFF\x03", 4));
    ML_CHECK_EQ(o.fa, 0x1106u);
    CheckOutputs(r);
}

ML_TEST(txd_incomplete_dxt_chain) {
    Tex t;
    t.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    t.depth = 16;
    t.last = 8;
    t.filterAddressing = 0x1106;
    ml::TxdResult r;

    t.width = 16;                   // 16, 8, 4 stored; 2 and 1 are missing: written as zero-size levels
    t.height = 16;
    t.levels = {Bytes(DxtBytes(16, 16, 8), 1), Bytes(DxtBytes(8, 8, 8), 2), Bytes(8, 3)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK(o.ok);
        ML_CHECK_EQ(o.levels, 5u);
        ML_CHECK(o.data.size() == 5 && o.data[2] == t.levels[2] && o.data[3].empty() && o.data[4].empty());
        ML_CHECK_EQ(o.fa, 0x1106u);
    }

    t.width = 64;                   // 64 and 32 stored: the rest cannot be invented, mipmaps are dropped
    t.height = 64;
    t.levels = {Bytes(DxtBytes(64, 64, 8), 4), Bytes(DxtBytes(32, 32, 8), 5)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.levels, 1u);
        ML_CHECK_EQ(o.fa, 0x1102u);   // a mip filter without mip levels would sample black
        ML_CHECK(o.data.size() == 1 && o.data[0] == t.levels[0]);
        ML_CHECK_EQ(o.rasterFormat & 0x8000u, 0u);
    }

    t.width = 8;                    // the file itself ends in zero-size levels (common for PC archives)
    t.height = 8;
    t.levels = {Bytes(32, 6), Bytes(8, 7), "", ""};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.levels, 4u);
        ML_CHECK(o.data.size() == 4 && o.data[1] == t.levels[1] && o.data[2].empty() && o.data[3].empty());
    }
    CheckOutputs(r);
}

// 4x4 block: red, blue and the two colours in between, the same in every row.
static std::string Dxt1Block(uint16_t c0, uint16_t c1, uint8_t row = 0xE4) {
    std::string b;
    Put16(b, c0);
    Put16(b, c1);
    b.append(4, static_cast<char>(row));
    return b;
}

static std::string Pixel(int b, int g, int r, int a) {
    return std::string({static_cast<char>(b), static_cast<char>(g), static_cast<char>(r), static_cast<char>(a)});
}

ML_TEST(txd_dxt_with_awkward_size_is_decoded) {
    const std::string red = Pixel(0, 0, 255, 255), blue = Pixel(255, 0, 0, 255);
    ml::TxdResult r;

    Tex t;                          // 6x6 DXT1: four blocks, of which only 6x6 pixels are used
    t.name = "six";
    t.rasterFormat = 0x0200;
    t.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    t.depth = 16;
    t.last = 8;
    t.width = 6;
    t.height = 6;
    t.filterAddressing = 0x1106;
    t.levels = {Dxt1Block(0xF800, 0x001F) + Dxt1Block(0xF800, 0x001F) + Dxt1Block(0xF800, 0x001F) +
                Dxt1Block(0xF800, 0x001F)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK(o.ok);
        ML_CHECK_EQ(o.d3dFormat, 21u);                     // plain 32-bit pixels, not DXT
        ML_CHECK_EQ(o.levels, 1u);
        ML_CHECK_EQ(o.w, 6u);
        ML_CHECK_EQ(o.h, 6u);
        ML_CHECK_EQ(o.fa, 0x1102u);
        ML_CHECK_EQ(o.flags, 0u);
        // colour 2 = (2 * red + blue) / 3, colour 3 = (red + 2 * blue) / 3, rounded
        const std::string row = red + blue + Pixel(85, 0, 170, 255) + Pixel(170, 0, 85, 255) + red + blue;
        std::string all;
        for (int y = 0; y < 6; ++y) all += row;
        ML_CHECK(o.data.size() == 1 && o.data[0] == all);
        ML_CHECK(!r.textures[0].compressed && !r.textures[0].alpha);
        ML_CHECK_EQ(r.textures[0].format, std::string("DXT1"));
        ML_CHECK_EQ(r.notes.size(), static_cast<size_t>(1));
    }
    CheckOutputs(r);

    ML_CHECK(Convert(Txd({t}), 4, r));                     // once decoded it can be reduced like any 32-bit texture
    ML_CHECK(r.textures.size() == 1 && r.textures[0].width == 3 && r.textures[0].height == 3);
    CheckOutputs(r);

    t.width = 5;                    // c0 <= c1: three colours plus "transparent"
    t.height = 5;
    t.last = 9;                     // compressed + alpha
    t.levels = {Dxt1Block(0x001F, 0xF800) + Dxt1Block(0x001F, 0xF800) + Dxt1Block(0x001F, 0xF800) +
                Dxt1Block(0x001F, 0xF800)};
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        const std::string row = blue + red + Pixel(128, 0, 128, 255) + Pixel(0, 0, 0, 0) + blue;
        std::string all;
        for (int y = 0; y < 5; ++y) all += row;
        ML_CHECK(o.data.size() == 1 && o.data[0] == all);
        ML_CHECK(r.textures[0].alpha);
        ML_CHECK_EQ(o.flags, 1u);
    }
    t.last = 8;                     // the same data without the alpha flag: the fourth colour is opaque black
    ML_CHECK(Convert(Txd({t}), 0, r));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK(o.data.size() == 1 && o.data[0].substr(12, 4) == Pixel(0, 0, 0, 255));
    } else {
        ML_CHECK(false);
    }

    Tex d3;                         // 6x4 DXT3: explicit 4-bit alpha, pixel i of a block has alpha i * 17
    d3.platform = 8;
    d3.rasterFormat = 0x0300;
    d3.formatOrAlpha = 1;
    d3.depth = 16;
    d3.last = 3;
    d3.width = 6;
    d3.height = 4;
    const std::string alpha3("\x10\x32\x54\x76\x98\xBA\xDC\xFE", 8);
    d3.levels = {alpha3 + Dxt1Block(0xFFFF, 0xFFFF) + alpha3 + Dxt1Block(0xFFFF, 0xFFFF)};
    ML_CHECK(Convert(Txd({d3}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        std::string all;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 6; ++x) all += Pixel(255, 255, 255, (y * 4 + x % 4) * 17);
        }
        ML_CHECK(o.data.size() == 1 && o.data[0] == all);
        ML_CHECK_EQ(o.d3dFormat, 21u);
    }
    CheckOutputs(r);

    Tex d5;                         // 7x4 DXT5: interpolated alpha, both table layouts
    d5.formatOrAlpha = FourCC('D', 'X', 'T', '5');
    d5.depth = 16;
    d5.last = 9;
    d5.width = 7;
    d5.height = 4;
    // 3-bit index per pixel, pixel i uses index i % 8: bits 000 001 010 011 100 101 110 111, least significant first
    const std::string index5("\x88\xC6\xFA\x88\xC6\xFA", 6);
    d5.levels = {std::string("\xFF\x00", 2) + index5 + Dxt1Block(0xFFFF, 0xFFFF) +   // a0 > a1: six steps in between
                 std::string("\x0A\xC8", 2) + index5 + Dxt1Block(0xFFFF, 0xFFFF)};   // a0 <= a1: four steps, 0 and 255
    ML_CHECK(Convert(Txd({d5}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        const int first[8] = {255, 0, 219, 182, 146, 109, 73, 36};
        const int second[8] = {10, 200, 48, 86, 124, 162, 0, 255};
        std::string all;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 7; ++x) {
                const int* table = x < 4 ? first : second;   // left block, right block
                all += Pixel(255, 255, 255, table[(y * 4 + x % 4) % 8]);
            }
        }
        ML_CHECK(o.data.size() == 1 && o.data[0] == all);
    }
    CheckOutputs(r);

    Tex keep;                       // sides that are multiples of 4, or 1 or 2, fit the game's buffer: untouched
    keep.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    keep.depth = 16;
    keep.last = 8;
    keep.width = 12;
    keep.height = 20;
    keep.levels = {Bytes(DxtBytes(12, 20, 8), 1)};
    ML_CHECK(Convert(Txd({keep}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].compressed && r.textures[0].levels == 1);
    if (r.textures.size() == 1) ML_CHECK(ParseOut(r.textures[0].chunk).data[0] == keep.levels[0]);
    ML_CHECK(r.notes.empty());
    CheckOutputs(r);
    keep.width = 2;
    keep.height = 1;
    keep.levels = {Bytes(8, 2)};
    ML_CHECK(Convert(Txd({keep}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].compressed);
    CheckOutputs(r);
}

ML_TEST(txd_non_power_of_two_keeps_one_level) {
    Tex t;
    t.width = 6;
    t.height = 10;
    t.filterAddressing = 0x1104;
    t.levels = {Bytes(240, 1), Bytes(60, 2)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK_EQ(o.levels, 1u);
    ML_CHECK_EQ(o.w, 6u);
    ML_CHECK_EQ(o.h, 10u);
    ML_CHECK_EQ(o.fa, 0x1102u);
    ML_CHECK(o.data.size() == 1 && o.data[0] == t.levels[0]);
}

ML_TEST(txd_max_size) {
    ml::TxdResult r;
    Tex dxt;
    dxt.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    dxt.depth = 16;
    dxt.last = 8;
    dxt.width = 16;
    dxt.height = 16;
    dxt.levels = {Bytes(128, 1), Bytes(32, 2), Bytes(8, 3), Bytes(8, 4), Bytes(8, 5)};

    ML_CHECK(Convert(Txd({dxt}), 8, r));              // top level dropped, the rest untouched
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.w, 8u);
        ML_CHECK_EQ(o.h, 8u);
        ML_CHECK_EQ(o.levels, 4u);
        ML_CHECK(o.data.size() == 4 && o.data[0] == dxt.levels[1] && o.data[3] == dxt.levels[4]);
    }
    ML_CHECK(r.notes.empty());

    ML_CHECK(Convert(Txd({dxt}), 0, r));              // 0 = no limit
    ML_CHECK(r.textures.size() == 1 && r.textures[0].width == 16 && r.textures[0].levels == 5);

    dxt.levels.resize(1);                             // DXT without mipmaps cannot be reduced: kept, with a note
    ML_CHECK(Convert(Txd({dxt}), 8, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].width == 16 && r.textures[0].levels == 1);
    ML_CHECK_EQ(r.notes.size(), static_cast<size_t>(1));

    Tex raw;                                          // 32-bit without mipmaps: reduced with a box filter
    raw.width = 16;
    raw.height = 8;
    raw.levels = {std::string(16 * 8 * 4, '\x40')};
    ML_CHECK(Convert(Txd({raw}), 4, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.w, 4u);
        ML_CHECK_EQ(o.h, 2u);
        ML_CHECK_EQ(o.levels, 1u);
        ML_CHECK(o.data.size() == 1 && o.data[0] == std::string(4 * 2 * 4, '\x40'));
    }
    CheckOutputs(r);

    Tex wide;                                         // wider than the game can take: the first level that fits is used
    wide.width = 8192;
    wide.height = 1;
    for (uint32_t w = 8192; w >= 1; w /= 2) wide.levels.push_back(Bytes(w * 4, w));
    ML_CHECK(Convert(Txd({wide}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out o = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(o.w, 4096u);
        ML_CHECK_EQ(o.levels, 13u);
        ML_CHECK(o.data.size() == 13 && o.data[0] == wide.levels[1]);
    }
    CheckOutputs(r);
    wide.levels.resize(1);                            // without smaller levels it is not converted at all
    ML_CHECK(Convert(Txd({wide}), 2048, r));
    ML_CHECK(r.textures.empty());
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
}

ML_TEST(txd_bad_textures_are_skipped_good_ones_kept) {
    Tex ps2;
    ps2.name = "ps2";
    ps2.platform = 0x00325350;
    ps2.levels = {Bytes(64, 1)};
    Tex cube;
    cube.name = "cube";
    cube.last = 2;
    cube.levels = {Bytes(64, 2)};
    Tex odd;
    odd.name = "odd";
    odd.formatOrAlpha = 36;   // A16B16G16R16
    odd.levels = {Bytes(128, 3)};
    Tex cut;
    cut.name = "cut";
    cut.levels = {Bytes(10, 4)};   // 4x4 needs 64 bytes
    Tex none;
    none.name = "none";
    Tex zero;
    zero.name = "zero";
    zero.width = 0;
    zero.levels = {Bytes(4, 5)};
    Tex good;
    good.name = "good";
    good.levels = {Bytes(64, 6)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({ps2, cube, odd, cut, none, zero, good}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].name == "good");
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(6));
    for (const std::string& s : r.skipped) ML_CHECK(s.find(": ") != std::string::npos);

    Tex big;                       // larger than the game can take, even after the size limit
    big.name = "big";
    big.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    big.depth = 16;
    big.last = 8;
    big.width = 8192;
    big.height = 4;
    big.levels = {Bytes(DxtBytes(8192, 4, 8), 7)};
    ML_CHECK(Convert(Txd({big, good}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
}

ML_TEST(txd_names_are_terminated) {
    Tex t;
    t.name = std::string(32, 'n');   // no terminator inside the 32-byte field
    t.mask = std::string(32, 'm');
    t.levels = {Bytes(64, 1)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    ML_CHECK_EQ(r.textures[0].name, std::string(31, 'n'));
    const std::vector<uint8_t>& c = r.textures[0].chunk;
    ML_CHECK_EQ(c[24 + 8 + 31], static_cast<uint8_t>(0));    // name[31]
    ML_CHECK_EQ(c[24 + 40 + 31], static_cast<uint8_t>(0));   // mask[31]
}

ML_TEST(txd_truncated_and_corrupted_input_is_safe) {
    Tex dxt;
    dxt.name = "dxt";
    dxt.formatOrAlpha = FourCC('D', 'X', 'T', '5');
    dxt.depth = 16;
    dxt.last = 9;
    dxt.width = 16;
    dxt.height = 16;
    dxt.levels = {Bytes(256, 1), Bytes(64, 2), Bytes(16, 3), Bytes(16, 4), Bytes(16, 5)};
    Tex pal;
    pal.name = "pal";
    pal.platform = 8;
    pal.rasterFormat = 0x2500;
    pal.depth = 8;
    pal.width = 8;
    pal.height = 8;
    pal.palette = Bytes(1024, 6);
    pal.levels = {Bytes(64, 7), Bytes(16, 8)};
    Tex rgb;
    rgb.name = "rgb";
    rgb.formatOrAlpha = 23;
    rgb.depth = 16;
    rgb.width = 8;
    rgb.height = 4;
    rgb.levels = {Bytes(64, 9)};
    Tex slack;                      // level data longer than needed: a corrupted width or height still "fits"
    slack.name = "slack";
    slack.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    slack.depth = 16;
    slack.last = 9;
    slack.width = 8;
    slack.height = 8;
    slack.levels = {Bytes(3000, 10)};
    const std::string base = Txd({dxt, pal, rgb, slack});

    ml::TxdResult r;
    ML_CHECK(Convert(base, 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(4));
    CheckOutputs(r);

    size_t withTextures = 0;
    for (size_t n = 0; n < base.size(); ++n) {   // cut at every byte: the file is shorter than its header says
        Convert(base.substr(0, n), 8, r);
        CheckOutputs(r);
        if (!r.textures.empty()) ++withTextures;
    }
    ML_CHECK(withTextures > base.size() / 2);    // the cut files really reach the texture code

    withTextures = 0;
    for (size_t n = 12; n < base.size(); ++n) {  // ... and the same with an outer length that is honest about it
        std::string cut = base.substr(0, n);
        const uint32_t length = static_cast<uint32_t>(n - 12);
        for (int i = 0; i < 4; ++i) cut[4 + i] = static_cast<char>(length >> (8 * i));
        ML_CHECK(Convert(cut, 0, r));
        CheckOutputs(r);
        if (!r.textures.empty()) ++withTextures;
    }
    ML_CHECK(withTextures > base.size() / 2);

    uint32_t x = 12345;
    const auto next = [&x]() {
        x = x * 1664525u + 1013904223u;
        return x >> 8;
    };
    size_t decoded = 0;
    for (int v = 1; v <= 40; ++v) {              // every small width and height of the texture with slack
        Tex odd = slack;
        odd.width = static_cast<uint16_t>(v);
        odd.height = static_cast<uint16_t>(41 - v);
        Convert(Txd({odd}), 0, r);
        ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
        if (r.textures.size() == 1 && !r.textures[0].compressed) ++decoded;
        CheckOutputs(r);
    }
    ML_CHECK(decoded > 20);

    for (int i = 0; i < 4000; ++i) {             // flip one to four random bytes
        std::string bad = base;
        const int flips = 1 + static_cast<int>(next() % 4);
        for (int k = 0; k < flips; ++k) bad[next() % bad.size()] = static_cast<char>(next());
        Convert(bad, (i % 3 == 0) ? 4 : 0, r);
        CheckOutputs(r);
    }
}

// The game's reader copies a level into a buffer sized from the texture's width and height without looking
// at the level's own size, and reads a fixed 88-byte header. A level or a header that is one byte short must
// never be accepted. The texture is the last thing in its chunk here, so reading past it leaves the buffer the
// converter holds it in (and the sanitizer says so).
ML_TEST(txd_level_one_byte_short_is_rejected) {
    ml::TxdResult r;
    struct Case {
        uint32_t d3dFormat;
        uint8_t depth, last;
        uint16_t w, h;
        size_t need;
    };
    const Case cases[] = {
        {21, 32, 0, 4, 4, 64},                               // 32-bit
        {23, 16, 0, 4, 4, 32},                               // 16-bit
        {50, 8, 0, 4, 4, 16},                                // 8-bit
        {FourCC('D', 'X', 'T', '1'), 16, 8, 8, 8, 32},
        {FourCC('D', 'X', 'T', '5'), 16, 9, 8, 8, 64},
        {FourCC('D', 'X', 'T', '1'), 16, 8, 6, 6, 32},       // decoded to 32-bit by the converter itself
    };
    for (const Case& c : cases) {
        Tex t;
        t.bare = true;
        t.formatOrAlpha = c.d3dFormat;
        t.depth = c.depth;
        t.last = c.last;
        t.width = c.w;
        t.height = c.h;
        t.levels = {Bytes(c.need - 1, 1)};
        ML_CHECK(Convert(Txd({t}), 0, r));
        ML_CHECK(r.textures.empty());
        ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
        t.levels = {Bytes(c.need, 1)};                       // exactly enough is fine
        ML_CHECK(Convert(Txd({t}), 0, r));
        ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
        CheckOutputs(r);
    }

    Tex chain;                                               // the second level is short: only the first one is used
    chain.bare = true;
    chain.width = 4;
    chain.height = 4;
    chain.filterAddressing = 0x1106;
    chain.levels = {Bytes(64, 2), Bytes(15, 3)};
    ML_CHECK(Convert(Txd({chain}), 0, r));
    ML_CHECK(r.textures.size() == 1 && r.textures[0].levels == 1);
    CheckOutputs(r);

    Tex pal;                                                 // a palette one byte short
    pal.bare = true;
    pal.platform = 8;
    pal.rasterFormat = 0x2500;
    pal.depth = 8;
    pal.palette = Bytes(1023, 4);
    ML_CHECK(Convert(Txd({pal}), 0, r));
    ML_CHECK(r.textures.empty());
    pal.palette = Bytes(1024, 4);                            // palette complete, but then the pixels are missing
    ML_CHECK(Convert(Txd({pal}), 0, r));
    ML_CHECK(r.textures.empty());
    pal.levels = {Bytes(16, 5)};
    ML_CHECK(Convert(Txd({pal}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
}

ML_TEST(txd_short_header_is_rejected) {
    ml::TxdResult r;
    Tex t;
    t.bare = true;
    t.levels = {Bytes(64, 1)};
    for (int n = 0; n < 88; ++n) {   // every header length below the 88 bytes the format has
        t.cutStruct = n;
        ML_CHECK(Convert(Txd({t}), 0, r));
        ML_CHECK(r.textures.empty());
        ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
    }
    t.cutStruct = 88;                // a whole header and nothing after it: no level data
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK(r.textures.empty());
    t.cutStruct = -1;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
}

// Lengths inside the file are claims. A texture is parsed from a buffer that holds exactly its own chunk, so
// a claim that reaches further is simply not believed.
ML_TEST(txd_lying_lengths_are_not_believed) {
    ml::TxdResult r;
    Tex good;
    good.name = "good";
    good.levels = {Bytes(64, 1)};
    const std::string one = Native(good);

    const auto dictionary = [](const std::string& body) { return Chunk(0x16, body); };
    const auto poke32 = [](std::string s, size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) s[at + i] = static_cast<char>(v >> (8 * i));
        return s;
    };

    // The STRUCT inside the first texture claims to be longer than the texture chunk around it.
    std::string lyingStruct = poke32(one, 12 + 4, 0x7FFFFFF0);
    ML_CHECK(Convert(dictionary(lyingStruct + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));   // the second texture is still read
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
    CheckOutputs(r);
    lyingStruct = poke32(one, 12 + 4, static_cast<uint32_t>(one.size()));   // ... by as little as its own header
    ML_CHECK(Convert(dictionary(lyingStruct + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));

    // A level claims more bytes than the STRUCT has left.
    const size_t levelSize = 12 + 12 + 88;
    std::string lyingLevel = poke32(one, levelSize, 65);
    ML_CHECK(Convert(dictionary(lyingLevel + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    lyingLevel = poke32(one, levelSize, 0xFFFFFFFF);
    ML_CHECK(Convert(dictionary(lyingLevel + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));

    // The texture chunk itself claims more than the dictionary holds: nothing after it can be located, and
    // the bytes it claims are not even asked for.
    const std::string lyingNative = poke32(one, 4, static_cast<uint32_t>(2 * one.size()));
    ML_CHECK(Convert(dictionary(one + lyingNative + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK(r.skipped.size() == 1 && r.skipped[0].find("terpotong atau rusak") != std::string::npos);
    CheckOutputs(r);
    for (const uint32_t claim : {static_cast<uint32_t>(2 * one.size()), 0x00100000u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        const Piecewise p = ConvertPiecewise(dictionary(one + poke32(one, 4, claim)), ml::TxdLimits());
        ML_CHECK(p.ok);
        ML_CHECK_EQ(p.textures.size(), static_cast<size_t>(1));
        ML_CHECK(!p.readPastEnd);
        ML_CHECK(p.largestRead <= one.size());
    }

    // The dictionary claims less than the file holds: what lies behind its end is not part of it.
    std::string shortDictionary = dictionary(one + one);
    shortDictionary = poke32(shortDictionary, 4, static_cast<uint32_t>(one.size()));
    ML_CHECK(Convert(shortDictionary, 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));

    // ... and one that claims more is read as far as the file goes, with a remark.
    std::string longDictionary = poke32(dictionary(one), 4, 0x7FFFFFFF);
    ML_CHECK(Convert(longDictionary, 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(r.notes.size(), static_cast<size_t>(1));
    ML_CHECK(r.skipped.empty());
    {
        const Piecewise p = ConvertPiecewise(longDictionary, ml::TxdLimits());
        ML_CHECK_EQ(p.textures.size(), static_cast<size_t>(1));
        ML_CHECK(!p.readPastEnd);   // the file's real size bounds every read, whatever the header says
    }
    {   // a file that stops in the middle of a texture: the same, and the half texture is reported once
        const std::string whole = dictionary(one + one);
        const Piecewise p = ConvertPiecewise(whole.substr(0, whole.size() - 40), ml::TxdLimits());
        ML_CHECK_EQ(p.textures.size(), static_cast<size_t>(1));
        ML_CHECK(!p.readPastEnd);
        ML_CHECK_EQ(p.report.skipped.size(), static_cast<size_t>(1));
        ML_CHECK_EQ(p.report.notes.size(), static_cast<size_t>(1));
    }

    // A texture chunk without a STRUCT, an empty one, and one with garbage in front of the STRUCT.
    ML_CHECK(Convert(dictionary(Chunk(0x15, Chunk(0x03, "")) + Chunk(0x15, "") + Chunk(0x15, "abc") + one), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(3));
    ML_CHECK(Convert(dictionary(Chunk(0x15, Chunk(0x03, Bytes(20, 2)) + Chunk(0x01, NativeStruct(good)))), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));   // the first STRUCT counts, wherever it stands
}

// What the game's reader does with a texture that has six-face cube data is read past this converter's
// single-face output, so the cube flag must be refused on the way in and can never appear on the way out.
ML_TEST(txd_cube_flag_never_reaches_the_output) {
    ml::TxdResult r;
    for (int last = 0; last < 256; ++last) {
        Tex t;                      // Direct3D 9: the last byte is the flags byte
        t.last = static_cast<uint8_t>(last);
        t.levels = {Bytes(64, 1)};
        ML_CHECK(Convert(Txd({t}), 0, r));
        ML_CHECK_EQ(r.textures.empty(), (last & 2) != 0);
        CheckOutputs(r);

        Tex dxt = t;                // the same byte with compressed data
        dxt.formatOrAlpha = FourCC('D', 'X', 'T', '1');
        dxt.depth = 16;
        dxt.levels = {Bytes(8, 2)};
        ML_CHECK(Convert(Txd({dxt}), 0, r));
        ML_CHECK_EQ(r.textures.empty(), (last & 2) != 0);
        CheckOutputs(r);
    }
}

static std::string Solid(uint32_t w, uint32_t h, const std::function<std::string(uint32_t, uint32_t)>& pixel) {
    std::string s;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) s += pixel(x, y);
    }
    return s;
}

// Missing mip levels of a 32-bit texture are made with a 2x2 box filter. For a texture that is not square the
// width and the height of every generated level have to be the right way round.
ML_TEST(txd_generated_chain_of_a_non_square_texture) {
    Tex t;
    t.width = 16;
    t.height = 8;
    t.filterAddressing = 0x1106;
    t.rasterFormat = 0x8500;
    // Level 1 is 8x4: blue = 16 * x, green = 60 * y, red = 255, alpha = x + y.
    const std::string l1 = Solid(8, 4, [](uint32_t x, uint32_t y) { return Pixel(16 * x, 60 * y, 255, x + y); });
    t.levels = {Bytes(16 * 8 * 4, 1), l1};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const Out o = ParseOut(r.textures[0].chunk);
    ML_CHECK(o.ok);
    ML_CHECK_EQ(o.levels, 5u);   // 16x8, 8x4, 4x2, 2x1, 1x1
    if (o.data.size() != 5) return;
    ML_CHECK(o.data[1] == l1);
    // 4x2 from 8x4: each pixel is the mean of a 2x2 block, (a + b + c + d + 2) / 4.
    //   blue  (2 * 32x + 2 * (32x + 16) + 2) / 4 = 32x + 8
    //   green (2 * 120y + 2 * (120y + 60) + 2) / 4 = 120y + 30
    //   alpha ((2x + 2y) + 2 * (2x + 2y + 1) + (2x + 2y + 2) + 2) / 4 = 2x + 2y + 1
    const std::string l2 =
        Solid(4, 2, [](uint32_t x, uint32_t y) { return Pixel(32 * x + 8, 120 * y + 30, 255, 2 * x + 2 * y + 1); });
    ML_CHECK_EQ(o.data[2].size(), static_cast<size_t>(4 * 2 * 4));
    ML_CHECK(o.data[2] == l2);
    ML_CHECK_EQ(o.data[3].size(), static_cast<size_t>(2 * 1 * 4));
    ML_CHECK_EQ(o.data[4].size(), static_cast<size_t>(4));
    // 2x1 from 4x2: blue is the mean of two columns (8 and 40, then 72 and 104); green of the two rows (30 and 150).
    ML_CHECK(o.data[3] == Pixel(24, 90, 255, 3) + Pixel(88, 90, 255, 7));
    // 1x1 from 2x1: only two pixels to average.
    ML_CHECK(o.data[4] == Pixel(56, 90, 255, 5));
    CheckOutputs(r);

    Tex tall = t;                // the same the other way round: 8x16, level 1 is 4x8
    tall.width = 8;
    tall.height = 16;
    const std::string t1 = Solid(4, 8, [](uint32_t x, uint32_t y) { return Pixel(60 * x, 16 * y, 255, x + y); });
    tall.levels = {Bytes(8 * 16 * 4, 2), t1};
    ML_CHECK(Convert(Txd({tall}), 0, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() != 1) return;
    const Out ot = ParseOut(r.textures[0].chunk);
    ML_CHECK_EQ(ot.levels, 5u);
    if (ot.data.size() != 5) return;
    const std::string t2 =
        Solid(2, 4, [](uint32_t x, uint32_t y) { return Pixel(120 * x + 30, 32 * y + 8, 255, 2 * x + 2 * y + 1); });
    ML_CHECK(ot.data[2] == t2);
    ML_CHECK_EQ(ot.data[3].size(), static_cast<size_t>(1 * 2 * 4));   // 1x2
    ML_CHECK(ot.data[3] == Pixel(90, 24, 255, 3) + Pixel(90, 88, 255, 7));
    CheckOutputs(r);

    Tex shrink;                  // TxdMaxSize on a texture without mipmaps goes through the same filter
    shrink.width = 8;
    shrink.height = 4;
    shrink.levels = {l1};
    ML_CHECK(Convert(Txd({shrink}), 4, r));
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(1));
    if (r.textures.size() == 1) {
        const Out os = ParseOut(r.textures[0].chunk);
        ML_CHECK_EQ(os.w, 4u);
        ML_CHECK_EQ(os.h, 2u);
        ML_CHECK(os.data.size() == 1 && os.data[0] == l2);
    }
}

ML_TEST(txd_limits_bound_one_dictionary) {
    std::vector<Tex> five;
    for (int i = 0; i < 5; ++i) {
        Tex t;
        t.name = "t" + std::to_string(i);
        t.levels = {Bytes(64, i)};
        five.push_back(t);
    }
    const std::string txd = Txd(five);
    ml::TxdLimits limits;

    Piecewise all = ConvertPiecewise(txd, limits);
    ML_CHECK(all.ok);
    ML_CHECK_EQ(all.textures.size(), static_cast<size_t>(5));
    ML_CHECK_EQ(all.report.textures, static_cast<size_t>(5));
    ML_CHECK(all.report.skipped.empty());
    const size_t oneChunk = all.textures.empty() ? 0 : all.textures[0].chunk.size();

    limits.maxTextures = 3;                    // the fourth texture is not even looked at
    Piecewise three = ConvertPiecewise(txd, limits);
    ML_CHECK_EQ(three.textures.size(), static_cast<size_t>(3));
    ML_CHECK_EQ(three.report.skipped.size(), static_cast<size_t>(1));
    ML_CHECK(three.report.skipped.size() == 1 && three.report.skipped[0].find("terlalu banyak tekstur") != std::string::npos);

    limits = ml::TxdLimits();
    limits.maxOutputBytes = 2 * oneChunk;      // exactly two fit: the limit is never exceeded, not even by one texture
    Piecewise two = ConvertPiecewise(txd, limits);
    ML_CHECK_EQ(two.textures.size(), static_cast<size_t>(2));
    ML_CHECK(two.report.skipped.size() == 1 && two.report.skipped[0].find("terlalu besar setelah dikonversi") != std::string::npos);
    limits.maxOutputBytes = 2 * oneChunk - 1;
    ML_CHECK_EQ(ConvertPiecewise(txd, limits).textures.size(), static_cast<size_t>(1));
    limits.maxOutputBytes = 0;
    ML_CHECK(ConvertPiecewise(txd, limits).textures.empty());

    limits = ml::TxdLimits();
    Tex big;                                   // one texture larger than a texture may be in the file
    big.name = "big";
    big.width = 64;
    big.height = 64;
    big.levels = {Bytes(64 * 64 * 4, 9)};
    std::vector<Tex> mixed = {five[0], big, five[1]};
    limits.maxTextureBytes = 4096;
    Piecewise small = ConvertPiecewise(Txd(mixed), limits);
    ML_CHECK_EQ(small.textures.size(), static_cast<size_t>(2));          // the ones around it are still read
    ML_CHECK_EQ(small.report.skipped.size(), static_cast<size_t>(1));
    ML_CHECK(small.largestRead <= 4096);                                  // and its bytes are never fetched

    limits = ml::TxdLimits();
    limits.maxSize = 2;                        // maxSize travels with the limits
    Piecewise shrunk = ConvertPiecewise(txd, limits);
    ML_CHECK(shrunk.textures.size() == 5 && shrunk.textures[0].width == 2);
}

// The loader reads a dictionary through its file descriptor, one texture at a time.
ML_TEST(txd_source_is_read_one_texture_at_a_time) {
    std::vector<Tex> textures;
    for (int i = 0; i < 6; ++i) {
        Tex t;
        t.name = "t" + std::to_string(i);
        t.width = 32;
        t.height = 32;
        t.levels = {Bytes(32 * 32 * 4, i)};
        textures.push_back(t);
    }
    const std::string txd = Txd(textures);
    const ml::TxdLimits limits;

    Piecewise all = ConvertPiecewise(txd, limits);
    ML_CHECK(all.ok);
    ML_CHECK_EQ(all.textures.size(), static_cast<size_t>(6));
    ML_CHECK(!all.readPastEnd);
    ML_CHECK(all.largestRead < txd.size() / 4);          // never the whole file, only one texture
    for (size_t i = 0; i < all.textures.size(); ++i) ML_CHECK_EQ(all.textures[i].name, "t" + std::to_string(i));

    Piecewise stopped = ConvertPiecewise(txd, limits, 2);   // the sink says stop after the second texture
    ML_CHECK(stopped.ok);
    ML_CHECK_EQ(stopped.textures.size(), static_cast<size_t>(2));
    ML_CHECK_EQ(stopped.report.textures, static_cast<size_t>(2));
    ML_CHECK(stopped.reads < all.reads);

    // The medium fails in the middle of the third texture: the first two stay, the rest is reported.
    const size_t third = txd.size() / 2 - 100;
    Piecewise broken = ConvertPiecewise(txd, limits, static_cast<size_t>(-1), third);
    ML_CHECK(broken.ok);
    ML_CHECK(!broken.textures.empty() && broken.textures.size() < 6);
    ML_CHECK_EQ(broken.report.skipped.size(), static_cast<size_t>(1));
    ML_CHECK(broken.report.skipped.size() == 1 && broken.report.skipped[0].find("tidak terbaca") != std::string::npos);

    // Not a dictionary, an unreadable header, no reader, no sink.
    ml::TxdReport report;
    ML_CHECK(!ConvertPiecewise(Chunk(0x10, "x"), limits).ok);
    ML_CHECK(!ConvertPiecewise(txd, limits, static_cast<size_t>(-1), 0).ok);
    ml::TxdSource none;
    none.size = 100;
    ML_CHECK(!ml::ConvertTxd(none, limits, [](ml::TxdTexture&) { return true; }, report));
    ml::TxdSource mem;
    mem.size = txd.size();
    mem.read = [&txd](uint64_t offset, uint8_t* dst, size_t n) {
        memcpy(dst, txd.data() + offset, n);
        return true;
    };
    ML_CHECK(!ml::ConvertTxd(mem, limits, nullptr, report));
}

// On a phone a large allocation can fail. One texture that cannot be converted must not take the rest of the
// dictionary with it, and must never take the game down.
ML_TEST(txd_allocation_failure_skips_only_that_texture) {
    Tex small1;
    small1.name = "small1";
    small1.levels = {Bytes(64, 1)};
    Tex big;                       // PAL8 1024x1024: 1 MB in the file, 4 MB once converted
    big.name = "big";
    big.platform = 8;
    big.rasterFormat = 0x2500;
    big.depth = 8;
    big.width = 1024;
    big.height = 1024;
    big.palette = Bytes(1024, 2);
    big.levels = {std::string(1024 * 1024, '\x07')};
    Tex small2 = small1;
    small2.name = "small2";
    const std::string txd = Txd({small1, big, small2});
    std::vector<uint8_t> copy(txd.begin(), txd.end());
    ml::TxdResult r;
    {
        mltest::FailLargeAllocations fail(2 * 1024 * 1024);   // the file's bytes fit, the converted pixels do not
        ML_CHECK(ml::ConvertTxd(copy.data(), copy.size(), 0, r));
    }
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(2));
    ML_CHECK(r.textures.size() == 2 && r.textures[0].name == "small1" && r.textures[1].name == "small2");
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
    ML_CHECK(r.skipped.size() == 1 && r.skipped[0].find("memori tidak cukup") != std::string::npos);
    CheckOutputs(r);
    {
        mltest::FailLargeAllocations fail(512 * 1024);        // not even the texture's bytes from the file fit
        ML_CHECK(ml::ConvertTxd(copy.data(), copy.size(), 0, r));
    }
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(2));
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));

    ML_CHECK(ml::ConvertTxd(copy.data(), copy.size(), 0, r)); // with memory available everything converts
    ML_CHECK_EQ(r.textures.size(), static_cast<size_t>(3));
}

ML_TEST(txd_names_are_made_printable_for_the_report) {
    ML_CHECK_EQ(ml::PrintableName("body_256"), std::string("body_256"));
    ML_CHECK_EQ(ml::PrintableName(std::string("a\nb\rc\td\x7F" "e\x01", 10)), std::string("a?b?c?d?e?"));
    ML_CHECK_EQ(ml::PrintableName("\xC3\xA9t\xC3\xA9"), std::string("??t??"));
    ML_CHECK_EQ(ml::PrintableName(""), std::string(""));

    Tex t;                       // a name that tries to write its own line into the report
    t.name = "x\n[Saklar]\n  Enabled";
    t.platform = 0x00325350;     // skipped: its name ends up in a message
    t.levels = {Bytes(64, 1)};
    Tex odd;                     // converted with a remark: the name ends up in a note
    odd.name = "y\nz";
    odd.formatOrAlpha = FourCC('D', 'X', 'T', '1');
    odd.depth = 16;
    odd.last = 8;
    odd.width = 6;
    odd.height = 6;
    odd.levels = {Bytes(32, 2)};
    ml::TxdResult r;
    ML_CHECK(Convert(Txd({t, odd}), 0, r));
    ML_CHECK_EQ(r.skipped.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(r.notes.size(), static_cast<size_t>(1));
    for (const std::string& s : r.skipped) ML_CHECK(s.find('\n') == std::string::npos);
    for (const std::string& s : r.notes) ML_CHECK(s.find('\n') == std::string::npos);
    ML_CHECK(r.textures.size() == 1 && r.textures[0].name == "y\nz");   // the texture keeps the name the game looks up
}
