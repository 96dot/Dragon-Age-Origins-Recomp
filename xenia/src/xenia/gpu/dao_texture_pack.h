// DAO recomp: texture packs (PC .dds files replacing the game's textures on the
// host GPU only, so the guest keeps its own small copies in its 512 MB).
//
// A texture is known by its content: the game's .xds files are the texels as
// they are in guest memory, followed by a 52-byte header, so a hash of the
// start of a texture's base level in guest memory is the hash of the start of
// its file. The index (built from the game's archives and the pack folder by
// DAORecomp/gpu/texture_pack.cpp, on its own thread) maps those hashes to the
// pack's files; the texture cache (d3d12_texture_cache.cc) looks textures up
// in it as they load.
#ifndef XENIA_GPU_DAO_TEXTURE_PACK_H_
#define XENIA_GPU_DAO_TEXTURE_PACK_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xe::gpu::dao_texture_pack {

// How much of a texture identifies it: the first 64 KB of its texels, or all
// of them if it's smaller.
constexpr uint32_t kMaxIdBytes = 64 * 1024;

constexpr uint64_t kHashBasis = 0xCBF29CE484222325ull;

// FNV-1a 64, continued from `hash` (kHashBasis to start).
inline uint64_t Hash(const uint8_t* data, size_t size,
                     uint64_t hash = kHashBasis) {
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  return hash;
}

struct Index {
  struct Entry {
    std::filesystem::path file;
    // The game's texture's size (its file's fetch constant): a texture of
    // another size with the same first bytes isn't this one.
    uint32_t width, height;
  };
  // Hash of the first min(kMaxIdBytes, texel bytes) of a game file -> its
  // replacement.
  std::unordered_map<uint64_t, Entry> entries;
  // The lengths those hashes cover, ascending, each once.
  std::vector<uint32_t> lengths;
  // The largest width or height of a replacement to load (its larger mips
  // are skipped), 0 for any.
  uint32_t max_size = 0;
  // Video memory the replacements may take, in bytes (0: half the graphics
  // card's own).
  uint64_t memory_budget = 0;

  // The id of the texture whose texels start at `data` (`available` bytes
  // readable from there) and that has this size, or 0 if it isn't replaced.
  // One pass over at most kMaxIdBytes; the longest match wins.
  uint64_t Identify(const uint8_t* data, size_t available, uint32_t width,
                    uint32_t height) const {
    uint64_t hash = kHashBasis, found = 0;
    size_t hashed = 0;
    for (uint32_t length : lengths) {
      if (length > available) {
        break;
      }
      hash = Hash(data + hashed, length - hashed, hash);
      hashed = length;
      auto it = entries.find(hash);
      if (it != entries.end() && it->second.width == width &&
          it->second.height == height) {
        found = hash;
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
