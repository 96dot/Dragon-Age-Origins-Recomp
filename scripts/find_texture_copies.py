"""Finds the game's textures that are copies of another's picture under another name, for
texture packs (DAORecomp/gpu/texture_pack.cpp): a pack file for the original then also
replaces its copies. Writes DAORecomp/gpu/texture_copies.inc.

Copies may be the same size, smaller or larger: the half and quarter size ones distant
models use (the name with l2 or l3 added), terrain copies of props' textures (ter_...),
armour shared between bodies, and the like. Each pair is written both ways where both
names aren't an LOD of the other. A pair counts when both are compressed textures of the
formats packs replace, both colour (DXT) or both normal maps (DXN), their sizes differ by
a power of two in both directions, their names end in the same kind of map (the letter
before an LOD's l2), and their pictures match: each reduced to one colour and alpha per
4x4 block (from the blocks' endpoints and indices), the larger box-filtered to the
smaller's grid, differing by at most MAX_LOD_DIFFERENCE (an LOD of the same name) or
MAX_DIFFERENCE (0-255 per channel, on average), and by little in the worst blocks too.
Other names are compared as 16x16 thumbnails first. The console version's re-laid out
"_fixed" textures (the castle rugs) are left out: plain ones can pass for them.

    python -I scripts/find_texture_copies.py <game folder>

Needs numpy and Pillow, and the game folder the installer makes (packages/core/data and
modules/; its textures are the same on the disc and every title update). Not run by CI:
run it again after changing it, and commit the table.
"""
import os
import re
import struct
import sys
import zlib

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'DAORecomp', 'gpu', 'texture_copies.inc')
# The .xds layout, as texture_pack.cpp reads it: the texels, then a 52-byte D3D texture
# header (Common 3 for a texture, the fetch constant from byte 28: dword 1 has the format
# in its low 6 bits, dword 2 the width - 1 and height - 1 in 13 bits each).
XDS_HEADER_BYTES = 52
XDS_COMMON_TEXTURE = 3
XDS_FETCH_OFFSET = 28
# The formats packs replace (dao_texture_pack.h's IsReplacedFormat), with their block bytes
# (Dds::BlockBytes): DXT1, DXT2_3, DXT4_5, DXN.
FORMATS = {18: 8, 19: 16, 20: 16, 49: 16}
DXN = 49
THUMB = 16
# Thumbnail correlation for a pair to be checked closely, and the least detail (standard
# deviation, 0-255) a thumbnail needs: flat textures match anything.
MIN_SIMILARITY = 0.97
MIN_DETAIL = 4.0
# Block difference allowed (DXT compression of the two sizes differs): on average, and in
# the worst 5% of blocks (a different emblem or patch).
MAX_LOD_DIFFERENCE = 3.0
MAX_DIFFERENCE = 1.5
MAX_LOD_LOCAL_DIFFERENCE = 12.0
MAX_LOCAL_DIFFERENCE = 6.0
# Cross-name pairs need the smaller to have at least this many blocks across.
MIN_BLOCKS = 16
# Size ratios looked for: up to 16 times.
MAX_SHIFT = 4
# An LOD: the name with l2 (half size) or l3 (quarter size) added.
LOD = re.compile(r'^(.+)l([23])$')
NAME = re.compile(r'^[a-z0-9_.-]+$')
FIXED = re.compile(r'_fixed_')


def archives(game):
    """The archives, sorted by path (as texture_pack.cpp's ListArchives)."""
    found = []
    for root in (os.path.join(game, 'packages', 'core', 'data'), os.path.join(game, 'modules')):
        for folder, _, files in os.walk(root):
            found += [os.path.join(folder, name) for name in files if name.lower().endswith(('.erf', '.rim'))]
    return sorted(found)


def tiled_offsets(bw, bh, bpp_log2):
    # XGAddress2DTiledOffset (Xenia's texture_address::Tiled2D), per block.
    y, x = np.mgrid[0:bh, 0:bw].astype(np.int64)
    aligned = (bw + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def rgb565(v):
    return np.stack([(v >> 11 & 31) * (255 / 31), (v >> 5 & 63) * (255 / 63), (v & 31) * (255 / 31)], -1)


def bc4_means(blocks, k):
    """The mean value of the BC4 (DXT5 alpha, DXN channel) blocks at byte k of each block."""
    a0 = blocks[:, k].astype(np.float64)
    a1 = blocks[:, k + 1].astype(np.float64)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, k + 2 + i].astype(np.uint64) << np.uint64(8 * i)
    index = np.stack([(bits >> np.uint64(3 * j)) & np.uint64(7) for j in range(16)], -1).astype(np.int64)
    # Each index's weight of a0 and fixed part: 8 values when a0 > a1, else 6 and 0, 255.
    eight = np.array([1, 0, 6 / 7, 5 / 7, 4 / 7, 3 / 7, 2 / 7, 1 / 7])
    six = np.array([1, 0, 4 / 5, 3 / 5, 2 / 5, 1 / 5, 0, 0])
    six_fixed = np.array([0, 0, 0, 0, 0, 0, 0, 255.0])
    weight = np.where((a0 > a1)[:, None], eight[index], six[index])
    fixed = np.where((a0 > a1)[:, None], 0.0, six_fixed[index])
    # (In the 6-value mode, indices 6 and 7 are 0 and 255, not a mix of a0 and a1.)
    mix = np.where((a0 > a1)[:, None] | (index < 6), 1.0, 0.0)
    values = mix * (weight * a0[:, None] + (1 - weight) * a1[:, None]) + fixed
    return values.mean(-1)


def block_colours(blocks, fmt):
    """Each block's mean colour and alpha; `blocks` is (n, block bytes), little-endian."""
    if fmt == DXN:
        return np.stack([bc4_means(blocks, 0), bc4_means(blocks, 8), np.full(len(blocks), 128.0),
                         np.full(len(blocks), 255.0)], -1)
    colour = blocks[:, 8:] if fmt in (19, 20) else blocks
    c0 = colour[:, 0].astype(np.int64) | colour[:, 1].astype(np.int64) << 8
    c1 = colour[:, 2].astype(np.int64) | colour[:, 3].astype(np.int64) << 8
    bits = (colour[:, 4].astype(np.int64) | colour[:, 5].astype(np.int64) << 8 |
            colour[:, 6].astype(np.int64) << 16 | colour[:, 7].astype(np.int64) << 24)
    index = np.stack([(bits >> (2 * j)) & 3 for j in range(16)], -1)
    four = (c0 > c1)[:, None]
    weight = np.where(four, np.array([1, 0, 2 / 3, 1 / 3])[index], np.array([1, 0, 0.5, 0])[index])
    # DXT1's 3-colour mode: index 3 is black, and transparent.
    opaque = np.where(four | (index < 3), 1.0, 0.0)
    rgb = (opaque[..., None] * (weight[..., None] * rgb565(c0)[:, None] +
                                (1 - weight[..., None]) * rgb565(c1)[:, None])).mean(1)
    if fmt == 18:
        alpha = opaque.mean(-1) * 255
    elif fmt == 19:
        nibbles = np.stack([(blocks[:, j // 2] >> (4 * (j % 2))) & 15 for j in range(16)], -1)
        alpha = nibbles.mean(-1) * (255 / 15)
    else:
        alpha = bc4_means(blocks, 0)
    return np.concatenate([rgb, alpha[:, None]], -1)


def read_textures(game):
    """name -> (format, width, height, block colours), the first of each name by path."""
    textures = {}
    for path in archives(game):
        with open(path, 'rb') as f:
            head = f.read(0x20)
            if head[:8] != b'ERF V2.1':
                continue
            count = struct.unpack_from('<I', head, 0x10)[0]
            table = f.read(count * 44)
            for i in range(count):
                entry = table[i * 44:(i + 1) * 44]
                name = entry[:32].split(b'\0', 1)[0].decode('ascii', 'replace').lower()
                if not name.endswith('.xds') or name[:-4] in textures:
                    continue
                if not NAME.match(name):
                    sys.exit(f'{name} in {path}: not a name the table can hold')
                offset, packed, size = struct.unpack_from('<III', entry, 32)
                if size <= XDS_HEADER_BYTES:
                    continue
                f.seek(offset)
                raw = f.read(packed)
                data = zlib.decompress(raw) if packed != size else raw
                header = len(data) - XDS_HEADER_BYTES
                if struct.unpack_from('>I', data, header)[0] != XDS_COMMON_TEXTURE:
                    continue
                dword1, dword2 = struct.unpack_from('>II', data, header + XDS_FETCH_OFFSET + 4)
                fmt = dword1 & 0x3F
                width, height = (dword2 & 0x1FFF) + 1, (dword2 >> 13 & 0x1FFF) + 1
                if fmt not in FORMATS or width % 4 or height % 4:
                    continue
                bw, bh = width // 4, height // 4
                # Small textures' base level shares a packed mip tail: left out.
                if bw < 8 or bh < 8:
                    continue
                block_bytes = FORMATS[fmt]
                offsets = tiled_offsets(bw, bh, 3 if block_bytes == 8 else 4).ravel()
                if offsets.max() + block_bytes > header:
                    continue
                blocks = np.frombuffer(data, np.uint8)[offsets[:, None] + np.arange(block_bytes)].copy()
                # 8-in-16 byte order.
                blocks[:, 0::2], blocks[:, 1::2] = blocks[:, 1::2].copy(), blocks[:, 0::2].copy()
                textures[name[:-4]] = (fmt, width, height, block_colours(blocks, fmt).reshape(bh, bw, 4))
    return textures


def to_image(colours, size):
    image = Image.fromarray(np.clip(colours, 0, 255).astype(np.uint8), 'RGBA')
    return np.asarray(image.resize(size, Image.BOX), float)


def shift_between(copy, original):
    """log2(copy size / original size) when that's the same for width and height, or None."""
    (_, cw, ch, _), (_, ow, oh, _) = copy, original
    for shift in range(-MAX_SHIFT, MAX_SHIFT + 1):
        scale = 2.0 ** shift
        if cw == ow * scale and ch == oh * scale:
            return shift
    return None


def difference(a, b):
    """The mean and 95th percentile block difference at the smaller's grid."""
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    blocks = np.abs(to_image(a, (w, h)) - to_image(b, (w, h))).mean(-1)
    return float(blocks.mean()), float(np.percentile(blocks, 95))


def map_kind(name):
    match = LOD.match(name)
    return (match.group(1) if match else name)[-1:]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    textures = {name: t for name, t in read_textures(sys.argv[1]).items() if not FIXED.search(name)}
    print(f'{len(textures)} textures read')
    pairs = {}

    def check(copy, original, limit, local_limit, shift_wanted=None):
        if (copy, original) in pairs:
            return
        a, b = textures[copy], textures[original]
        if (a[0] == DXN) != (b[0] == DXN) or map_kind(copy) != map_kind(original):
            return
        shift = shift_between(a, b)
        if shift is None or (shift_wanted is not None and shift != shift_wanted):
            return
        mean, local = difference(a[3], b[3])
        if mean <= limit and local <= local_limit:
            pairs[(copy, original)] = (shift, mean)

    # LODs of the same name, at their size (l2 half, l3 a quarter).
    for name in textures:
        match = LOD.match(name)
        if match and match.group(1) in textures:
            check(name, match.group(1), MAX_LOD_DIFFERENCE, MAX_LOD_LOCAL_DIFFERENCE, 1 - int(match.group(2)))
    lods = len(pairs)

    # Other names: thumbnails first.
    names = sorted(textures)
    thumbs, detail = [], []
    for name in names:
        t = to_image(textures[name][3], (THUMB, THUMB))
        v = (t - t.mean(axis=(0, 1))).ravel()
        n = np.linalg.norm(v)
        thumbs.append(v / n if n else v)
        detail.append(t[..., :3].std())
    thumbs = np.stack(thumbs)
    detail = np.array(detail)
    for start in range(0, len(names), 512):
        sim = thumbs[start:start + 512] @ thumbs.T
        for i, j in zip(*np.nonzero(sim >= MIN_SIMILARITY)):
            a, b = names[start + i], names[j]
            if a == b or detail[start + i] < MIN_DETAIL or detail[j] < MIN_DETAIL:
                continue
            ta, tb = textures[a], textures[b]
            if min(ta[3].shape[1], tb[3].shape[1], ta[3].shape[0], tb[3].shape[0]) < MIN_BLOCKS:
                continue
            check(a, b, MAX_DIFFERENCE, MAX_LOCAL_DIFFERENCE)
    print(f'{lods} LODs of the same name, {len(pairs) - lods} other pairs')

    lines = [
        '// Generated by scripts/find_texture_copies.py from the game\'s own textures: textures',
        '// that are a copy of another\'s picture (the same size, smaller or larger: the smaller',
        '// ones distant models use, terrain copies of props\', and the like), as { copy, original,',
        '// log2(copy size / original size) }, sorted by copy, the closest original first.',
        '// Don\'t edit: run the script again.',
    ]
    for (copy, original), (shift, _) in sorted(pairs.items(), key=lambda p: (p[0][0], p[1][1], p[0][1])):
        lines.append(f'{{ "{copy}", "{original}", {shift} }},')
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{len(pairs)} pairs in {os.path.relpath(OUT, ROOT)}')


if __name__ == '__main__':
    main()
