#pragma once
// The modloader as the rest of the client sees it. Loose files in TESTLIT/modloader/ are read by the game
// directly: ordinary files through NvFOpen, IMG contents through overlays on the registered archives, PC
// .txd files through the RwTextureRead hook. See modloader/README.md.
//
// Most of these functions are called from hooks, with the game's own frames above them: none of them lets an
// exception out. When the modloader could not be set up at all (no memory to scan the mod folder) every one
// of them does what the client did without a modloader.
#include <cstddef>
#include <cstdio>
#include <string>

struct RwTexture;

namespace ml {

// Scans the mod folder. Only the first call does anything, and every function below calls it when needed.
void Initialize();
// The mod folder with a trailing '/', "" when it could not be found or created.
const char* Root();

// --- NvFOpen ---
// Opens the file the game asks for. In this order: the overlay of an IMG archive (loose mod files layered
// on top of it); the mod file that replaces `gamePath`, or that replaces the file `remap` substitutes for
// it (never a .txd); a full path into the mod folder, as it is; the game's own file, at the path
// `buildPath` gives. `opened` receives what was opened or tried last, for the log. nullptr when nothing
// could be opened.
FILE* OpenGameFile(const char* gamePath, const char* (*remap)(const char* gamePath),
                   void (*buildPath)(const char* gamePath, char* out, size_t cap), char* opened, size_t openedCap);

// --- data files the client parses itself ---
// fopen() for a game data file the client reads on its own, next to the game or instead of it (handling.cfg,
// vehicles.ide, weapon.dat, ...): the mod file with that name when there is one, otherwise `path` as it is.
// Only plain reading ("r", "rb") is redirected; a file the client writes or updates is opened as asked.
FILE* OpenClientFile(const char* path, const char* mode);

// --- CStreaming ---
// InitImageList: `archives` are the IMG files about to be registered; `buildPath` gives the path of a game
// file on disk.
void PrepareStreaming(const char* const* archives, size_t count,
                      void (*buildPath)(const char* gamePath, char* out, size_t cap));
void Tick();               // Update: once per frame
void OnMemoryPressure();   // MakeSpaceFor
// For a part of the client that registers an IMG archive of its own (vicecity/): the game remembers which
// model every DFF entry of its archives belongs to by position, in MODELS/MINFO.BIN, and one more archive
// shifts the positions. Makes the game look the entries up by name instead, exactly as it is done for
// overlays. Call before the game reads its archive directories. False when that cannot be done.
bool LookUpModelsByName();

// --- RwTextureRead hook ---
// "<textureName>.png" from the mod folder, otherwise nullptr.
const char* FindPng(const char* textureName);
// Texture from the loose .txd of the TXD the game is reading (or of one of its parents), with a reference
// for the caller; otherwise nullptr.
RwTexture* FindTexture(const char* name);
// True when the mod folder holds something the RwTextureRead hook could serve (a PNG, or a loose .txd with
// that feature on). Otherwise the hook is not installed at all.
bool WantsTextureHook();
// For the report: whether the RwTextureRead hook could be installed.
void SetTextureHookInstalled(bool installed);

// While alive, FindTexture() on this thread looks in "<txdName>.txd" first, whatever TXD the game has
// current. For the places where the client reads a model file itself.
class ScopedTxdContext {
public:
    explicit ScopedTxdContext(const char* txdName) noexcept;
    ~ScopedTxdContext();
    ScopedTxdContext(const ScopedTxdContext&) = delete;
    ScopedTxdContext& operator=(const ScopedTxdContext&) = delete;

private:
    std::string m_name;
    const std::string* m_previous;
};

}  // namespace ml
