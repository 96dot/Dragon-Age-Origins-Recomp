#include "stdafx.h"
#include "gpu/texture_pack.h"

#include <set>
#include <stop_token>

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
        // first, the 6-dword fetch constant last, from byte 28; see
        // mods/convert/texture.h).
        constexpr uint32_t kXdsHeaderBytes = 52;
        constexpr size_t kXdsFetchOffset = 28;
        // The largest .xds read (an 8192x8192 DXT5 with mips is 86 MB).
        constexpr uint32_t kMaxXdsBytes = 96u << 20;

        // The files under `root` (none if it can't be listed), stopping early if asked.
        template <typename Function>
        void ForEachFile(const fs::path& root, std::stop_token stop, Function&& function)
        {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && !stop.stop_requested() && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                std::error_code fileEc;
                if (it->is_regular_file(fileEc))
                    function(it->path());
            }
        }

        // The index entry for a game .xds the pack has a file for, or none if it isn't
        // a replaced format.
        std::optional<pack::Index::Entry> XdsEntry(const std::vector<uint8_t>& xds, const fs::path& file,
            uint64_t& startHash, uint32_t& startLength)
        {
            const size_t header = xds.size() - kXdsHeaderBytes;
            if (ui::ReadBE32(xds, header) != 3)
                return std::nullopt;
            // The fetch constant: dword 1 has the format (low 6 bits) and the base
            // address, dword 2 the width - 1 and height - 1 (13 bits each), dword 5 the
            // mips' address (4 KB aligned, 0 without mips), as offsets in the file.
            const size_t fetch = header + kXdsFetchOffset;
            const uint32_t dword1 = ui::ReadBE32(xds, fetch + 4);
            const uint32_t size = ui::ReadBE32(xds, fetch + 8);
            const uint32_t mipAddress = ui::ReadBE32(xds, fetch + 20) & 0xFFFFF000;
            const uint32_t baseAddress = dword1 & 0xFFFFF000;
            pack::Index::Entry entry = {};
            entry.file = file;
            entry.format = dword1 & 0x3F;
            if (!pack::IsReplacedFormat(entry.format))
                return std::nullopt;
            entry.width = (size & 0x1FFF) + 1;
            entry.height = (size >> 13 & 0x1FFF) + 1;
            // The base level: up to the mips, or all the texels.
            size_t baseBytes = header;
            if (mipAddress > baseAddress && mipAddress - baseAddress < header)
                baseBytes = mipAddress - baseAddress;
            startLength = uint32_t(std::min(baseBytes, size_t(pack::kMaxIdBytes)));
            startHash = pack::Hash(xds.data(), startLength);
            // Beyond the start, a sample from the middle of the base level.
            if (baseBytes > pack::kMaxIdBytes)
            {
                entry.sample_offset = uint32_t(baseBytes / 2) & ~uint32_t(0xFFF);
                entry.sample_length = uint32_t(std::min(size_t(pack::kSampleBytes), baseBytes - entry.sample_offset));
                entry.sample_hash = pack::Hash(xds.data() + entry.sample_offset, entry.sample_length);
            }
            entry.id = entry.sample_length ? pack::Hash(reinterpret_cast<const uint8_t*>(&entry.sample_hash),
                                                 sizeof(entry.sample_hash), startHash)
                                           : startHash;
            return entry;
        }

        void BuildIndex(std::stop_token stop, const fs::path& gameRoot, const fs::path& packsRoot,
            uint32_t maxSize, uint64_t memoryBudget)
        {
            const auto start = std::chrono::steady_clock::now();

            // The pack's files by lower-case name; where two packs have the same one,
            // the later path (alphabetically) wins.
            std::vector<fs::path> ddsFiles;
            ForEachFile(packsRoot, stop, [&](const fs::path& path) {
                if (Lower(Utf8(path.extension())) == ".dds")
                    ddsFiles.push_back(path);
            });
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
            // Each entry's file, by id.
            std::unordered_map<uint64_t, fs::path> files;
            size_t collisions = 0;
            std::vector<fs::path> folders = { gameRoot / "packages" / "core" / "data" };
            std::error_code ec;
            for (auto it = fs::directory_iterator(gameRoot / "modules", ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec))
                folders.push_back(it->path() / "data");
            for (const fs::path& data : folders)
            {
                ForEachFile(data, stop, [&](const fs::path& path) {
                    const std::string extension = Lower(Utf8(path.extension()));
                    if (extension != ".erf" && extension != ".rim")
                        return;
                    const auto erf = ui::ErfArchive::Open(path);
                    if (!erf)
                        return;
                    for (const auto& erfEntry : erf->Entries())
                    {
                        const std::string name = Lower(erfEntry.name);
                        if (!name.ends_with(".xds") || erfEntry.size <= kXdsHeaderBytes)
                            continue;
                        const auto file = byName.find(name.substr(0, name.size() - 4));
                        if (file == byName.end())
                            continue;
                        const auto xds = erf->Read(erfEntry, kMaxXdsBytes);
                        if (!xds || xds->size() <= kXdsHeaderBytes)
                            continue;
                        uint64_t startHash;
                        uint32_t startLength;
                        auto entry = XdsEntry(*xds, file->second, startHash, startLength);
                        if (!entry)
                            continue;
                        const auto [same, added] = files.try_emplace(entry->id, entry->file);
                        if (!added)
                        {
                            // The same texels again: the same texture (in another
                            // archive), or two that can't be told apart (the first wins).
                            if (same->second != entry->file)
                                ++collisions;
                            continue;
                        }
                        index->shapes.insert(pack::Shape(entry->width, entry->height, entry->format));
                        index->entries[startHash].push_back(std::move(*entry));
                        lengths.insert(startLength);
                        ++index->entry_count;
                    }
                });
            }
            if (stop.stop_requested())
                return;
            index->lengths.assign(lengths.begin(), lengths.end());
            index->max_size = maxSize;
            index->memory_budget = memoryBudget;
            LOGI("Texture packs: {} of {} textures in texture packs replace the game's ({} ms)",
                index->entry_count, byName.size(),
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (collisions)
                LOGW("Texture packs: {} of the game's textures have the same texels as another of another name", collisions);
            if (index->entry_count)
                pack::Publish(std::move(index));
        }

        // Stopped and joined at exit.
        std::jthread indexThread;
    }

    void StartTexturePackIndex(const fs::path& gameRoot)
    {
        const Settings& settings = GetSettings();
        std::error_code ec;
        if (!settings.texturePacks || !fs::is_directory(TexturePacksRoot(gameRoot), ec))
            return;
        indexThread = std::jthread([gameRoot, maxSize = uint32_t(settings.texturePackMaxSize),
                                       memoryBudget = uint64_t(settings.texturePackMemory) << 20](std::stop_token stop) {
            try
            {
                BuildIndex(stop, gameRoot, TexturePacksRoot(gameRoot), maxSize, memoryBudget);
            }
            catch (const std::exception& e)
            {
                // Out of memory or a damaged archive: no pack this time.
                LOGW("Texture packs: couldn't read the game's textures: {}", e.what());
            }
        });
    }
}
