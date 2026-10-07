#pragma once
// Helpers for the Vice City host tests: the modloader's harness, plus builders for the files the map is made
// of (a DFF with SA-MP's embedded collision, a COL3 file, a PC texture dictionary).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../modloader/tests/mltest.h"

namespace vctest {

inline void Put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v));
    s.push_back(static_cast<char>(v >> 8));
}

inline void Put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}

inline void PutF(std::string& s, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    Put32(s, bits);
}

inline void Set32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
}

inline uint32_t Get32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[at + static_cast<size_t>(i)])) << (8 * i);
    return v;
}

inline std::string Chunk(uint32_t type, const std::string& body, uint32_t library = 0x1803FFFF) {
    std::string out;
    Put32(out, type);
    Put32(out, static_cast<uint32_t>(body.size()));
    Put32(out, library);
    return out + body;
}

struct Face {
    uint16_t a, b, c;
    uint8_t material = 0, light = 0;
};

struct Group {
    uint16_t first, last;
};

struct ColSpec {
    float box[6] = {-1, -2, -3, 4, 5, 6};
    int spheres = 0;
    int boxes = 0;
    int vertices = 0;
    std::vector<Face> faces;
    std::vector<Group> groups;
    uint32_t flags = 2;          // 2 = has volumes, 8 = has face groups
    int padAfterVertices = 0;    // the converter that made the map pads to four bytes
    bool shadow = false;         // append a shadow mesh the client is expected to drop
};

// A complete COL3 file. Offsets inside it count from byte 4, as the format has them.
inline std::string BuildCol3(const ColSpec& spec) {
    std::string data;   // everything behind the 0x20 + 0x58 byte head
    const size_t dataAt = 0x78;
    const auto offsetHere = [&] { return static_cast<uint32_t>(dataAt + data.size() - 4); };

    uint32_t offSpheres = 0, offBoxes = 0, offVertices = 0, offFaces = 0, offShadowVertices = 0, offShadowFaces = 0;
    if (spec.spheres > 0) {
        offSpheres = offsetHere();
        for (int i = 0; i < spec.spheres; ++i) {
            PutF(data, 1.0f * static_cast<float>(i));
            PutF(data, 2.0f);
            PutF(data, 3.0f);
            PutF(data, 0.5f);
            data += std::string("\x04\x00\x00\x00", 4);
        }
    }
    if (spec.boxes > 0) {
        offBoxes = offsetHere();
        for (int i = 0; i < spec.boxes; ++i) {
            for (int k = 0; k < 6; ++k) PutF(data, static_cast<float>(k - 3 + i));
            data += std::string("\x09\x00\x00\x00", 4);
        }
    }
    if (spec.vertices > 0) {
        offVertices = offsetHere();
        for (int i = 0; i < spec.vertices; ++i) {
            Put16(data, static_cast<uint32_t>(i * 7));
            Put16(data, static_cast<uint32_t>(i * 3));
            Put16(data, static_cast<uint32_t>(i));
        }
        data.append(static_cast<size_t>(spec.padAfterVertices), '\0');
    }
    if (!spec.groups.empty()) {
        for (const Group& g : spec.groups) {
            for (int k = 0; k < 6; ++k) PutF(data, static_cast<float>(k));
            Put16(data, g.first);
            Put16(data, g.last);
        }
        Put32(data, static_cast<uint32_t>(spec.groups.size()));
    }
    if (!spec.faces.empty()) {
        offFaces = offsetHere();
        for (const Face& f : spec.faces) {
            Put16(data, f.a);
            Put16(data, f.b);
            Put16(data, f.c);
            data.push_back(static_cast<char>(f.material));
            data.push_back(static_cast<char>(f.light));
        }
    }
    uint32_t shadowFaces = 0;
    if (spec.shadow) {
        offShadowVertices = offsetHere();
        for (int i = 0; i < 3; ++i) {
            Put16(data, 1);
            Put16(data, 2);
            Put16(data, 3);
        }
        offShadowFaces = offsetHere();
        Put16(data, 0);
        Put16(data, 1);
        Put16(data, 2);
        Put16(data, 0);
        shadowFaces = 1;
    }

    std::string info;
    for (int k = 0; k < 6; ++k) PutF(info, spec.box[k]);
    for (int k = 0; k < 3; ++k) PutF(info, (spec.box[k] + spec.box[k + 3]) * 0.5f);
    PutF(info, 10.0f);
    Put16(info, static_cast<uint32_t>(spec.spheres));
    Put16(info, static_cast<uint32_t>(spec.boxes));
    Put16(info, static_cast<uint32_t>(spec.faces.size()));
    info.push_back('\0');   // lines
    info.push_back('\0');
    Put32(info, spec.flags | (spec.shadow ? 16u : 0u));
    Put32(info, offSpheres);
    Put32(info, offBoxes);
    Put32(info, 0);            // lines
    Put32(info, offVertices);
    Put32(info, offFaces);
    Put32(info, 0);            // planes
    Put32(info, shadowFaces);
    Put32(info, offShadowVertices);
    Put32(info, offShadowFaces);

    std::string file = "COL3";
    Put32(file, static_cast<uint32_t>(0x18 + info.size() + data.size()));
    std::string name = "testmodel";
    name.resize(22, '\0');
    file += name;
    Put16(file, 0);
    return file + info + data;
}

// A model file the way the map's are laid out: clump { struct, frame list, geometry list, atomic, extension }
// with SA-MP's collision in the extension of the clump.
inline std::string BuildDff(const std::string& colFile, bool uvDictionaryFirst = false, bool otherPluginFirst = false,
                            uint32_t collisionChunk = 0x253F2FF) {
    std::string clump;
    std::string st;
    Put32(st, 1);
    Put32(st, 0);
    Put32(st, 0);
    clump += Chunk(0x01, st);
    clump += Chunk(0x0E, Chunk(0x01, std::string(60, '\0')) + Chunk(0x03, ""));
    clump += Chunk(0x1A, Chunk(0x01, std::string(4, '\0')) + Chunk(0x0F, Chunk(0x01, std::string(40, '\x11'))));
    clump += Chunk(0x14, Chunk(0x01, std::string(16, '\0')) + Chunk(0x03, Chunk(0x1F, std::string(8, '\0'))));
    std::string extension;
    if (otherPluginFirst) extension += Chunk(0x253F2F8, std::string(20, '\x22'));
    if (!colFile.empty()) extension += Chunk(collisionChunk, colFile);
    clump += Chunk(0x03, extension);
    std::string file;
    if (uvDictionaryFirst) file += Chunk(0x2B, std::string(64, '\x33'));
    return file + Chunk(0x10, clump);
}

struct TexSpec {
    std::string name;
    uint32_t side = 4;   // pixels; the texture takes side * side * 4 bytes
};

// A PC texture dictionary (Direct3D 9, uncompressed 32 bit, one level).
inline std::string BuildTxdSized(const std::vector<TexSpec>& textures) {
    std::string head;
    Put16(head, static_cast<uint32_t>(textures.size()));
    Put16(head, 2);
    std::string body = Chunk(0x01, head);
    for (const TexSpec& texture : textures) {
        std::string st;
        Put32(st, 9);        // platform: Direct3D 9
        Put32(st, 0x1102);   // linear, wrap, wrap
        std::string name = texture.name, mask;
        name.resize(32, '\0');
        mask.resize(32, '\0');
        st += name + mask;
        Put32(st, 0x0500);   // raster format 8888
        Put32(st, 21);       // D3DFMT_A8R8G8B8
        Put16(st, texture.side);
        Put16(st, texture.side);
        st.push_back(32);
        st.push_back(1);
        st.push_back(4);
        st.push_back(0);
        const uint32_t bytes = texture.side * texture.side * 4;
        Put32(st, bytes);
        for (uint32_t i = 0; i < bytes; ++i) st.push_back(static_cast<char>(i * 7 + texture.name.size()));
        body += Chunk(0x15, Chunk(0x01, st) + Chunk(0x03, ""));
    }
    body += Chunk(0x03, "");
    return Chunk(0x16, body);
}

// The same with every texture four pixels wide.
inline std::string BuildTxd(const std::vector<std::string>& names) {
    std::vector<TexSpec> textures;
    for (const std::string& name : names) textures.push_back({name, 4});
    return BuildTxdSized(textures);
}

}  // namespace vctest
