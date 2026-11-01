#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include <unordered_set>

#include "gpu/texture_pack.h"

namespace dao::launcher
{
    namespace
    {
        // The names of kTexturePackMaxSizes.
        constexpr const char* kTexturePackMaxSizeNames[] = { "No limit", "1024 (1K)", "2048 (2K)", "4096 (4K)",
            "8192 (8K)" };
        static_assert(std::size(kTexturePackMaxSizeNames) == std::size(kTexturePackMaxSizes));
        // The names of kTexturePackMemories.
        constexpr const char* kTexturePackMemoryNames[] = { "Automatic (half the graphics memory)", "512 MB",
            "1 GB", "2 GB", "3 GB", "4 GB", "6 GB", "8 GB" };
        static_assert(std::size(kTexturePackMemoryNames) == std::size(kTexturePackMemories));

        // The texture pack check's results, taken when it's done; started when there
        // are none, they're for another size limit, or `again` (after the one running).
        void UpdateTexturePackCheck(State& st, bool again)
        {
            st.packCheckAgain |= again;
            if (st.packCheck.valid())
            {
                if (st.packCheck.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return;
                st.packReport = st.packCheck.get();
            }
            if (st.packReport && !st.packCheckAgain && st.packReport->maxSize == st.settings.texturePackMaxSize)
                return;
            st.packCheckAgain = false;
            st.packCheck = std::async(std::launch::async,
                [gameRoot = st.gameRoot, userRoot = st.userRoot, maxSize = st.settings.texturePackMaxSize,
                    stop = st.packStop.get_token()] {
                    return gpu::CheckTexturePacks(gameRoot, userRoot, maxSize, stop);
                });
        }

        // Which pack files are used, and why the others aren't, by pack.
        void TexturePackStatus(State& st)
        {
            UpdateTexturePackCheck(st, false);
            if (!st.packReport)
            {
                HelpText("Checking the texture packs...");
                return;
            }
            const gpu::TexturePackReport& report = *st.packReport;
            if (!report.error.empty())
            {
                HelpText("The texture packs couldn't be checked: something went wrong (see dao.log).");
                return;
            }
            if (report.packs.empty())
            {
                HelpText("No texture packs in the folder yet.");
                return;
            }
            // The textures the loaded mods put in the override folder: the game uses
            // theirs, not the pack's.
            constexpr std::string_view kOverride = "override/", kXds = ".xds";
            std::unordered_set<std::string> modTextures;
            for (const mods::ModInfo& mod : st.mods)
            {
                if (!mod.IsLoaded())
                    continue;
                for (const std::string& change : mod.changes)
                {
                    if (change.starts_with(kOverride) && change.ends_with(kXds))
                        modTextures.insert(
                            change.substr(kOverride.size(), change.size() - kOverride.size() - kXds.size()));
                }
            }
            for (const gpu::TexturePackReport::Pack& pack : report.packs)
            {
                const size_t byMods = size_t(std::ranges::count_if(pack.usedNames,
                    [&](const std::string& name) { return modTextures.contains(name); }));
                const std::string name = pack.name.empty() ? "Files in the folder itself" : pack.name;
                if (pack.copies)
                    ImGui::TextWrapped("%s: %zu of %zu files used, also for %zu of the game's copies of their "
                                       "textures under other names (such as smaller ones for distant models)",
                        name.c_str(), pack.used - byMods, pack.files, pack.copies);
                else
                    ImGui::TextWrapped("%s: %zu of %zu files used", name.c_str(), pack.used - byMods, pack.files);
                if (pack.skipped.empty() && !byMods)
                    continue;
                ImGui::PushID(name.c_str());
                if (ImGui::TreeNode("Files not used"))
                {
                    if (byMods)
                        ImGui::BulletText("%zu: a mod replaces the texture, and the mod's is used", byMods);
                    // By reason, in PackFileStatus's order.
                    for (int status = 1; status < int(gpu::PackFileStatus::Count); ++status)
                    {
                        std::vector<const std::string*> files;
                        for (const auto& file : pack.skipped)
                        {
                            if (int(file.status) == status)
                                files.push_back(&file.path);
                        }
                        if (files.empty())
                            continue;
                        const std::string title = fmt::format("{}: {}##{}", files.size(),
                            gpu::DescribeStatus(gpu::PackFileStatus(status)), status);
                        if (ImGui::TreeNode(title.c_str()))
                        {
                            // The first ones, as examples (a pack for the PC version
                            // may have thousands the game hasn't).
                            constexpr size_t kShown = 100;
                            for (size_t i = 0; i < std::min(files.size(), kShown); ++i)
                                ImGui::BulletText("%s", files[i]->c_str());
                            if (files.size() > kShown)
                                ImGui::BulletText("and %zu more", files.size() - kShown);
                            ImGui::TreePop();
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }

        void TexturePackList(State& st)
        {
            Settings& s = st.settings;
            Section("Texture packs");
            ImGui::Checkbox("Use texture packs", &s.texturePacks);
            HelpText("PC texture packs (.dds files, such as from Nexus Mods) put in the texture packs folder "
                     "replace the game's textures of the same name, on the graphics card only, and the game's "
                     "copies of those textures under other names. They load in the background, so a texture may "
                     "sharpen a moment after it appears. In the game, F9 switches between the packs' textures and "
                     "the game's own, to compare.");
            if (!s.texturePacks)
                return;

            Section("Packs");
            TexturePackStatus(st);
            ImGui::Spacing();
            if (ImGui::Button("Open the texture packs folder"))
                OpenFolder(gpu::TexturePacksRoot(st.gameRoot));
            ImGui::SameLine();
            if (ImGui::Button("Check again"))
                UpdateTexturePackCheck(st, true);

            Section("Graphics memory");
            int maxSize = int(std::ranges::find(kTexturePackMaxSizes, s.texturePackMaxSize) -
                std::begin(kTexturePackMaxSizes));
            if (maxSize >= int(std::size(kTexturePackMaxSizes)))
                maxSize = 0;
            if (ComboRow("Size limit", &maxSize, kTexturePackMaxSizeNames, int(std::size(kTexturePackMaxSizeNames))))
                s.texturePackMaxSize = kTexturePackMaxSizes[maxSize];
            HelpText("Larger textures are loaded at this size: less graphics memory, for the Steam Deck or cards "
                     "with 4 GB or less.");
            // A value set in config.toml that isn't offered: shown as it is, first.
            std::vector<const char*> memoryNames(std::begin(kTexturePackMemoryNames),
                std::end(kTexturePackMemoryNames));
            std::string customMemory;
            int memory = int(std::ranges::find(kTexturePackMemories, s.texturePackMemory) -
                std::begin(kTexturePackMemories));
            const bool custom = memory >= int(std::size(kTexturePackMemories));
            if (custom)
            {
                customMemory = fmt::format("{} MB (set in config.toml)", s.texturePackMemory);
                memoryNames.insert(memoryNames.begin(), customMemory.c_str());
                memory = 0;
            }
            if (ComboRow("Memory", &memory, memoryNames.data(), int(memoryNames.size())))
            {
                // (The value from config.toml, picked again, stays.)
                const int offered = custom ? memory - 1 : memory;
                if (offered >= 0)
                    s.texturePackMemory = kTexturePackMemories[offered];
            }
            HelpText("The most graphics memory texture packs take. When it's full, the textures not seen lately "
                     "go back to the game's to make room.");
        }

        void RestoreTexturePacks(Settings& s)
        {
            const Settings defaults;
            s.texturePacks = defaults.texturePacks;
            s.texturePackMaxSize = defaults.texturePackMaxSize;
            s.texturePackMemory = defaults.texturePackMemory;
        }
    }

    // (Until the view takes the results: it polls the check each frame it shows the
    // packs, which screenshot mode's wait for it relies on.)
    bool CheckingTexturePacks(const State& st)
    {
        return st.packCheck.valid();
    }

    void TexturePacksView(State& st, const Layout& layout)
    {
        PanelFrame(st, layout, "Texture Packs", RestoreTexturePacks);
        ShowContents(st, layout, kListLeft, TexturePackList);
    }
}
