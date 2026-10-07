#pragma once
// vice_city.ini: the few settings of the Vice City map. No game dependencies.
#include <string>
#include <string_view>
#include <vector>

namespace vc {

enum class Mode {
    Auto,     // the client places the map once the server has shown that it uses it (an object with a map model)
    Always,   // the client places the map on every server
    Server,   // the client places nothing; the models are there for the objects the server creates
    Off       // nothing of the map is registered
};

struct Config {
    Mode mode = Mode::Auto;          // [ViceCity] Mode = auto | always | server | off
    float drawDistance = 1.0f;       // DrawDistance: 0.3 .. 2.0, multiplies the distances of the original script
    int maxObjects = 2500;           // MaxObjects: 200 .. 8000 map objects at the same time
    int budgetMs = 4;                // BudgetMs: 1 .. 50, time per frame for objects that are not urgent
    int txdIdleSeconds = 20;         // TxdIdleSeconds: 5 .. 3600, unused texture files are released after this
    int firstModelId = 0;            // FirstModelId: 0 = the highest free model ids, otherwise search upwards from here
    bool specialFlags = false;       // SpecialFlags: 1 = breakable glass, garage doors ... as the PC script has them
    bool worldPatch = true;          // WorldPatch: 0 = leave the game's object limit at x/y 3050 alone (the map will be empty)
};

const char* ModeName(Mode mode);

// Tolerates a UTF-8 BOM, CRLF, ';' and '#' comments. Keys and section names are case-insensitive; keys
// outside [ViceCity] are ignored. `notes` receives what could not be used, for the status file.
void ParseConfig(std::string_view text, Config& out, std::vector<std::string>& notes);

// The text of a new vice_city.ini with every setting at its default, as written next to the models.
std::string DefaultConfigText();

}  // namespace vc
