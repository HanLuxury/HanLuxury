#pragma once
// EAGLE graphics engine - shaderUniform.ini: shader values the player tunes live.
//
// File format (same idea as SA_DOX):
//     class   Shadow:
//         float   Strength    1.0     0   1.5   0.05      <- type name value min max step
//         bool    Enable      true
//     endl
// A shader file declares a parameter with
//     @float Shadow_Strength < class = "Shadow", name = "Strength", default = 1.0; min = 0.0; max = 1.5; step = 0.05; >
// Each declared parameter gets one float slot of "uniform vec4 SG_User[8]",
// uploaded together with the receiver uniforms every frame: changing a value
// never needs a shader rebuild. Slots are never reassigned during a session,
// because GTA shaders that are already compiled keep their slot indices.
//
// Thread safety: every method locks; Snapshot() is what the GL thread reads.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gfx {

enum class UniformType { Bool, Int, Float };

struct UniformEntry {
    std::string cls;
    std::string name;
    UniformType type = UniformType::Float;
    float value = 0.0f;
    float def = 0.0f;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.01f;
    int slot = -1;         // index into SG_User (x,y,z,w of vec4 0..7), -1 = not used by any shader
    bool rangeFromFile = false; // min/max/step written in shaderUniform.ini (wins over the shader's)
};

class ShaderUniforms {
public:
    static constexpr int kVec4Count = 8;
    static constexpr int kSlotCount = kVec4Count * 4;

    static ShaderUniforms& Get();

    // Reads shaderUniform.ini. Values of known entries are updated, new ones added;
    // slots stay as they are. Returns false if the file is missing.
    bool Load(const std::string& path);
    bool LoadFromString(const std::string& text, std::string* error = nullptr);
    // Writes every entry back (SDX layout, grouped by class).
    bool Save(const std::string& path) const;
    std::string ToIniString() const;

    // Called by the shader preprocessor for every @parameter. A value already read
    // from shaderUniform.ini wins over 'def'. Returns the slot, -1 when all are used.
    int Declare(const std::string& cls, const std::string& name, UniformType type, float def, float min, float max,
                float step, bool hasRange);

    bool Set(const std::string& cls, const std::string& name, float value);
    bool Lookup(const std::string& cls, const std::string& name, UniformEntry& out) const;
    void ResetToDefaults();
    void Clear(); // tests

    // JNI: one entry per line: class \t name \t type \t value \t min \t max \t step \t used
    std::string Serialize() const;
    void Snapshot(float out[kSlotCount]) const;
    size_t Count() const;
    int UsedSlots() const;

    static const char* TypeName(UniformType type);
    static bool ParseType(const std::string& text, UniformType& out);

private:
    UniformEntry* FindLocked(const std::string& cls, const std::string& name);
    const UniformEntry* FindLocked(const std::string& cls, const std::string& name) const;
    static float Sanitize(const UniformEntry& e, float value);

    mutable std::mutex m_mutex;
    std::vector<UniformEntry> m_entries;
    int m_nextSlot = 0;
};

} // namespace gfx
