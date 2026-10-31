#pragma once
// Installing the player's Xbox 360 packages dropped on the launcher or chosen in
// setup (one by one, a folder holding them, as copied from a console's drive, or the
// Ultimate Edition's second disc image, read without extracting it):
//   - downloadable content goes to userdata/content/454108C0/00000002/<content ID>/,
//     where the game finds it (with its name in <content ID>.name);
//   - Title Update 6 or 7 (needed by Awakening and the DLC campaigns after it) goes
//     to title_update/, next to the game folder.
// The packages themselves are only read.

#include <stop_token>

#include "mods/mod_install.h"

namespace dao::content
{
    struct DroppedPackage
    {
        std::filesystem::path path;
        // Found in a folder or disc image dropped (where other games' packages are
        // passed over).
        bool inFolder = false;
        // In a disc image (`path`): where the package is in it (size 0: the whole file).
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    // Takes the packages out of a drop: package files, and folders and disc images
    // with packages in them (each such folder is all packages: none of it becomes a
    // mod). The rest stays in `dropped`.
    std::vector<DroppedPackage> TakePackages(std::vector<std::filesystem::path>& dropped);

    // Installs them; what happened goes to `lines`. `building`: the game is built next
    // (setup), so a title update installed is built in without asking for it.
    void InstallPackages(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot,
        const std::vector<DroppedPackage>& packages, std::stop_token stop,
        std::vector<mods::InstallResult::Line>& lines, bool building = false);
}
