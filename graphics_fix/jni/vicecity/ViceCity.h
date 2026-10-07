#pragma once
// Vice City for the 0.3.7 client: the map of github.com/casualmind/samp-vice-city (a SA-MP 0.3.DL custom
// model map) built into the client. See vicecity/README.md.
//
// The models (PC .dff with the collision SA-MP embeds, PC .txd) are read from TESTLIT/vice_city/ as they come
// from that repository. The client registers them with the game under model ids of its own, places the map
// itself around the player, and maps the negative 0.3.DL model ids of server objects to those models.
//
// Every function here may be called at any time and from hooks: none of them lets an exception out, and
// without the data folder each one does what the client did before there was a map.
#include <cstdint>
#include <cstdio>

struct RwTexture;
class CVector;

namespace vc {

// Looks for the data folder and reads vice_city.ini. Only the first call does anything; every function below
// calls it when needed.
void Initialize();
// True when the data folder holds models and the map is not switched off.
bool Present();

// --- hooks (JNI_OnLoad, with the client's other hooks) ---
// Hooks CStreaming::Init2: the models are registered with the game right before it reads its archives.
void InstallHooks();

// --- NvFOpen ---
// The virtual archive the models are streamed from, when `gamePath` names it; otherwise nullptr.
FILE* OpenGameFile(const char* gamePath);

// --- RwTextureRead hook ---
bool WantsTextureHook();
void SetTextureHookInstalled(bool installed);
// Texture `name` from the TXD file of the map model the game is loading, with a reference for the caller;
// nullptr for every other model.
RwTexture* FindTexture(const char* name);

// --- CStreaming ---
void Tick();               // Update: once per frame, on the game thread
void OnMemoryPressure();   // MakeSpaceFor

// --- network (game thread) ---
// Model id in the game for an object the server creates with `sampModelId`; to be asked for every object.
// Ids of the 0.3.DL script are negative and are mapped to the map's models. Any other id is returned as it
// is, unless it is one the client gave to a map model: the server cannot mean that model by it.
// -1: no such model.
int ResolveServerModel(int32_t sampModelId);
// A server object with this model at `pos`; to be asked for every object the server creates. True when the
// client has placed that very object itself, so the server's copy must not be created. Also how the client
// learns that the server uses the map (Mode = auto): the object repeats a placement of the map, or has one
// of the map's models.
bool OnServerObject(int32_t sampModelId, const CVector& pos);
// The connection was reset or closed: the map goes away until a server asks for it again.
void OnNetworkReset();

// --- text draws ---
// Sprite "mdl<id>:<texture>" of a text draw with font 4: the texture from the TXD of that 0.3.DL model
// (-1500 is the minimap of vc_minimap.pwn). nullptr when `txdName` is not of that form or nothing is found.
RwTexture* FindSprite(const char* txdName, const char* textureName);

}  // namespace vc
