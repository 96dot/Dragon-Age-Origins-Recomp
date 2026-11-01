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

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// The guest formats replaced (xenos::TextureFormat): k_DXT1, k_DXT2_3 and
// k_DXT4_5 (colour textures, and the few normal maps stored as DXT), and k_DXN
// (most of the game's normal maps: two channels, which the host has as BC5 as
// is).
constexpr uint32_t kGuestDXT1 = 18, kGuestDXT4_5 = 20, kGuestDXN = 49;
inline bool IsReplacedFormat(uint32_t format) {
  return (format >= kGuestDXT1 && format <= kGuestDXT4_5) ||
         format == kGuestDXN;
}

// A pack file's block format.
enum class FileFormat : uint8_t { kUnknown, kBC1, kBC2, kBC3, kBC4, kBC5, kBC7 };

// Whether a file of this format can stand for a guest texture of that one:
// four channels (BC1/2/3/7) for DXT, two (BC5) for DXN. (BC4, one channel, has
// no replaced guest format to stand for.)
inline bool Fits(FileFormat file, uint32_t guest_format) {
  if (guest_format == kGuestDXN) {
    return file == FileFormat::kBC5;
  }
  return file == FileFormat::kBC1 || file == FileFormat::kBC2 ||
         file == FileFormat::kBC3 || file == FileFormat::kBC7;
}

// Larger .dds files aren't read (a 16384x16384 DXT5 with mips is 358 MB).
constexpr uint64_t kMaxFileBytes = 512ull << 20;
// D3D12's largest 2D texture side.
constexpr uint32_t kMaxSide = 16384;
// Larger sides in a .dds header are taken as damage (and keep the size math
// far from overflowing).
constexpr uint32_t kMaxFileSide = 65536;
// The .dds header (with the magic), and with the DX10 one after it: what
// ParseDds needs to have.
constexpr size_t kDdsHeaderBytes = 128;
constexpr size_t kDdsDx10HeaderBytes = 148;

// What's wrong with a .dds file, if anything.
enum class DdsProblem : uint8_t {
  kNone,
  // Not a 2D .dds of a block format above (or a cube map, volume or array).
  kFormat,
  // Sizes that make no sense, or shorter than its mips.
  kDamaged,
  // Over kMaxFileBytes.
  kTooLarge,
};

struct Dds {
  FileFormat format = FileFormat::kUnknown;
  uint32_t width = 0, height = 0;
  // Down to 1x1 at most.
  uint32_t mip_levels = 0;
  // Where the texels start.
  size_t data_offset = 0;

  uint32_t BlockBytes() const {
    return format == FileFormat::kBC1 || format == FileFormat::kBC4 ? 8 : 16;
  }
  // The bytes of a mip level, in 4x4 blocks.
  size_t LevelBytes(uint32_t level) const {
    return size_t(std::max(((width >> level) + 3) / 4, uint32_t(1))) *
           std::max(((height >> level) + 3) / 4, uint32_t(1)) * BlockBytes();
  }

  // The first mip level loaded: the largest within `max_size` (0: no limit)
  // and D3D12's kMaxSide, as far as there are mips. Whether it can stand for
  // a guest texture of that size: not smaller, and in whole 4x4 blocks as
  // D3D12 requires (and within kMaxSide when no mip is small enough).
  uint32_t FirstLevel(uint32_t max_size) const;
  bool FirstLevelFits(uint32_t max_size, uint32_t guest_width,
                      uint32_t guest_height) const;
};

inline uint32_t Dds::FirstLevel(uint32_t max_size) const {
  max_size = max_size ? std::min(max_size, kMaxSide) : kMaxSide;
  uint32_t level = 0;
  while (level + 1 < mip_levels &&
         std::max(width >> level, height >> level) > max_size) {
    ++level;
  }
  return level;
}

inline bool Dds::FirstLevelFits(uint32_t max_size, uint32_t guest_width,
                                uint32_t guest_height) const {
  uint32_t level = FirstLevel(max_size);
  uint32_t level_width = std::max(width >> level, uint32_t(1));
  uint32_t level_height = std::max(height >> level, uint32_t(1));
  return level_width >= guest_width && level_height >= guest_height &&
         std::max(level_width, level_height) <= kMaxSide &&
         !(level_width % 4) && !(level_height % 4);
}

// Reads a .dds header: `head` has the file's first `head_size` bytes (at least
// kDdsDx10HeaderBytes, or the whole file if it's shorter) and the file is
// `file_size` bytes long.
inline DdsProblem ParseDds(const uint8_t* head, size_t head_size,
                           uint64_t file_size, Dds& dds) {
  if (file_size > kMaxFileBytes) {
    return DdsProblem::kTooLarge;
  }
  if (head_size < kDdsHeaderBytes) {
    return DdsProblem::kFormat;
  }
  auto u32 = [head](size_t offset) {
    uint32_t value;
    std::memcpy(&value, head + offset, sizeof(value));
    return value;
  };
  // After the magic "DDS ": height at 12, width at 16, mip count at 28, the
  // FourCC at 84, caps2 at 112; with "DX10", the DXGI format at 128, the
  // dimension at 132 (3: 2D), the misc flags at 136 (4: cube map) and the
  // array size at 140, and the texels after them.
  constexpr uint32_t kMagic = 0x20534444, kCubemap = 0x200, kVolume = 0x200000;
  constexpr uint32_t kDx10Texture2D = 3, kDx10Cube = 0x4;
  if (u32(0) != kMagic || (u32(112) & (kCubemap | kVolume))) {
    return DdsProblem::kFormat;
  }
  dds.data_offset = kDdsHeaderBytes;
  switch (u32(84)) {
    case 0x31545844:  // DXT1
      dds.format = FileFormat::kBC1;
      break;
    case 0x33545844:  // DXT3
      dds.format = FileFormat::kBC2;
      break;
    case 0x35545844:  // DXT5
      dds.format = FileFormat::kBC3;
      break;
    case 0x31495441:  // ATI1
    case 0x55344342:  // BC4U
      dds.format = FileFormat::kBC4;
      break;
    case 0x32495441:  // ATI2
    case 0x55354342:  // BC5U
      dds.format = FileFormat::kBC5;
      break;
    case 0x30315844:  // DX10
      if (head_size < kDdsDx10HeaderBytes || u32(132) != kDx10Texture2D ||
          (u32(136) & kDx10Cube) || u32(140) > 1) {
        return DdsProblem::kFormat;
      }
      dds.data_offset = kDdsDx10HeaderBytes;
      // DXGI_FORMAT: BC1 71/72 (sRGB), BC2 74/75, BC3 77/78, BC4 80, BC5 83,
      // BC7 98/99 (the sRGB ones as the plain ones, as the game's are).
      switch (u32(128)) {
        case 71:
        case 72:
          dds.format = FileFormat::kBC1;
          break;
        case 74:
        case 75:
          dds.format = FileFormat::kBC2;
          break;
        case 77:
        case 78:
          dds.format = FileFormat::kBC3;
          break;
        case 80:
          dds.format = FileFormat::kBC4;
          break;
        case 83:
          dds.format = FileFormat::kBC5;
          break;
        case 98:
        case 99:
          dds.format = FileFormat::kBC7;
          break;
        default:
          return DdsProblem::kFormat;
      }
      break;
    default:
      return DdsProblem::kFormat;
  }
  dds.height = u32(12);
  dds.width = u32(16);
  if (!dds.width || !dds.height || dds.width > kMaxFileSide ||
      dds.height > kMaxFileSide) {
    return DdsProblem::kDamaged;
  }
  // Not more mips than down to 1x1 (bit_width: log2 + 1).
  dds.mip_levels =
      std::clamp(u32(28), uint32_t(1),
                 uint32_t(std::bit_width(std::max(dds.width, dds.height))));
  uint64_t data_end = dds.data_offset;
  for (uint32_t level = 0; level < dds.mip_levels; ++level) {
    data_end += dds.LevelBytes(level);
  }
  return file_size < data_end ? DdsProblem::kDamaged : DdsProblem::kNone;
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
    // The start's hash and the sample's: unique among the entries (the index
    // builder keeps the first of the same id).
    uint64_t id;
    // The largest width or height of `file` to load (its larger mips are
    // skipped), 0 for any: the size limit, or for a game texture that is a
    // smaller copy of another's picture, the size loaded for the original at
    // the copy's scale.
    uint32_t max_size;
  };
  // Hash of the start (the first min(kMaxIdBytes, texel bytes)) -> the
  // entries with it.
  std::unordered_map<uint64_t, std::vector<Entry>> entries;
  // The lengths those hashes cover, ascending, each once.
  std::vector<uint32_t> lengths;
  // The Shape of every entry: others aren't hashed.
  std::unordered_set<uint64_t> shapes;
  size_t entry_count = 0;
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
