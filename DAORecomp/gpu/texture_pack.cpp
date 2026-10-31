#include "stdafx.h"
#include "gpu/texture_pack.h"

#include <set>

#include "settings.h"
#include "text.h"
#include "ui/erf.h"
#include "xenia/gpu/dao_texture_pack.h"

namespace dao::gpu
{
    namespace fs = std::filesystem;
    namespace pack = xe::gpu::dao_texture_pack;

    namespace
    {
        // A .xds ends with the 52-byte D3D texture header the game reads (Common 3
        // first, the fetch constant last; see mods/convert/texture.h).
        constexpr uint32_t kXdsHeaderBytes = 52;
        // The largest .xds read (an 8192x8192 DXT5 with mips is 86 MB).
        constexpr uint32_t kMaxXdsBytes = 96u << 20;

        void BuildIndex(const fs::path& gameRoot, const fs::path& packsRoot, uint32_t maxSize,
            uint64_t memoryBudget)
        {
            const auto start = std::chrono::steady_clock::now();

            // The pack's files by lower-case name; where two packs have the same one,
            // the later path (alphabetically) wins.
            std::vector<fs::path> ddsFiles;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(packsRoot, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_regular_file(ec) && Lower(Utf8(it->path().extension())) == ".dds")
                    ddsFiles.push_back(it->path());
            }
            std::ranges::sort(ddsFiles);
            std::unordered_map<std::string, fs::path> byName;
            for (const fs::path& file : ddsFiles)
                byName[Lower(Utf8(file.stem()))] = file;
            if (byName.empty())
                return;

            // The game's .xds of those names, from the archives the game reads (as
            // mods::GameFiles finds them). A mod's replacement of one has other
            // texels, so the pack doesn't replace it.
            auto index = std::make_shared<pack::Index>();
            std::set<uint32_t> lengths;
            std::vector<fs::path> folders = { gameRoot / "packages" / "core" / "data" };
            for (const auto& module : fs::directory_iterator(gameRoot / "modules", ec))
                folders.push_back(module.path() / "data");
            for (const fs::path& data : folders)
            {
                for (auto it = fs::recursive_directory_iterator(data, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    const std::string extension = Lower(Utf8(it->path().extension()));
                    if (extension != ".erf" && extension != ".rim")
                        continue;
                    const auto erf = ui::ErfArchive::Open(it->path());
                    if (!erf)
                        continue;
                    for (const auto& entry : erf->Entries())
                    {
                        const std::string name = Lower(entry.name);
                        if (!name.ends_with(".xds") || entry.size <= kXdsHeaderBytes)
                            continue;
                        const auto file = byName.find(name.substr(0, name.size() - 4));
                        if (file == byName.end())
                            continue;
                        const auto xds = erf->Read(entry, kMaxXdsBytes);
                        if (!xds || xds->size() <= kXdsHeaderBytes)
                            continue;
                        const size_t header = xds->size() - kXdsHeaderBytes;
                        if (ui::ReadBE32(*xds, header) != 3)
                            continue;
                        // The fetch constant's third dword: width - 1, height - 1 (13 bits each).
                        const uint32_t size = ui::ReadBE32(*xds, header + 28 + 8);
                        const uint32_t length = uint32_t(std::min(header, size_t(pack::kMaxIdBytes)));
                        const uint64_t id = pack::Hash(xds->data(), length);
                        if (index->entries.try_emplace(id,
                                pack::Index::Entry{ file->second, (size & 0x1FFF) + 1, (size >> 13 & 0x1FFF) + 1 }).second)
                            lengths.insert(length);
                    }
                }
            }
            index->lengths.assign(lengths.begin(), lengths.end());
            index->max_size = maxSize;
            index->memory_budget = memoryBudget;
            LOGI("Texture packs: {} of {} textures in {} replace the game's ({} ms)", index->entries.size(),
                byName.size(), Utf8(packsRoot),
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (!index->entries.empty())
                pack::Publish(std::move(index));
        }
    }

    fs::path TexturePacksRoot(const fs::path& gameRoot)
    {
        return gameRoot.parent_path() / "texture packs";
    }

    void StartTexturePackIndex(const fs::path& gameRoot)
    {
        const Settings& settings = GetSettings();
        if (!settings.texturePacks || !fs::is_directory(TexturePacksRoot(gameRoot)))
            return;
        std::thread(BuildIndex, gameRoot, TexturePacksRoot(gameRoot), uint32_t(settings.texturePackMaxSize),
            uint64_t(settings.texturePackMemory) << 20).detach();
    }
}
