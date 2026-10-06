#include "GlShader.h"
#include "GraphicsLog.h"
#include "ShaderManager.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx::GlShader {
namespace {

constexpr char kTag[] = "GlShader";
constexpr int kMaxIncludeDepth = 8;
constexpr size_t kMaxSourceSize = 256 * 1024;

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return s.compare(0, 3, "gl_") != 0;
}

bool ParseFloat(const std::string& text, float& out) {
    const std::string s = Lower(Trim(text));
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

std::string DirOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// "a/./b/../c" -> "a/c"; rejects paths that leave glShader/.
bool NormalizePath(const std::string& path, std::string& out) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(pos, end - pos);
        if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        pos = end + 1;
    }
    out.clear();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return !out.empty();
}

bool ReadShaderFile(const std::string& path, std::string& out, bool& fromFile, bool embeddedOnly) {
    return ShaderManager::Get().ReadFile(path, out, &fromFile, embeddedOnly);
}

// '#include "x";' -> x
bool ParseInclude(const std::string& line, std::string& target) {
    std::string t = Trim(line);
    if (t.compare(0, 8, "#include") != 0) return false;
    const size_t q1 = t.find('"');
    const size_t q2 = q1 == std::string::npos ? std::string::npos : t.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    target = t.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

struct Expander {
    const char* stage;
    bool embeddedOnly;
    std::vector<std::string> done;
    BuildResult* result;

    bool Seen(const std::string& path) const {
        for (const std::string& d : done)
            if (d == path) return true;
        return false;
    }

    bool Expand(const std::string& path, std::string& out, int depth) {
        if (depth > kMaxIncludeDepth) {
            result->error = "include depth > " + std::to_string(kMaxIncludeDepth) + " at " + path;
            return false;
        }
        if (Seen(path)) return true; // included once
        done.push_back(path);

        std::string text;
        bool fromFile = false;
        if (!ReadShaderFile(path, text, fromFile, embeddedOnly)) {
            result->error = "missing " + path;
            return false;
        }
        if (depth == 0) result->fromFile = fromFile;
        result->files.push_back(path + (fromFile ? " (file)" : " (embedded)"));

        std::string section;
        if (!ExtractSection(text, stage, section)) {
            result->error = path + " has no <" + std::string(stage) + "> section";
            return false;
        }
        size_t pos = 0;
        while (pos < section.size()) {
            size_t end = section.find('\n', pos);
            if (end == std::string::npos) end = section.size();
            const std::string line = section.substr(pos, end - pos);
            pos = end + 1;
            std::string target;
            if (ParseInclude(line, target)) {
                std::string resolved, candidate;
                bool found = false;
                // Next to the including file first, then from the glShader/ root.
                for (const std::string& base : {DirOf(path), std::string()}) {
                    if (!NormalizePath(base + target, candidate)) continue;
                    std::string probe;
                    bool probeFromFile = false;
                    if (Seen(candidate) || ReadShaderFile(candidate, probe, probeFromFile, embeddedOnly)) {
                        resolved = candidate;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    result->error = path + ": #include \"" + target + "\" not found";
                    return false;
                }
                if (!Expand(resolved, out, depth + 1)) return false;
                continue;
            }
            out += line;
            out += '\n';
            if (out.size() > kMaxSourceSize) {
                result->error = "source larger than 256 KiB";
                return false;
            }
        }
        return true;
    }
};

std::string FloatLiteral(float v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
    return buf;
}

} // namespace

bool ExtractSection(const std::string& text, const char* stage, std::string& out) {
    const std::string open = std::string("<") + stage + ">";
    const std::string close = std::string("</") + stage + ">";
    const size_t b = text.find(open);
    if (b == std::string::npos) {
        // No tags at all: the file is a single stage.
        if (text.find("<vert>") != std::string::npos || text.find("<frag>") != std::string::npos) return false;
        out = text;
        return true;
    }
    size_t start = b + open.size();
    const size_t e = text.find(close, start);
    out = text.substr(start, e == std::string::npos ? std::string::npos : e - start);
    // "#version" must be the first line of a GLSL ES 3.00 source.
    size_t first = 0;
    while (first < out.size() && std::isspace(static_cast<unsigned char>(out[first]))) ++first;
    out.erase(0, first);
    return true;
}

bool ParseParam(const std::string& line, ParamDecl& out, std::string& error) {
    std::string t = Trim(line);
    if (t.empty() || t[0] != '@') {
        error = "not a parameter";
        return false;
    }
    const size_t lt = t.find('<');
    const size_t gt = t.rfind('>');
    if (lt == std::string::npos || gt == std::string::npos || gt < lt) {
        error = "expected '@type name < ... >'";
        return false;
    }
    // "@float Name"
    std::string head = Trim(t.substr(1, lt - 1));
    const size_t space = head.find_first_of(" \t");
    if (space == std::string::npos) {
        error = "expected '@type name'";
        return false;
    }
    if (!ShaderUniforms::ParseType(Trim(head.substr(0, space)), out.type)) {
        error = "type must be bool, int or float";
        return false;
    }
    out.glslName = Trim(head.substr(space + 1));
    if (!IsIdentifier(out.glslName) || out.glslName.compare(0, 3, "SG_") == 0) {
        error = "bad name '" + out.glslName + "' (letters/digits/_ , not SG_)";
        return false;
    }
    // key = value pairs separated by ',' or ';'
    const std::string body = t.substr(lt + 1, gt - lt - 1);
    bool hasDefault = false, hasMin = false, hasMax = false, hasStep = false;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find_first_of(",;", pos);
        if (end == std::string::npos) end = body.size();
        const std::string pair = Trim(body.substr(pos, end - pos));
        pos = end + 1;
        if (pair.empty()) continue;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            error = "expected key = value in '" + pair + "'";
            return false;
        }
        const std::string key = Lower(Trim(pair.substr(0, eq)));
        std::string value = Trim(pair.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        float f = 0.0f;
        if (key == "class") out.cls = value;
        else if (key == "name") out.name = value;
        else if (key == "default" && ParseFloat(value, f)) { out.def = f; hasDefault = true; }
        else if (key == "min" && ParseFloat(value, f)) { out.min = f; hasMin = true; }
        else if (key == "max" && ParseFloat(value, f)) { out.max = f; hasMax = true; }
        else if (key == "step" && ParseFloat(value, f)) { out.step = f; hasStep = true; }
        else {
            error = "unknown or bad '" + pair + "'";
            return false;
        }
    }
    if (out.cls.empty() || out.name.empty() || !hasDefault) {
        error = "class, name and default are required";
        return false;
    }
    out.hasRange = hasMin && hasMax && out.max > out.min;
    if (!hasStep) out.step = out.type == UniformType::Float ? 0.01f : 1.0f;
    return true;
}

bool LoadStage(const std::string& path, const char* stage, BuildResult& out, bool embeddedOnly) {
    out = BuildResult{};
    std::string normalized;
    if (!NormalizePath(path, normalized)) {
        out.error = "bad path " + path;
        return false;
    }
    Expander ex{stage, embeddedOnly, {}, &out};
    if (!ex.Expand(normalized, out.code, 0)) return false;
    // Engine programs have no SG_User: parameters are not supported there yet.
    std::string code;
    size_t pos = 0;
    while (pos < out.code.size()) {
        size_t end = out.code.find('\n', pos);
        if (end == std::string::npos) end = out.code.size();
        const std::string line = out.code.substr(pos, end - pos);
        pos = end + 1;
        if (!Trim(line).empty() && Trim(line)[0] == '@') {
            GFX_LOGW(kTag, "%s: @parameter ignored outside receiver shaders", path.c_str());
            continue;
        }
        code += line;
        code += '\n';
    }
    out.code.swap(code);
    return true;
}

bool BuildReceiver(const std::string& path, ShaderUniforms& table, BuildResult& out, bool embeddedOnly) {
    out = BuildResult{};
    std::string normalized;
    if (!NormalizePath(path, normalized)) {
        out.error = "bad path " + path;
        return false;
    }
    Expander ex{"frag", embeddedOnly, {}, &out};
    std::string expanded;
    if (!ex.Expand(normalized, expanded, 0)) return false;

    std::string code = "uniform mediump vec4 SG_User[" + std::to_string(ShaderUniforms::kVec4Count) + "];\n";
    std::string init;
    std::vector<std::string> declared;
    static const char kComp[] = "xyzw";
    size_t pos = 0;
    while (pos < expanded.size()) {
        size_t end = expanded.find('\n', pos);
        if (end == std::string::npos) end = expanded.size();
        const std::string line = expanded.substr(pos, end - pos);
        pos = end + 1;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] != '@') {
            code += line;
            code += '\n';
            continue;
        }
        ParamDecl p;
        std::string error;
        if (!ParseParam(trimmed, p, error)) {
            out.error = path + ": " + error + " in '" + trimmed + "'";
            return false;
        }
        bool dup = false;
        for (const std::string& d : declared) dup = dup || d == p.glslName;
        if (dup) continue; // same parameter from two included files
        declared.push_back(p.glslName);

        const int slot = table.Declare(p.cls, p.name, p.type, p.def, p.min, p.max, p.step, p.hasRange);
        std::string src;
        if (slot >= 0) {
            src = "SG_User[" + std::to_string(slot / 4) + "]." + kComp[slot % 4];
        }
        switch (p.type) {
            case UniformType::Bool:
                code += "bool " + p.glslName + ";\n";
                init += p.glslName + " = " + (slot >= 0 ? src + " > 0.5" : std::string(p.def > 0.5f ? "true" : "false")) + ";\n";
                break;
            case UniformType::Int:
                code += "int " + p.glslName + ";\n";
                init += p.glslName + " = " + (slot >= 0 ? "int(floor(" + src + " + 0.5))" : std::to_string(static_cast<int>(std::lround(p.def)))) + ";\n";
                break;
            default:
                code += "mediump float " + p.glslName + ";\n";
                init += p.glslName + " = " + (slot >= 0 ? src : FloatLiteral(p.def)) + ";\n";
                break;
        }
        ++out.params;
    }
    code += "void SG_InitUser() {\n" + init + "}\n";
    out.code = code;
    return true;
}

} // namespace gfx::GlShader
