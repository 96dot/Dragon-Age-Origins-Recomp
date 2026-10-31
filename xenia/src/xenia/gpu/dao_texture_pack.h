// DAO recomp: texture packs (PC .dds files replacing the game's textures on the
// host GPU only, so the guest keeps its own small copies in its 512 MB).
//
// A texture is known by its content: the game's .xds files are the texels as
// they are in guest memory (base level first), followed by a 52-byte header,
// so the start of a texture's base level in guest memory is the start of its
// file. The index (built from the game's archives and the pack folder by
// DAORecomp/gpu/texture_pack.cpp, on its own thread) keys the pack's files by
// a hash of that start, and checks a second sample from the middle of the base
// level and the texture's size and format, so textures that only share their
// first texels (a blank corner) aren't taken for one another. The texture
// cache (d3d12_texture_cache.cc) looks textures up in it as they load.
#ifndef XENIA_GPU_DAO_TEXTURE_PACK_H_
#define XENIA_GPU_DAO_TEXTURE_PACK_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xe::gpu::dao_texture_pack {

// The hashed start of a texture: its first 64 KB of texels, or all of them if
// it's smaller.
constexpr uint32_t kMaxIdBytes = 64 * 1024;
// The second sample (for base levels beyond the start): this much from the
// middle of the base level, on a 4 KB boundary.
constexpr uint32_t kSampleBytes = 16 * 1024;

// The guest formats replaced (xenos::TextureFormat k_DXT1, k_DXT2_3,
// k_DXT4_5): the colour textures packs are made of, whose channels the fetch
// constants don't reorder.
inline bool IsReplacedFormat(uint32_t format) {
  return format >= 18 && format <= 20;
}

constexpr uint64_t kHashBasis = 0xCBF29CE484222325ull;

// FNV-1a 64, continued from `hash` (kHashBasis to start).
inline uint64_t Hash(const uint8_t* data, size_t size,
                     uint64_t hash = kHashBasis) {
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  return hash;
}

// A texture's size and format as one key.
inline uint64_t Shape(uint32_t width, uint32_t height, uint32_t format) {
  return uint64_t(width) | uint64_t(height) << 16 | uint64_t(format) << 32;
}

struct Index {
  struct Entry {
    std::filesystem::path file;
    // The game's texture's size and format (from its file's fetch
    // constant).
    uint32_t width, height, format;
    // The second sample: where from the start of the base level, how long
    // (0: none, the start covers the base level) and its hash.
    uint32_t sample_offset, sample_length;
    uint64_t sample_hash;
    // Unique for each entry: the start's hash and the sample's.
    uint64_t id;
  };
  // Hash of the start (the first min(kMaxIdBytes, texel bytes)) -> the
  // entries with it.
  std::unordered_map<uint64_t, std::vector<Entry>> entries;
  // The lengths those hashes cover, ascending, each once.
  std::vector<uint32_t> lengths;
  // The Shape of every entry: others aren't hashed.
  std::unordered_set<uint64_t> shapes;
  size_t entry_count = 0;
  // The largest width or height of a replacement to load (its larger mips
  // are skipped), 0 for any.
  uint32_t max_size = 0;
  // Video memory the replacements may take, in bytes (0: half the graphics
  // card's own, at least 256 MB).
  uint64_t memory_budget = 0;

  // The entry for the texture whose texels start at `data` (`available`
  // bytes readable from there) and that has this shape, or null. One pass
  // over at most kMaxIdBytes, plus the sample of a match; the longest match
  // wins.
  const Entry* Identify(const uint8_t* data, size_t available, uint64_t shape)
      const {
    if (!shapes.count(shape)) {
      return nullptr;
    }
    uint64_t hash = kHashBasis;
    size_t hashed = 0;
    const Entry* found = nullptr;
    for (uint32_t length : lengths) {
      if (length > available) {
        break;
      }
      hash = Hash(data + hashed, length - hashed, hash);
      hashed = length;
      auto it = entries.find(hash);
      if (it == entries.end()) {
        continue;
      }
      for (const Entry& entry : it->second) {
        if (Shape(entry.width, entry.height, entry.format) == shape &&
            (!entry.sample_length ||
             (size_t(entry.sample_offset) + entry.sample_length <= available &&
              Hash(data + entry.sample_offset, entry.sample_length) ==
                  entry.sample_hash))) {
          found = &entry;
          break;
        }
      }
    }
    return found;
  }
};

namespace detail {
inline std::mutex mutex;
inline std::shared_ptr<const Index> index;
}  // namespace detail

// Set once the index is built (any thread).
inline void Publish(std::shared_ptr<const Index> index) {
  std::lock_guard<std::mutex> lock(detail::mutex);
  detail::index = std::move(index);
}

// The index, or null until it's built (or with no pack).
inline std::shared_ptr<const Index> Get() {
  std::lock_guard<std::mutex> lock(detail::mutex);
  return detail::index;
}

}  // namespace xe::gpu::dao_texture_pack

#endif  // XENIA_GPU_DAO_TEXTURE_PACK_H_
