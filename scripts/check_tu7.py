"""Checks that Title Update 7 still runs on Title Update 6's build (docs/porting-playbook.md,
"Later title updates"): their code differs only in the six known instructions, no hook
or address in DAORecomp/ lands in a function holding one (but the version number's
hook), and the jump tables found in 7's image are the ones in
config/DAO_tu6_switch_tables.toml. Run it after any hook, address or switch-table change.
The image sizes (PPC_IMAGE_SIZE) differ and aren't checked: the loader takes either.

Needs analysis/tu6 and analysis/tu7 (tools/xexdump dumps of the patched images).

    python -I scripts/check_tu7.py
"""
import bisect
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for variant in ('tu6', 'tu7'):
    if not os.path.isfile(os.path.join(ROOT, 'analysis', variant, 'image.bin')):
        sys.exit(f'analysis/{variant}/image.bin missing: dump the patched image with '
                 f'tools/xexdump (docs/porting-playbook.md, "Later title updates")')
os.environ['DAO_VARIANT'] = 'tu6'
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import ppc  # noqa: E402

# TU6 address -> (TU6 word, TU7 word): a .data global four bytes lower in 7, and the
# version number the constructor at 82620A10 stores.
KNOWN = {
    0x82256820: (0x916A9A60, 0x916A9A5C),
    0x82257D74: (0x816A9A60, 0x816A9A5C),
    0x82257D7C: (0x916A9A60, 0x916A9A5C),
    0x82257DBC: (0x816A9A60, 0x816A9A5C),
    0x82257DC4: (0x916A9A60, 0x916A9A5C),
    0x82620A44: (0x3940304D, 0x394032FF),
}
# The one address in differing code DAORecomp may name: the version number's
# constructor, hooked by kernel/title_update_version.cpp to store 7's.
VERSION_HOOK = 0x82620A10
# The moved global: 83299A60 in 6, 83299A5C in 7 (both, four bytes each).
MOVED_GLOBAL_RANGE = (0x83299A5C, 0x83299A64)


def main():
    errors = []
    tu6 = ppc.IMG
    with open(os.path.join(ROOT, 'analysis', 'tu7', 'image.bin'), 'rb') as f:
        tu7 = f.read()
    lo, hi = ppc.TEXT
    diffs = {}
    for a in range(lo, hi, 4):
        o = a - ppc.BASE
        if tu6[o:o + 4] != tu7[o:o + 4]:
            diffs[a] = (int.from_bytes(tu6[o:o + 4], 'big'), int.from_bytes(tu7[o:o + 4], 'big'))
    if diffs != KNOWN:
        for a in sorted(set(diffs) | set(KNOWN)):
            if diffs.get(a) != KNOWN.get(a):
                errors.append(f'.text {a:08X}: TU6/TU7 words {diffs.get(a)} (expected {KNOWN.get(a)})')

    # Code that differs: each difference's function, or for a frameless leaf with no
    # .pdata, the code around it.
    funcs = sorted(ppc.pdata())
    starts = [s for s, _ in funcs]
    differing = set()
    for a in diffs:
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and funcs[i][0] <= a < funcs[i][0] + funcs[i][1]:
            differing.add((funcs[i][0], funcs[i][0] + funcs[i][1]))
        else:
            differing.add((a - 0x400, a + 0x400))
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'DAORecomp')):
        for name in files:
            if not name.endswith(('.cpp', '.h')):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding='utf-8', errors='replace') as f:
                for n, line in enumerate(f, 1):
                    for m in re.finditer(r'(?:0[xX]|sub_)(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])', line):
                        a = int(m.group(1), 16)
                        if a == VERSION_HOOK:
                            continue
                        if (any(s <= a < e for s, e in differing)
                                or MOVED_GLOBAL_RANGE[0] <= a < MOVED_GLOBAL_RANGE[1]):
                            errors.append(f'{os.path.relpath(path, ROOT)}:{n}: {a:08X} is in code or data '
                                          'that differs in Title Update 7')

    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'tu7_switch_tables.toml')
        env = dict(os.environ, DAO_VARIANT='tu7')
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'scripts', 'find_switches.py'), out],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode:
            errors.append(f'find_switches.py with DAO_VARIANT=tu7 failed: {r.stderr.strip()}')
        else:
            with open(out, encoding='utf-8') as f:
                tu7_tables = f.read()
            with open(os.path.join(ROOT, 'config', 'DAO_tu6_switch_tables.toml'), encoding='utf-8') as f:
                if f.read() != tu7_tables:
                    errors.append('find_switches.py with DAO_VARIANT=tu7 differs from '
                                  'config/DAO_tu6_switch_tables.toml')

    for e in errors:
        print(e)
    print('Title Update 7 runs on 6\'s build' if not errors else f'{len(errors)} problems')
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
