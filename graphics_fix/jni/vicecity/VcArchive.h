#pragma once
// The archive the game streams the map's models from. It exists only in memory: a directory in the game's
// IMG format, and behind it the .dff files as they lie in the data folder. The game opens it by name like any
// other archive and receives a FILE* that reads from it (see OpenGameFile in ViceCity.h).
// No game dependencies; built on the modloader's virtual archive.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../modloader/ImgOverlay.h"
#include "VcMap.h"

namespace vc {

// The game derives the name of the texture database a model uses from the name of its archive: what follows
// the first '\\', without ".IMG", in lower case. It has to be a database that exists ("gta3"), or the game
// registers a null database for the model.
constexpr char kArchiveName[] = "VICECITY\\GTA3.IMG";
constexpr char kArchiveKey[] = "vicecity/gta3.img";   // the same name as the file layer sees it, normalized
constexpr char kTextureDatabase[] = "gta3";

// Is `gamePath`, as the game hands it to its file layer, the archive? Does not allocate.
bool IsArchivePath(const char* gamePath);

class Archive {
public:
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // One entry "<name>.dff" for every model of `models` that has a game id. False, with `why`, when the
    // archive cannot be made; nothing of it is usable then.
    bool Build(const std::vector<MapModel>& models, std::string& why);

    // A new FILE* that reads the archive; several may be open at once. nullptr when it cannot be made.
    FILE* Open() const;

    // Reads the archive back through such a FILE*, the way the game will: its size, the directory, and the
    // first sector of a sample of the models against the files they come from. False, with `why`, when the
    // device's libc does not deliver what the archive holds; the game must not be given the archive then.
    bool SelfTest(std::string& why) const;

    size_t entryCount() const { return m_files.size(); }   // models in it
    uint64_t sizeBytes() const { return m_overlay ? m_overlay->sizeBytes() : 0; }
    const ml::Overlay* overlay() const { return m_overlay.get(); }

private:
    std::vector<ml::ModFile> m_files;        // entry name -> file on disk; the overlay keeps pointers into it
    std::unique_ptr<ml::Overlay> m_overlay;
};

}  // namespace vc
