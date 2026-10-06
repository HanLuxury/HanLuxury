#include "ShaderUniforms.h"
#include "GraphicsLog.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx {
namespace {

constexpr char kTag[] = "Uniform";

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool SameName(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

std::vector<std::string> SplitWords(const std::string& line) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        size_t b = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > b) words.emplace_back(line, b, i - b);
    }
    return words;
}

bool ParseNumber(const std::string& text, float& out) {
    const std::string s = Lower(text);
    if (s == "true") { out = 1.0f; return true; }
    if (s == "false") { out = 0.0f; return true; }
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string FormatValue(UniformType type, float v) {
    char buf[48];
    switch (type) {
        case UniformType::Bool: return v > 0.5f ? "true" : "false";
        case UniformType::Int: std::snprintf(buf, sizeof(buf), "%ld", std::lround(v)); return buf;
        default: std::snprintf(buf, sizeof(buf), "%g", v); return buf;
    }
}

} // namespace

ShaderUniforms& ShaderUniforms::Get() {
    static ShaderUniforms instance;
    return instance;
}

const char* ShaderUniforms::TypeName(UniformType type) {
    switch (type) {
        case UniformType::Bool: return "bool";
        case UniformType::Int: return "int";
        default: return "float";
    }
}

bool ShaderUniforms::ParseType(const std::string& text, UniformType& out) {
    const std::string t = Lower(text);
    if (t == "bool") { out = UniformType::Bool; return true; }
    if (t == "int") { out = UniformType::Int; return true; }
    if (t == "float") { out = UniformType::Float; return true; }
    return false;
}

float ShaderUniforms::Sanitize(const UniformEntry& e, float value) {
    if (!std::isfinite(value)) value = e.def;
    switch (e.type) {
        case UniformType::Bool: return value > 0.5f ? 1.0f : 0.0f;
        case UniformType::Int: value = std::round(value); break;
        default: break;
    }
    if (e.max > e.min) value = std::clamp(value, e.min, e.max);
    return value;
}

UniformEntry* ShaderUniforms::FindLocked(const std::string& cls, const std::string& name) {
    for (UniformEntry& e : m_entries)
        if (SameName(e.cls, cls) && SameName(e.name, name)) return &e;
    return nullptr;
}

const UniformEntry* ShaderUniforms::FindLocked(const std::string& cls, const std::string& name) const {
    for (const UniformEntry& e : m_entries)
        if (SameName(e.cls, cls) && SameName(e.name, name)) return &e;
    return nullptr;
}

bool ShaderUniforms::Load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        text.append(chunk, n);
        if (text.size() > 128 * 1024) break;
    }
    std::fclose(f);
    std::string error;
    const bool ok = LoadFromString(text, &error);
    if (!error.empty()) GFX_LOGW(kTag, "%s: %s", path.c_str(), error.c_str());
    GFX_LOGI(kTag, "loaded %s (%zu values)", path.c_str(), Count());
    return ok;
}

bool ShaderUniforms::LoadFromString(const std::string& text, std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string cls;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const size_t comment = line.find("//");
        if (comment != std::string::npos) line.erase(comment);
        std::vector<std::string> w = SplitWords(line);
        if (w.empty() || w[0][0] == '#' || w[0][0] == ';') continue;
        if (Lower(w[0]) == "endl") break;
        if (Lower(w[0]) == "class") {
            // "class Name:" or "class Name :"
            std::string name = w.size() > 1 ? w[1] : std::string();
            if (!name.empty() && name.back() == ':') name.pop_back();
            if (name.empty()) {
                if (error) *error += "line " + std::to_string(lineNo) + ": class without a name; ";
                continue;
            }
            cls = name;
            continue;
        }
        UniformType type;
        if (!ParseType(w[0], type) || w.size() < 3 || cls.empty()) {
            if (error) *error += "line " + std::to_string(lineNo) + ": expected 'type name value [min max step]'; ";
            continue;
        }
        float value = 0.0f;
        if (!ParseNumber(w[2], value)) {
            if (error) *error += "line " + std::to_string(lineNo) + ": bad value '" + w[2] + "'; ";
            continue;
        }
        UniformEntry* e = FindLocked(cls, w[1]);
        if (!e) {
            UniformEntry fresh;
            fresh.cls = cls;
            fresh.name = w[1];
            fresh.type = type;
            fresh.def = value;
            if (type == UniformType::Bool) { fresh.min = 0.0f; fresh.max = 1.0f; fresh.step = 1.0f; }
            m_entries.push_back(fresh);
            e = &m_entries.back();
        }
        e->type = type;
        float mn, mx, st;
        if (w.size() >= 6 && ParseNumber(w[3], mn) && ParseNumber(w[4], mx) && ParseNumber(w[5], st) && mx > mn) {
            e->min = mn;
            e->max = mx;
            e->step = st > 0.0f ? st : (type == UniformType::Float ? 0.01f : 1.0f);
            e->rangeFromFile = true;
        } else if (type == UniformType::Bool) {
            e->min = 0.0f; e->max = 1.0f; e->step = 1.0f;
        }
        e->value = Sanitize(*e, value);
    }
    return true;
}

std::string ShaderUniforms::ToIniString() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string out =
        "// =====================================================================\n"
        "//  EAGLE Graphics - shaderUniform.ini\n"
        "//  Nilai shader yang bisa diubah LANGSUNG (tab \"Grafis\" di pengaturan,\n"
        "//  tanpa restart). Dipakai shader lewat deklarasi @ di glShader/*.shader:\n"
        "//    @float Shadow_Strength < class = \"Shadow\", name = \"Strength\", default = 1.0; >\n"
        "//  Format per baris:  tipe  nama  nilai  min  max  step\n"
        "//  tipe: bool | int | float.  'endl' = akhir file.\n"
        "//  Ditulis ulang oleh engine saat tombol simpan ditekan.\n"
        "// =====================================================================\n";
    std::vector<std::string> classes;
    for (const UniformEntry& e : m_entries) {
        bool seen = false;
        for (const std::string& c : classes) seen = seen || SameName(c, e.cls);
        if (!seen) classes.push_back(e.cls);
    }
    char buf[256];
    for (const std::string& c : classes) {
        out += "\nclass\t" + c + ":\n";
        for (const UniformEntry& e : m_entries) {
            if (!SameName(e.cls, c)) continue;
            if (e.type == UniformType::Bool) {
                std::snprintf(buf, sizeof(buf), "\tbool\t%s\t%s\n", e.name.c_str(), e.value > 0.5f ? "true" : "false");
            } else {
                std::snprintf(buf, sizeof(buf), "\t%s\t%s\t%s\t%s\t%s\t%s\n", TypeName(e.type), e.name.c_str(),
                              FormatValue(e.type, e.value).c_str(), FormatValue(UniformType::Float, e.min).c_str(),
                              FormatValue(UniformType::Float, e.max).c_str(), FormatValue(UniformType::Float, e.step).c_str());
            }
            out += buf;
        }
    }
    out += "\nendl\n";
    return out;
}

bool ShaderUniforms::Save(const std::string& path) const {
    const std::string text = ToIniString();
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        GFX_LOGE(kTag, "cannot write %s", tmp.c_str());
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        GFX_LOGE(kTag, "saving %s failed", path.c_str());
        return false;
    }
    GFX_LOGI(kTag, "saved %s", path.c_str());
    return true;
}

int ShaderUniforms::Declare(const std::string& cls, const std::string& name, UniformType type, float def, float min,
                            float max, float step, bool hasRange) {
    std::lock_guard<std::mutex> lock(m_mutex);
    UniformEntry* e = FindLocked(cls, name);
    if (!e) {
        UniformEntry fresh;
        fresh.cls = cls;
        fresh.name = name;
        fresh.type = type;
        fresh.value = def;
        if (type == UniformType::Bool) { fresh.min = 0.0f; fresh.max = 1.0f; fresh.step = 1.0f; }
        else if (!hasRange) { fresh.min = std::min(0.0f, def); fresh.max = std::max(1.0f, def * 2.0f); fresh.step = type == UniformType::Int ? 1.0f : 0.01f; }
        m_entries.push_back(fresh);
        e = &m_entries.back();
    } else if (e->type != type) {
        GFX_LOGW(kTag, "%s.%s: shader declares %s, shaderUniform.ini has %s (shader wins)", cls.c_str(), name.c_str(),
                 TypeName(type), TypeName(e->type));
        e->type = type;
    }
    e->def = def;
    // shaderUniform.ini keeps its own range if it has one; otherwise take the shader's.
    if (hasRange && max > min && type != UniformType::Bool && !e->rangeFromFile) {
        e->min = min;
        e->max = max;
        e->step = step > 0.0f ? step : e->step;
    }
    e->value = Sanitize(*e, e->value);
    if (e->slot < 0) {
        if (m_nextSlot >= kSlotCount) {
            GFX_LOGW(kTag, "%s.%s: all %d SG_User slots are used, the shader keeps the default", cls.c_str(),
                     name.c_str(), kSlotCount);
            return -1;
        }
        e->slot = m_nextSlot++;
    }
    return e->slot;
}

bool ShaderUniforms::Set(const std::string& cls, const std::string& name, float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    UniformEntry* e = FindLocked(cls, name);
    if (!e) return false;
    e->value = Sanitize(*e, value);
    return true;
}

bool ShaderUniforms::Lookup(const std::string& cls, const std::string& name, UniformEntry& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const UniformEntry* e = FindLocked(cls, name);
    if (!e) return false;
    out = *e;
    return true;
}

void ShaderUniforms::ResetToDefaults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (UniformEntry& e : m_entries) e.value = Sanitize(e, e.def);
}

void ShaderUniforms::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_nextSlot = 0;
}

std::string ShaderUniforms::Serialize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string out;
    char buf[320];
    for (const UniformEntry& e : m_entries) {
        std::snprintf(buf, sizeof(buf), "%s\t%s\t%s\t%g\t%g\t%g\t%g\t%d\n", e.cls.c_str(), e.name.c_str(),
                      TypeName(e.type), e.value, e.min, e.max, e.step, e.slot >= 0 ? 1 : 0);
        out += buf;
    }
    return out;
}

void ShaderUniforms::Snapshot(float out[kSlotCount]) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fill(out, out + kSlotCount, 0.0f);
    for (const UniformEntry& e : m_entries)
        if (e.slot >= 0 && e.slot < kSlotCount) out[e.slot] = e.value;
}

size_t ShaderUniforms::Count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

int ShaderUniforms::UsedSlots() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nextSlot;
}

} // namespace gfx
