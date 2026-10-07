#pragma once
// EAGLE graphics engine - loading/compiling the engine's own GLSL programs.
//
// Sources live in /storage/emulated/0/TESTLIT/graphics/glShader/ (SA_DOX-style
// .shader files, see GlShader.h). A file that is missing there uses the copy
// built into the library (EmbeddedGlShader.h). Compile/link errors are written
// to logOutput.log and the program is reported as 0: the effect that needs it
// is disabled, the game continues.
//
// GL functions (CompileShader/CreateProgram/GetProgram) must be called on the
// GL thread. ReadFile/LoadShaderFile are plain file IO and thread-safe once the
// embedded copies are registered (EarlyInit).

#include <GLES3/gl3.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx {

struct AttribBinding {
    GLuint index;
    const char* name;
};

class ShaderManager {
public:
    static ShaderManager& Get();

    void SetBaseDir(const std::string& dir);
    void RegisterEmbedded(const std::string& name, const char* source);

    // Raw file <baseDir>/<path>, else the embedded copy. 'fromFile' tells which one was used.
    bool ReadFile(const std::string& path, std::string& out, bool* fromFile = nullptr, bool embeddedOnly = false) const;
    // "Debug/ShadowMap.shader:vert" -> that stage with #include expanded (GlShader).
    // A name without ":stage" is read raw.
    bool LoadShaderFile(const std::string& name, std::string& out, bool* fromFile = nullptr) const;

    GLuint CompileShader(GLenum type, const std::string& source, const std::string& debugName);
    GLuint CreateProgram(GLuint vs, GLuint fs, const std::string& debugName,
                         const AttribBinding* bindings, int bindingCount);

    // Cached program built from two shader files. Returns 0 on failure; a failed
    // program is not retried until ReloadAll().
    GLuint GetProgram(const std::string& key, const std::string& vsFile, const std::string& fsFile,
                      const AttribBinding* bindings, int bindingCount);

    // Development hot reload: deletes cached programs (GL thread) so the next
    // GetProgram() call re-reads the files. Never called per frame by the engine.
    void ReloadAll();
    // Context loss: forget handles without deleting them (they died with the context).
    void ForgetAll();

    unsigned Generation() const { return m_generation; }

private:
    struct Entry {
        GLuint program = 0;
        bool failed = false;
    };
    std::string m_baseDir;
    std::unordered_map<std::string, std::string> m_embedded;
    std::unordered_map<std::string, Entry> m_programs;
    unsigned m_generation = 1;
};

// Reads a whole text file (max 1 MiB). Returns false if missing/unreadable.
bool ReadTextFile(const std::string& path, std::string& out);

} // namespace gfx
