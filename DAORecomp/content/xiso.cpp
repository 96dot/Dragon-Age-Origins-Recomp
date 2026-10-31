#include "stdafx.h"
#include "content/xiso.h"

#include <fstream>
#include <ranges>
#include <unordered_set>

#include "files.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr std::string_view kMagic = "MICROSOFT*XBOX*MEDIA";
        // Where the game partition starts: plain XDVDFS, XGD1, XGD2, XGD3.
        constexpr uint64_t kPartitionOffsets[] = { 0x0, 0x18300000, 0xFD90000, 0x2080000 };
        // A directory's table is a few KB; anything far larger isn't one. Listing a disc
        // reads at most kMaxListed of them in all (a real disc's are well under 1 MB).
        constexpr uint32_t kMaxTableSize = 16 << 20;
        constexpr uint64_t kMaxListed = 64 << 20;
        constexpr uint8_t kDirectory = 0x10;
        // An entry: left and right subtree (in 4-byte units), sector, size, attributes,
        // name length, then the name.
        constexpr size_t kEntryHeader = 14;

        struct Entry
        {
            uint32_t sector = 0, size = 0;
            bool directory = false;
        };

        template <typename T>
        T ReadLe(const uint8_t* p)
        {
            T value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        bool ReadAt(std::ifstream& in, uint64_t offset, void* data, size_t size)
        {
            in.clear();
            return in.seekg(std::streamoff(offset)) && in.read(static_cast<char*>(data), std::streamsize(size));
        }

        // An open disc image: where its game partition starts, and its root folder.
        struct Image
        {
            std::ifstream in;
            uint64_t base = 0;
            Entry root;
        };

        bool OpenImage(const fs::path& iso, Image& image, std::string& error)
        {
            image.in.open(iso, std::ios::binary);
            if (!image.in)
            {
                error = "can't open the disc image";
                return false;
            }
            for (const uint64_t offset : kPartitionOffsets)
            {
                char magic[kMagic.size()];
                uint8_t root[8];
                if (ReadAt(image.in, offset + 32 * kSector, magic, sizeof(magic)) &&
                    std::string_view(magic, sizeof(magic)) == kMagic &&
                    ReadAt(image.in, offset + 32 * kSector + kMagic.size(), root, sizeof(root)))
                {
                    image.base = offset;
                    image.root = { ReadLe<uint32_t>(root), ReadLe<uint32_t>(root + 4), true };
                    return true;
                }
            }
            error = "not an Xbox 360 disc image";
            return false;
        }

        // Calls visit(name, entry) for each entry of the directory table at `dir`,
        // visiting its binary tree once per entry, until visit returns false.
        template <typename Visit>
        void ForEachEntry(Image& image, const Entry& dir, Visit&& visit)
        {
            if (!dir.directory || dir.size == 0 || dir.size > kMaxTableSize)
                return;
            std::vector<uint8_t> table(dir.size);
            if (!ReadAt(image.in, image.base + dir.sector * kSector, table.data(), table.size()))
                return;
            std::vector<bool> seen(table.size() / 4 + 1);
            std::vector<uint32_t> stack = { 0 };
            while (!stack.empty())
            {
                const uint32_t offset = stack.back();
                stack.pop_back();
                if (offset + kEntryHeader > table.size() || seen[offset / 4])
                    continue;
                seen[offset / 4] = true;
                const uint8_t* p = table.data() + offset;
                const uint16_t left = ReadLe<uint16_t>(p), right = ReadLe<uint16_t>(p + 2);
                const uint32_t sector = ReadLe<uint32_t>(p + 4);
                if (left == 0xFFFF && right == 0xFFFF && sector == 0xFFFFFFFF)
                    continue;
                const size_t nameLength = p[13];
                if (offset + kEntryHeader + nameLength > table.size())
                    continue;
                const std::string_view name(reinterpret_cast<const char*>(p + kEntryHeader), nameLength);
                if (!visit(name, Entry{ sector, ReadLe<uint32_t>(p + 8), (p[12] & kDirectory) != 0 }))
                    return;
                if (left && left != 0xFFFF)
                    stack.push_back(uint32_t(left) * 4);
                if (right && right != 0xFFFF)
                    stack.push_back(uint32_t(right) * 4);
            }
        }

        // The entry at `path` ('/'-separated, any case).
        std::optional<Entry> Find(Image& image, std::string_view path)
        {
            std::optional<Entry> entry = image.root;
            for (const auto part : std::views::split(path, '/'))
            {
                const std::string wanted = Lower(std::string_view(part.begin(), part.end()));
                std::optional<Entry> found;
                ForEachEntry(image, *entry, [&](std::string_view name, const Entry& child) {
                    if (Lower(name) == wanted)
                        found = child;
                    return !found;
                });
                if (!found)
                    return std::nullopt;
                entry = found;
            }
            return entry;
        }
    }

    bool ExtractIsoFile(const fs::path& iso, std::string_view path, const fs::path& out, uint64_t maxSize,
        std::string& error)
    {
        Image image;
        if (!OpenImage(iso, image, error))
            return false;
        const std::optional<Entry> entry = Find(image, path);
        if (!entry)
        {
            error = fmt::format("{} isn't on the disc image", path);
            return false;
        }
        if (entry->directory || entry->size > maxSize)
        {
            error = fmt::format("{} on the disc image isn't a file it can use", path);
            return false;
        }

        std::vector<char> data(entry->size);
        if (!ReadAt(image.in, image.base + entry->sector * kSector, data.data(), data.size()) ||
            !WriteFileAtomically(out, std::string_view(data.data(), data.size())))
        {
            error = fmt::format("can't copy {} out of the disc image", path);
            return false;
        }
        return true;
    }

    bool IsoHasFile(const fs::path& iso, std::string_view path)
    {
        Image image;
        std::string error;
        if (!OpenImage(iso, image, error))
            return false;
        const std::optional<Entry> entry = Find(image, path);
        return entry && !entry->directory;
    }

    bool ListIsoFiles(const fs::path& iso, int maxDepth, size_t maxFiles, std::vector<IsoFile>& files,
        std::string& error)
    {
        Image image;
        if (!OpenImage(iso, image, error))
            return false;
        struct Folder
        {
            Entry entry;
            std::string path;
            int depth = 0;
        };
        std::vector<Folder> folders = { { image.root, "", 0 } };
        // Each folder's table is read once (a damaged image's may point back at another).
        std::unordered_set<uint32_t> tables;
        uint64_t listed = 0;
        size_t seen = 0;
        while (!folders.empty() && seen < maxFiles)
        {
            const Folder folder = std::move(folders.back());
            folders.pop_back();
            listed += folder.entry.size;
            if (!tables.insert(folder.entry.sector).second || listed > kMaxListed)
                continue;
            ForEachEntry(image, folder.entry, [&](std::string_view name, const Entry& entry) {
                const std::string path = folder.path.empty() ? std::string(name)
                                                             : fmt::format("{}/{}", folder.path, name);
                if (!entry.directory)
                    files.push_back({ path, image.base + entry.sector * kSector, entry.size });
                else if (folder.depth < maxDepth)
                    folders.push_back({ entry, path, folder.depth + 1 });
                return ++seen < maxFiles;
            });
        }
        return true;
    }
}
