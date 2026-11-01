#pragma once
// Texture packs: PC .dds files in the "texture packs" folder next to the game
// folder (in any folders inside it) replace the game's textures of the same
// name on the graphics card only, so the game keeps its own small copies in
// the 360's 512 MB. Which of the game's textures they replace is found on a
// thread of its own at startup (saved in userdata/texture_pack_index.bin, and
// found again only when a pack or the game's files change) and handed to the
// texture cache (xenia/gpu/dao_texture_pack.h); textures that loaded before it
// was ready are replaced once it is. A file also replaces the game's copies of its
// texture's picture under other names (the same size, smaller or larger, such as
// the smaller ones distant models use), at the copy's scale. The launcher shows the
// same check's results: which files are used and why the others aren't.

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace dao::gpu
{
    // (Inline, as it was before the launcher-only build had the rest.)
    inline std::filesystem::path TexturePacksRoot(const std::filesystem::path& gameRoot)
    {
        return gameRoot.parent_path() / "texture packs";
    }

    // Whether a pack's file is used, or why not.
    enum class PackFileStatus : uint8_t
    {
        Used,
        // The game has no texture of that name: made for the PC version (or for DLC).
        NotInGame,
        // A file of the same name in a later folder (alphabetically) is used.
        Shadowed,
        // The game's texture isn't a compressed colour (DXT) or normal map (DXN) one.
        GameFormat,
        // Not a .dds format for that texture: DXT1, DXT3, DXT5 or BC7 for colour
        // textures, BC5 for the game's two-channel normal maps.
        FileFormat,
        Damaged,
        // Couldn't be opened (in use, or not allowed).
        Unreadable,
        // Over 512 MB.
        TooLarge,
        // Smaller than the game's (at the size limit), or its sides aren't multiples of 4.
        BadSize,
        // Another file already replaces a texture with the same texels (the first one
        // found is used).
        SameTexels,
        Count,
    };

    // For the player: why a file isn't used (Used: "used").
    const char* DescribeStatus(PackFileStatus status);

    struct TexturePackReport
    {
        struct File
        {
            // In the texture packs folder (UTF-8).
            std::string path;
            PackFileStatus status;
        };
        // A folder in the texture packs folder ("" for the files directly in it).
        struct Pack
        {
            std::string name;
            size_t files = 0, used = 0;
            // The game's textures that are copies of a used file's texture's picture
            // under other names (any size: smaller ones for distant models, and the
            // like), replaced too.
            size_t copies = 0;
            // The files used, by lower-case name without the extension (for
            // finding those a mod replaces).
            std::vector<std::string> usedNames;
            std::vector<File> skipped;
        };
        std::vector<Pack> packs;
        // The size limit checked with (Settings::texturePackMaxSize).
        int maxSize = 0;
        // What went wrong, if the check couldn't be made.
        std::string error;
    };

    // With texture packs on, when the folder exists: finds which textures they
    // replace on a thread and hands that to the texture cache.
    void StartTexturePackIndex(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot);

    // The same check, for the launcher, with this size limit: blocks (a second or two,
    // or a moment from the saved results), so it's for a thread.
    TexturePackReport CheckTexturePacks(const std::filesystem::path& gameRoot,
        const std::filesystem::path& userRoot, int maxSize, std::stop_token stop);
}
