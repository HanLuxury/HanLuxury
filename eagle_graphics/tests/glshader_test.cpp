// Host test of the TESTLIT/graphics files (no device, no NDK needed):
//   - glShader/Entity/*.shader build (files == built-in copies), snippet contract
//   - GTA world shaders patched for every option combination -> out/*.vert|frag
//     (validated afterwards with glslangValidator by run_host_tests.sh)
//   - shaderUniform.ini parse/save, @parameter slots
//   - Config.ini + Advanced.ini (SDX syntax), data/eagle_timecyc.dat
// Usage: glshader_test <eagle_graphics dir> <out dir>

#include "EmbeddedGlShader.h"
#include "GlShader.h"
#include "GraphicsConfig.h"
#include "ShaderManager.h"
#include "ShaderPatcher.h"
#include "ShaderUniforms.h"
#include "TimeCycleFX.h"
#include "gta_cases.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace gfx;

static int g_fails = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            ++g_fails;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <eagle_graphics dir> <out dir>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    const std::string out = argv[2];
    const std::string gfxDir = root + "/TESTLIT/graphics/";

    ShaderManager& sm = ShaderManager::Get();
    sm.SetBaseDir(gfxDir + "glShader/");
    RegisterEmbeddedShaders(sm);

    // ---- shaderUniform.ini first (as EarlyInit does), then the receivers
    ShaderUniforms& table = ShaderUniforms::Get();
    CHECK(table.Load(gfxDir + "shaderUniform.ini"), "shaderUniform.ini missing");
    const size_t iniEntries = table.Count();

    static const char* const kFiles[3] = {"Entity/Building.shader", "Entity/Vehicle.shader", "Entity/Character.shader"};
    ReceiverSnippets snippets;
    for (int i = 0; i < 3; ++i) {
        GlShader::BuildResult file, builtin;
        CHECK(GlShader::BuildReceiver(kFiles[i], table, file), "%s: %s", kFiles[i], file.error.c_str());
        CHECK(GlShader::BuildReceiver(kFiles[i], table, builtin, true), "%s builtin: %s", kFiles[i], builtin.error.c_str());
        CHECK(file.code == builtin.code, "%s differs from EmbeddedGlShader.h: run tools/embed_glshader.py", kFiles[i]);
        CHECK(file.fromFile, "%s not read from TESTLIT", kFiles[i]);
        const std::string clean = ShaderPatcher::SanitizeSnippet(file.code);
        std::string why;
        CHECK(ShaderPatcher::ValidateSnippet(clean, why), "%s: %s", kFiles[i], why.c_str());
        std::printf("%-26s params=%d maxStmt=%zu size=%zu files:", kFiles[i], file.params,
                    ShaderPatcher::MaxStatementLength(clean), clean.size());
        for (const std::string& f : file.files) std::printf(" %s", f.c_str());
        std::printf("\n");
        snippets.snippet[i] = clean;
    }
    std::printf("uniform slots used: %d, entries: %zu (ini had %zu)\n", table.UsedSlots(), table.Count(), iniEntries);
    CHECK(table.UsedSlots() == 10, "expected 10 @parameters, got %d", table.UsedSlots());
    CHECK(table.Count() == iniEntries, "shaderUniform.ini lacks entries the shaders declare (%zu vs %zu)", iniEntries,
          table.Count());

    // ---- patch every GTA case with every option
    const ReceiverEntity expected[] = {ReceiverEntity::Building, ReceiverEntity::Character, ReceiverEntity::Vehicle,
                                       ReceiverEntity::Building};
    int caseIndex = 0, written = 0;
    for (const GtaCase& c : GtaCases()) {
        for (int hw = 0; hw <= 1; ++hw)
            for (int taps : {1, 4, 9})
                for (int blend = 0; blend <= 1; ++blend) {
                    PatchOptions o;
                    o.hardwareCompare = hw;
                    o.taps = taps;
                    o.blend = blend;
                    std::string ops, ovs;
                    PatchInfo info;
                    const bool ok = ShaderPatcher::PatchSources(c.ps, c.vs, snippets, o, ops, ovs, info);
                    CHECK(ok == c.expectPatch, "%s: patched=%d expected=%d", c.name, ok, c.expectPatch);
                    if (!ok) continue;
                    if (caseIndex < 4)
                        CHECK(info.entity == expected[caseIndex], "%s classified as %s", c.name,
                              ShaderPatcher::EntityName(info.entity));
                    CHECK(ops.find("SG_InitUser();SG_Apply(fcolor);") != std::string::npos, "%s: call missing", c.name);
                    char base[256];
                    std::snprintf(base, sizeof(base), "%s/%s_hw%d_t%d_b%d", out.c_str(), c.name, hw, taps, blend);
                    std::ofstream(std::string(base) + ".vert") << ovs;
                    // glslang needs an explicit version (GTA relies on the ES default of 100)
                    std::ofstream(std::string(base) + ".frag") << "#version 100\n" << ops;
                    ++written;
                }
        std::ofstream(out + "/native_" + c.name + ".vert") << c.vs;
        std::ofstream(out + "/native_" + c.name + ".frag") << "#version 100\n" << c.ps;
        ++caseIndex;
    }
    std::printf("patched variants written: %d\n", written);

    // ---- engine program: Debug/ShadowMap.shader
    for (const char* stage : {"vert", "frag"}) {
        GlShader::BuildResult r;
        CHECK(GlShader::LoadStage("Debug/ShadowMap.shader", stage, r), "ShadowMap %s: %s", stage, r.error.c_str());
        CHECK(r.code.compare(0, 15, "#version 300 es") == 0, "ShadowMap %s: #version not first", stage);
        std::ofstream(out + "/debug_shadowmap." + stage) << r.code;
    }

    // ---- @parameter parser
    {
        GlShader::ParamDecl p;
        std::string err;
        CHECK(GlShader::ParseParam("@float Gx < class = \"GodRays\", name = \"Exposure\", default = 0.24; >", p, err) &&
                  p.cls == "GodRays" && p.name == "Exposure" && std::fabs(p.def - 0.24f) < 1e-6f && !p.hasRange,
              "SA_DOX-style @param: %s", err.c_str());
        CHECK(!GlShader::ParseParam("@float SG_X < class = \"a\", name = \"b\", default = 1; >", p, err), "SG_ name accepted");
        CHECK(!GlShader::ParseParam("@vec3 X < class = \"a\", name = \"b\", default = 1; >", p, err), "vec3 accepted");
    }

    // ---- shaderUniform.ini round trip + live set
    {
        CHECK(table.Set("shadow", "strength", 9.0f), "Set failed");
        UniformEntry e;
        CHECK(table.Lookup("Shadow", "Strength", e) && std::fabs(e.value - 1.5f) < 1e-6f, "clamp to max failed (%g)", e.value);
        float snap[ShaderUniforms::kSlotCount];
        table.Snapshot(snap);
        CHECK(e.slot >= 0 && std::fabs(snap[e.slot] - 1.5f) < 1e-6f, "snapshot slot");
        const std::string saved = table.ToIniString();
        ShaderUniforms copy;
        CHECK(copy.LoadFromString(saved), "reload of saved text");
        CHECK(copy.Count() == table.Count(), "round trip count %zu vs %zu", copy.Count(), table.Count());
        UniformEntry back;
        CHECK(copy.Lookup("Shadow", "Strength", back) && std::fabs(back.value - 1.5f) < 1e-6f, "round trip value");
        table.ResetToDefaults();
        CHECK(table.Lookup("Shadow", "Strength", e) && std::fabs(e.value - 1.0f) < 1e-6f, "reset to default");
        std::ofstream(out + "/shaderUniform.saved.ini") << saved;
    }

    // ---- Config.ini + Advanced.ini
    {
        GraphicsConfig cfg;
        CHECK(cfg.Load((gfxDir + "Config.ini").c_str(), (gfxDir + "Advanced.ini").c_str(), nullptr), "Config load");
        CHECK(cfg.graphics.enabled && cfg.shadow.enabled, "enabled flags");
        CHECK(cfg.graphics.quality == GraphicsQuality::High, "quality %d", static_cast<int>(cfg.graphics.quality));
        CHECK(cfg.shadow.cascades == 3 && cfg.shadow.resolution == 2048, "cascades/resolution");
        CHECK(std::fabs(cfg.shadow.distance - 160.0f) < 1e-3f, "distance %g", cfg.shadow.distance);
        CHECK(std::fabs(cfg.shadow.depthBias - 0.06f) < 1e-4f, "Advanced.ini depthBias %g", cfg.shadow.depthBias);
        CHECK(cfg.shadow.weapons && !cfg.shadow.water && cfg.performance.adaptive, "bool keys");

        IniFile ini;
        ini.LoadFromString("[[Performance]\n[Shadow]\n  fDistance = 123.5 ; comment\n  iPcf = 1\n  cTint = 255,128,0\n"
                           "[Performance]]\n[shadow]\n bEnabled = 0\n// note\n");
        float d = 0.0f;
        int pcf = -1;
        bool en = true;
        float tint[3] = {};
        CHECK(ini.GetFloat("shadow", "distance", d) && std::fabs(d - 123.5f) < 1e-4f, "typed float key");
        CHECK(ini.GetInt("Shadow", "pcf", pcf) && pcf == 1, "typed int key");
        CHECK(ini.GetBool("SHADOW", "enabled", en) && !en, "typed bool key");
        CHECK(ini.GetColor("shadow", "tint", tint) && std::fabs(tint[1] - 128.0f / 255.0f) < 1e-4f, "colour key");
        CHECK(IniFile::NormalizeKey("cascades") == "cascades" && IniFile::NormalizeKey("ucAlpha") == "alpha" &&
                  IniFile::NormalizeKey("splitLambda") == "splitlambda",
              "NormalizeKey");
    }

    // ---- data/eagle_timecyc.dat
    {
        TimeCycleFX tc;
        std::string err;
        const int rows = tc.ParseProfiles(ReadAll(gfxDir + "data/eagle_timecyc.dat"), err);
        CHECK(rows == 5 && err.empty(), "eagle_timecyc.dat rows=%d err=%s", rows, err.c_str());
        TimeCycleFX builtin;
        bool same = true;
        for (int i = 0; i < 4; ++i) {
            const LookProfile& a = tc.Profile(i);
            const LookProfile& b = builtin.Profile(i);
            same = same && std::fabs(a.shadowStrength - b.shadowStrength) < 1e-4f && std::fabs(a.sunBoost - b.sunBoost) < 1e-4f &&
                   std::fabs(a.shadowTint.x - b.shadowTint.x) < 1e-4f && std::fabs(a.sunColor.y - b.sunColor.y) < 1e-4f &&
                   std::fabs(a.fogDensity - b.fogDensity) < 1e-4f;
        }
        CHECK(same, "eagle_timecyc.dat must equal the built-in looks");
        WeatherSnapshot w{};
        const TimeOfDayState s = tc.Evaluate(18.0f, w);
        CHECK(std::fabs(s.wSunrise + s.wDay + s.wSunset + s.wNight - 1.0f) < 1e-4f, "weights sum");
        std::string bad;
        TimeCycleFX t2;
        CHECK(t2.ParseProfiles("RAMPS 7 5 16 19\nNOON 1 2 3\nDAY 1 2\n", bad) == 0 && !bad.empty(), "bad rows rejected");
    }

    std::printf(g_fails ? "\n%d FAILED\n" : "\nALL OK\n", g_fails);
    return g_fails ? 1 : 0;
}
