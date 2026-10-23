"""Find jump tables in the DAO image by concretely executing each bctr's setup code.

For every `bctr`, search backwards for a `cmplwi crN, rI, imm` bound check, then
walk forward from it (taking unconditional branches, falling through conditional
ones) to the bctr. The walk is replayed once per case index with rI = index; any
register not derived from constants/rI/table loads is unknown, which rejects the
site. Output is XenonRecomp's switch-table TOML, keyed by the bctr address.

usage: find_switches.py <out.toml> [report.txt]
"""
import sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from ppc import BASE, IMG, TEXT, BINK, VARIANT, u32, code_words, branch_target, pdata

MASK = 0xFFFFFFFF
# How far from a bctr in a function with no .pdata its setup and cases are looked for.
NO_PDATA_REACH = 0x400


class Unknown(Exception):
    pass


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def rotl32(v, n):
    v &= MASK
    return ((v << n) | (v >> (32 - n))) & MASK if n else v


def mask32(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) % 32
    return m


def load(addr, size):
    off = (addr & MASK) - BASE
    if not (0 <= off < len(IMG) - size):
        raise Unknown(f'load outside image {addr:#x}')
    return int.from_bytes(IMG[off:off + size], 'big')


def step(a, w, regs):
    """Execute one instruction on regs (dict reg->int). Returns next pc or None to fall through."""
    op = w >> 26
    rd = (w >> 21) & 31
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    imm = w & 0xFFFF

    # Unknown values propagate as None; only the final ctr must be known.
    def get(r):
        return regs.get(r)

    def get0(r):
        return 0 if r == 0 else get(r)

    class _N:
        """Arithmetic helper: any None operand yields None."""
        @staticmethod
        def f(fn, *xs):
            return None if any(x is None for x in xs) else fn(*xs) & MASK

    if op == 14:    # addi
        regs[rd] = _N.f(lambda x: x + sext16(imm), get0(ra))
    elif op == 15:  # addis
        regs[rd] = _N.f(lambda x: x + (sext16(imm) << 16), get0(ra))
    elif op == 24:  # ori (nop is ori 0,0,0)
        if w != 0x60000000:
            regs[ra] = _N.f(lambda x: x | imm, get(rd))
    elif op == 25:  # oris
        regs[ra] = _N.f(lambda x: x | (imm << 16), get(rd))
    elif op == 21:  # rlwinm
        sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
        regs[ra] = _N.f(lambda x: rotl32(x, sh) & mask32(mb, me), get(rd))
    elif op in (32, 40, 34, 42):  # lwz lhz lbz lha
        size = {32: 4, 40: 2, 34: 1, 42: 2}[op]
        base_ = get0(ra)
        v = None if base_ is None else load(base_ + sext16(imm), size)
        if op == 42 and v is not None:
            v = sext16(v) & MASK
        regs[rd] = v
    elif op == 30:  # rldicl/rldicr etc: only handle "clrldi rA, rS, 32" (zero-extend)
        xo = (w >> 2) & 7
        sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
        mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        if xo == 0 and sh == 0 and mb == 32:
            regs[ra] = _N.f(lambda x: x, get(rd))
        else:
            regs[ra] = None
    elif op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 266:    # add
            regs[rd] = _N.f(lambda x, y: x + y, get(ra), get(rb))
        elif xo == 40:   # subf
            regs[rd] = _N.f(lambda x, y: y - x, get(ra), get(rb))
        elif xo == 444:  # or / mr
            regs[ra] = _N.f(lambda x, y: x | y, get(rd), get(rb))
        elif xo in (23, 279, 87, 343):  # lwzx lhzx lbzx lhax
            size = {23: 4, 279: 2, 87: 1, 343: 2}[xo]
            ea = _N.f(lambda x, y: x + y, get0(ra), get(rb))
            v = None if ea is None else load(ea, size)
            if xo == 343 and v is not None:
                v = sext16(v) & MASK
            regs[rd] = v
        elif xo == 24:   # slw
            regs[ra] = _N.f(lambda x, y: (x << (y & 63)) if (y & 63) < 32 else 0, get(rd), get(rb))
        elif xo == 986:  # extsw
            regs[ra] = get(rd)
        elif xo == 467 and ((w >> 11) & 0x3FF) == 0x120:  # mtctr
            regs['ctr'] = get(rd)
        elif xo in (0, 32):  # cmp/cmpl: no GPR write
            pass
        elif xo in (151, 407, 215, 150, 183, 247, 662, 918):  # stores
            pass
        else:
            # Unknown X-form: assume it writes rD/rA (whichever is the target)
            regs[rd] = None
            regs[ra] = None
    elif op == 18:  # b
        if w & 1:
            raise Unknown('bl inside switch setup')
        return branch_target(a, w)
    elif op == 16:  # bc: fall through
        pass
    elif op in (10, 11):  # cmpli/cmpi
        pass
    elif op in (36, 37, 38, 44, 45, 39, 52, 54, 62):  # stores
        pass
    elif op == 58:  # ld family
        regs[rd] = None
    else:
        regs[rd] = None
        regs[ra] = None
    return None


# D-form load that produced the compared index (op, D, rA); a later identical
# load (debug-style re-read of the same variable) yields the index again.
ALIAS = None


COPIES = ()


def run(start, bctr, idx_reg, idx, take):
    """Execute from start to bctr with idx_reg = idx. Conditional branches to an
    address in `take` are taken, all others fall through. Returns (ctr, regs)."""
    regs = {idx_reg: idx}
    for r in COPIES:
        regs[r] = idx
    pc = start
    for _ in range(128):
        if pc == bctr:
            if regs.get('ctr') is None:
                raise Unknown('ctr unknown')
            return regs['ctr'], regs
        w = u32(pc)
        op = w >> 26
        if ALIAS and op in (32, 40, 34, 42) and (op, w & 0xFFFF, (w >> 16) & 31) == ALIAS:
            regs[(w >> 21) & 31] = idx
            pc += 4
            continue
        if op == 16 and branch_target_bc(pc, w) in take:
            pc = branch_target_bc(pc, w)
            continue
        nxt = step(pc, w, regs)
        pc = nxt if nxt is not None else pc + 4
    raise Unknown('did not reach bctr')


def is_terminator(w):
    """Unconditional b (no link), blr, bctr."""
    return (w >> 26 == 18 and not (w & 1)) or w in (0x4E800020, 0x4E800420)


def is_bclr(w):
    return w >> 26 == 19 and ((w >> 1) & 0x3FF) == 16 and w != 0x4E800020


def block_start(bctr, lo):
    a = bctr
    while a - 4 >= lo and not is_terminator(u32(a - 4)):
        a -= 4
        if bctr - a > 0x200:
            break
    return a


def bound_from_cmp(cmp_at, bc_at):
    """(ri, count, default, defining-load-alias) for `cmplwi crN, rI, imm` + bc on crN GT."""
    w = u32(cmp_at)
    bw = u32(bc_at)
    crf = (w >> 23) & 7
    ri = (w >> 16) & 31
    n = w & 0xFFFF
    bo, bi = (bw >> 21) & 31, (bw >> 16) & 31
    if bi // 4 != crf or bi % 4 != 1:
        return None
    if bw >> 26 == 19:  # bgtlr: default is a return
        if not bo & 8:
            return None
        default = 0
    elif bo & 8:    # bgt -> target is default
        default = branch_target_bc(bc_at, bw)
    else:           # ble -> target is the table, fallthrough is default
        default = bc_at + 4
    # defining load of rI just before the compare (for debug-style reloads)
    alias = None
    for k in range(1, 6):
        dw = u32(cmp_at - 4 * k)
        if dw >> 26 in (32, 40, 34, 42) and (dw >> 21) & 31 == ri:
            alias = (dw >> 26, dw & 0xFFFF, (dw >> 16) & 31)
            break
    # registers copied to/from rI by `mr` just before the compare
    global COPIES
    copies = []
    for k in range(1, 6):
        dw = u32(cmp_at - 4 * k)
        if dw >> 26 == 31 and ((dw >> 1) & 0x3FF) == 444 and ((dw >> 21) & 31) == ((dw >> 11) & 31):
            rs, ra_ = (dw >> 21) & 31, (dw >> 16) & 31
            if rs == ri:
                copies.append(ra_)
            elif ra_ == ri:
                copies.append(rs)
    COPIES = tuple(copies)
    return ri, n + 1, default, alias


def find_cmp_before(bc_at, lo):
    """cmpli (L=0) on the bc's CR field within 8 instructions before it."""
    bw = u32(bc_at)
    crf = ((bw >> 16) & 31) // 4
    for k in range(1, 9):
        a = bc_at - 4 * k
        if a < lo:
            break
        w = u32(a)
        if w >> 26 == 10 and ((w >> 21) & 1) == 0 and ((w >> 23) & 7) == crf:
            return a
        if is_terminator(w) or (w >> 26 == 18):
            break
    return None


def candidates(bctr, fn):
    """Yield (cmp_at, bc_at, take) guarding the setup block of this bctr."""
    lo = fn[0] if fn else bctr - NO_PDATA_REACH
    hi = fn[0] + fn[1] if fn else bctr
    S = block_start(bctr, lo)
    # (a) cmplwi + bgt inside the setup block itself
    for a in range(bctr - 4, S - 4, -4):
        w = u32(a)
        if w >> 26 == 16 or is_bclr(w):
            c = find_cmp_before(a, S)
            if c is not None:
                yield c, a, {S}
                break
    # (b) conditional branches elsewhere in the function that jump to the block
    for a in range(lo, hi, 4):
        w = u32(a)
        if w >> 26 == 16 and branch_target_bc(a, w) == S:
            c = find_cmp_before(a, lo)
            if c is not None:
                yield c, a, {S}
    # (c) block reached by fallthrough from a bc just above it
    if S - 4 >= lo and (u32(S - 4) >> 26 == 16 or is_bclr(u32(S - 4))):
        c = find_cmp_before(S - 4, lo)
        if c is not None:
            yield c, S - 4, set()


def unbounded_table(bctr, fb):
    """(ri, labels) for a bctr whose setup block indexes a table with a register it
    never bounds (the caller passes a valid index). Cases run from 0 while the
    target stays inside the function (for a frameless leaf with no .pdata, fb None:
    within NO_PDATA_REACH of the bctr), so the count is only a guess: entries past
    the real end often still land inside it. None if the ctr doesn't come from a
    table read with one register."""
    global COPIES
    if not fb:
        fb = (bctr - NO_PDATA_REACH, 2 * NO_PDATA_REACH)
    COPIES = ()
    # The setup is straight-line code after the block's last branch or call; walk
    # from the latest start that still computes the ctr.
    lo = block_start(bctr, fb[0])
    table_read = False
    for S in range(bctr - 4, lo - 4, -4):
        w = u32(S)
        if w >> 26 in (16, 18) or is_bclr(w):
            break
        # lwzx lhzx lbzx lhax: without one the ctr is computed, not read
        table_read |= w >> 26 == 31 and (w >> 1) & 0x3FF in (23, 279, 87, 343)
        if not table_read:
            continue
        try:
            run(S, bctr, 'none', 0, set())
            return None  # ctr is constant without any index: not a table
        except Unknown:
            pass
        for ri in range(32):
            labels = []
            for i in range(1024):
                try:
                    ctr, rg = run(S, bctr, ri, i, set())
                except Unknown:
                    break
                if rg.get(ri) != i or ctr & 3 or not fb[0] <= ctr < fb[0] + fb[1]:
                    break
                labels.append(ctr)
            if labels:
                return ri, labels
    return None


# Switches without a bounds check, checked by hand: bctr -> case count, per image
# (DAO_VARIANT; 'tu7' shares 'tu6''s, below). Every other one found is only reported
# (UNBOUNDED lines).
UNBOUNDED = {
    # Disc: the same four as TU6's below (82A6F958 and 82A6FA00 = 82B39960 and
    # 82B39A08, 82E22DC4 = 82EEC3C4, 82CB3214 = 82D7D1EC).
    '': {0x82A6F958: 11, 0x82A6FA00: 11, 0x82CB3214: 7, 0x82E22DC4: 16},
    'tu6': {
        # tu6_sub_82D7D148, a frameless leaf with no .pdata (its bounds are in
        # DAO_tu6.toml's AUTO-FUNCTIONS): every path sets r11 to 0-6 with li before
        # the jump, through 7 addresses stored right after the bctr. Entering Aeducan
        # Thaig hit case 6 as an unrecompiled call (issue #5).
        0x82D7D1EC: 7,
        # tu6_sub_823D7748: u16 table at 82169A60, entry 14 is the next table's
        # (outside the function); the callers pass constants 0-9.
        0x823D7794: 14,
        # A second dispatch on r10 inside cases of the switches at 82B3991C and
        # 82B399C4, which bound it (cmplwi r10,10). The chained-dispatch pass misses
        # them: their block starts after the first switch's inline table.
        0x82B39960: 11,
        0x82B39A08: 11,
        # r29 is a 4-bit field (rlwinm at 82EEC088), bound by cmplwi r29,15 for the
        # switch at 82EEC0B8.
        0x82EEC3C4: 16,
    },
}
# Title Update 7 runs on 6's build (docs/porting-playbook.md, "Later title updates"):
# DAO_VARIANT=tu7 must give the same tables as tu6 (scripts/check_tu7.py checks it).
UNBOUNDED['tu7'] = UNBOUNDED['tu6']


def branch_target_bc(a, w):
    if w >> 26 != 16:
        return 0
    bd = w & 0xFFFC
    if bd & 0x8000:
        bd -= 0x10000
    return (bd if w & 2 else a + bd) & MASK


def main():
    out_path = sys.argv[1]
    report = open(sys.argv[2], 'w') if len(sys.argv) > 2 else None
    funcs = sorted(pdata())
    import bisect
    starts = [s for s, _ in funcs]

    def func_bounds(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and funcs[i][0] <= addr < funcs[i][0] + funcs[i][1]:
            return funcs[i]
        return None

    global ALIAS
    found_tables = {}
    tables, failures, tailcalls = [], [], 0
    for a, w in code_words():
        if w != 0x4E800420:
            continue
        fb = func_bounds(a)
        found, err, any_cand = None, None, False
        for cmp_at, bc_at, take in candidates(a, fb):
            b = bound_from_cmp(cmp_at, bc_at)
            if b is None:
                continue
            any_cand = True
            ri, count, default, ALIAS = b
            try:
                runs = [run(cmp_at + 4, a, ri, i, take) for i in range(count)]
            except Unknown as e:
                err = str(e)
                continue
            # register holding the case index at the bctr (prefer the compared one)
            regs_ok = [r for r in range(32) if all(rg.get(r) == i for i, (_, rg) in enumerate(runs))]
            if not regs_ok:
                err = 'index not live at bctr'
                continue
            r_at = ri if ri in regs_ok else regs_ok[0]
            labels = [c for c, _ in runs]
            found = (r_at, default, labels)
            break
        ALIAS = None
        if found is None:
            if any_cand:
                failures.append((a, err or 'no usable bound'))
            else:
                tailcalls += 1
            continue
        r_at, default, labels = found
        found_tables[a] = (r_at, default, labels, fb)
        outside = [l for l in labels if fb and not (fb[0] <= l < fb[0] + fb[1])]
        tables.append((a, r_at, default, labels, fb, outside))

    # Chained dispatch: a table whose setup block is itself a case label of a
    # known table, dispatching again on the same (still live) index register.
    pending = {a for a, _ in failures} | set()
    unbounded = [a for a, w in code_words() if w == 0x4E800420 and a not in found_tables]
    progress = True
    while progress:
        progress = False
        for a in unbounded:
            if a in found_tables:
                continue
            fb = func_bounds(a)
            S = block_start(a, fb[0] if fb else a - NO_PDATA_REACH)
            for pa, (pr, pdef, plabels, pfb) in list(found_tables.items()):
                reach = [i for i, l in enumerate(plabels) if l == S]
                if not reach or pfb != fb:
                    continue
                labels = [None] * len(plabels)
                try:
                    for i in reach:
                        ctr, rg = run(S, a, pr, i, set())
                        if rg.get(pr) != i:
                            raise Unknown('index clobbered')
                        labels[i] = ctr
                except Unknown:
                    continue
                # indices that never reach this block: point them at a reachable label
                filler = labels[reach[0]]
                for i in range(len(plabels)):
                    if labels[i] is None:
                        try:
                            c, _ = run(S, a, pr, i, set())
                            labels[i] = c if fb and fb[0] <= c < fb[0] + fb[1] else filler
                        except Unknown:
                            labels[i] = filler
                found_tables[a] = (pr, pdef, labels, fb)
                outside = [l for l in labels if fb and not (fb[0] <= l < fb[0] + fb[1])]
                tables.append((a, pr, pdef, labels, fb, outside))
                failures[:] = [f for f in failures if f[0] != a]
                tailcalls -= 1
                progress = True
                break

    # Table reads with no bounds check: emit the hand-checked ones, report the rest.
    accepted = UNBOUNDED.get(VARIANT, {})
    unbounded_found = []
    for a in unbounded:
        if a in found_tables:
            continue
        fb = func_bounds(a)
        u = unbounded_table(a, fb)
        if u is None:
            if a in accepted:
                sys.exit(f'{a:08X}: listed in UNBOUNDED but no table read found')
            continue
        ri, labels = u
        if a not in accepted:
            unbounded_found.append((a, ri, labels, fb))
            continue
        if len(labels) < accepted[a]:
            sys.exit(f'{a:08X}: only {len(labels)} cases inside the function, UNBOUNDED says {accepted[a]}')
        labels = labels[:accepted[a]]
        found_tables[a] = (ri, 0, labels, fb)
        tables.append((a, ri, 0, labels, fb, []))
        if any(f[0] == a for f in failures):
            failures[:] = [f for f in failures if f[0] != a]
        else:
            tailcalls -= 1
    missing = set(accepted) - set(found_tables)
    if missing:
        sys.exit('UNBOUNDED entries not found: ' + ' '.join(f'{a:08X}' for a in sorted(missing)))
    tables.sort()

    with open(out_path, 'w') as f:
        f.write('# Generated by scripts/find_switches.py (concrete execution of switch setup)\n\n')
        for a, ri, default, labels, fb, outside in tables:
            if a in accepted:
                f.write('# No bounds check (UNBOUNDED in find_switches.py): no default case.\n')
            f.write(f'[[switch]]\nbase = 0x{a:X}\nr = {ri}\ndefault = 0x{default:X}\nlabels = [\n')
            for l in labels:
                f.write(f'    0x{l:X},\n')
            f.write(']\n\n')

    print(f'{len(tables)} switch tables, {len(failures)} rejected, {tailcalls} bctr without bound (indirect tail calls)')
    out_of_fn = [t for t in tables if t[5]]
    print(f'{len(out_of_fn)} tables have labels outside their .pdata function')
    print(f'{len(unbounded_found)} table reads without a bounds check not in UNBOUNDED (report only)')
    if report:
        for a, e in failures:
            report.write(f'REJECT {a:08X} {e}\n')
        for a, ri, default, labels, fb, outside in out_of_fn:
            report.write(f'OUTSIDE {a:08X} fn={fb[0]:08X}+{fb[1]:X} n={len(labels)} first_outside={outside[0]:08X}\n')
        for a, ri, labels, fb in unbounded_found:
            fn = f'{fb[0]:08X}+{fb[1]:X}' if fb else 'none'
            report.write(f'UNBOUNDED {a:08X} fn={fn} r{ri} n<={len(labels)} '
                         + ' '.join(f'{l:08X}' for l in labels[:16]) + '\n')


if __name__ == '__main__':
    main()
