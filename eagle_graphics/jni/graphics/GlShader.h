#pragma once
// EAGLE graphics engine - glShader/*.shader files (SA_DOX-style layout).
//
//   <vert> ... </vert>     vertex stage   (a file without tags is one stage)
//   <frag> ... </frag>     fragment stage
//   #include "x.shader";   same stage of another file: first next to the
//                          including file, then from glShader/. Each file is
//                          included once per build (no include guards needed).
//   @float Name < class = "C", name = "N", default = 1.0; min = 0; max = 2; step = 0.1; >
//   @int / @bool           live parameter from shaderUniform.ini (receivers only)
//
// Files are read from /storage/emulated/0/TESTLIT/graphics/glShader/; a missing
// file uses the copy built into libmultiplayer.so (EmbeddedGlShader.h).
//
// Receiver snippets (Entity/*.shader) are injected into GTA's own GLSL ES 1.00
// world shaders. Their @parameters become plain globals filled by the generated
// SG_InitUser() from "uniform mediump vec4 SG_User[8]" (see ShaderUniforms).

#include "ShaderUniforms.h"

#include <string>
#include <vector>

namespace gfx::GlShader {

struct ParamDecl {
    UniformType type = UniformType::Float;
    std::string glslName;
    std::string cls;
    std::string name;
    float def = 0.0f, min = 0.0f, max = 1.0f, step = 0.01f;
    bool hasRange = false;
};

struct BuildResult {
    std::string code;
    std::vector<std::string> files; // "path (file)" / "path (embedded)" in include order
    int params = 0;
    bool fromFile = false;          // the top-level file came from TESTLIT
    std::string error;
};

// Text between <stage> and </stage>; the whole text when the file has no stage tags.
// Returns false if the file has tags but not this stage.
bool ExtractSection(const std::string& text, const char* stage, std::string& out);

// Parses one "@type name < ... >" line.
bool ParseParam(const std::string& line, ParamDecl& out, std::string& error);

// Stage of a file with every #include expanded. embeddedOnly ignores TESTLIT files.
bool LoadStage(const std::string& path, const char* stage, BuildResult& out, bool embeddedOnly = false);

// Receiver snippet: LoadStage(path, "frag") + @parameters declared in 'table'.
bool BuildReceiver(const std::string& path, ShaderUniforms& table, BuildResult& out, bool embeddedOnly = false);

} // namespace gfx::GlShader
