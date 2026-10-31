#pragma once
// Texture packs: PC .dds files in the "texture packs" folder next to the game
// folder (in any folders inside it) replace the game's textures of the same
// name on the graphics card only, so the game keeps its own small copies in
// the 360's 512 MB. The index of which of the game's textures they replace is
// built on a thread of its own at startup and handed to the texture cache
// (xenia/gpu/dao_texture_pack.h); textures that load before it's ready are
// replaced when the game loads them again.

namespace dao::gpu
{
    std::filesystem::path TexturePacksRoot(const std::filesystem::path& gameRoot);

    // With texture packs on and any in the folder.
    void StartTexturePackIndex(const std::filesystem::path& gameRoot);
}
