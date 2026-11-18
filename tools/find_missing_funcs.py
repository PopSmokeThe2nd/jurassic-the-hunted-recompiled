"""Find code pointers in data sections that don't start a recompiled function.

The recompiler's analysis can merge two adjacent functions when nothing separates them
(e.g. back-to-back vtable thunks ending in `bctr`). At runtime an indirect call to the second
one then fails with "Call to invalid or unregistered function". This scans the decrypted image
(tools/xex_extract.ps1 output) for 32-bit big-endian values in non-code sections that point
into code, and reports those that aren't registered function starts but look like one
(the previous instruction ends a function, or is zero padding).

usage: python -I tools/find_missing_funcs.py build/default.bin generated/default/jurassichunted_init.cpp [--toml]
"""
import bisect, re, struct, sys

BASE = 0x82000000


def sections(img):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    assert img[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    optsz = struct.unpack_from("<H", img, pe + 20)[0]
    off = pe + 24 + optsz
    out = []
    for i in range(nsec):
        name = img[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", img, off + 8)
        chars = struct.unpack_from("<I", img, off + 36)[0]
        out.append((name, va, max(vsize, rawsize), chars))
        off += 40
    return out


def is_terminator(insn):
    if insn in (0x4E800020, 0x4E800420, 0x00000000):  # blr, bctr, padding
        return True
    if (insn >> 26) == 18 and (insn & 3) == 0:  # b target (unconditional, no link)
        return True
    return False


def main():
    img = open(sys.argv[1], "rb").read()
    # Every registered entry: sub_XXXXXXXX, named overrides, and __imp__ kernel import thunks.
    starts = sorted(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", open(sys.argv[2]).read()))
    start_set = set(starts)
    want_toml = "--toml" in sys.argv
    # Addresses already judged to be false positives: config lines like "# (0x82220804 rejected: ..."
    rejected = set()
    if "--config" in sys.argv:
        cfg_text = open(sys.argv[sys.argv.index("--config") + 1]).read()
        rejected = {int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8}) rejected", cfg_text)}

    secs = sections(img)
    code = [(va, va + sz) for n, va, sz, ch in secs if ch & 0x20000020]
    data = [(n, va, va + sz) for n, va, sz, ch in secs if not ch & 0x20000020]
    lo = BASE + min(a for a, _ in code)
    hi = BASE + max(b for _, b in code)

    def in_code(addr):
        rva = addr - BASE
        return any(a <= rva < b for a, b in code)

    def u32(addr):
        return struct.unpack_from(">I", img, addr - BASE)[0]

    # C-specific-handler (SEH) scope tables: a count word followed by that many
    # {BeginAddress, EndAddress, HandlerAddress, JumpTarget} records. Begin/End/JumpTarget
    # legitimately point inside their function, so every word in these tables is excluded.
    def code_ptr(x):
        return lo <= x < hi and not x & 3

    seh_words = set()
    for name, a, b in data:
        if name != ".rdata":
            continue
        for p in range(BASE + a, BASE + min(b, len(img)) - 20, 4):
            c = u32(p)
            if not 1 <= c <= 64 or p + 4 + 16 * c > BASE + b:
                continue
            ok = True
            for k in range(c):
                beg, end, hnd, tgt = struct.unpack_from(">4I", img, p + 4 + 16 * k - BASE)
                if not (code_ptr(beg) and code_ptr(end) and beg < end and end - beg < 0x40000
                        and code_ptr(hnd) and (tgt == 0 or code_ptr(tgt))):
                    ok = False
                    break
            if ok:
                seh_words.update(range(p, p + 4 + 16 * c, 4))

    found = {}
    for name, a, b in data:
        if name == ".pdata":  # compiler unwind records; the analyzer already consumes these
            continue
        for off in range(a, min(b, len(img)) - 3, 4):
            v = struct.unpack_from(">I", img, off)[0]
            if v & 3 or not (lo <= v < hi) or v in start_set or not in_code(v):
                continue
            if not is_terminator(u32(v - 4)):
                continue
            if BASE + off in seh_words:
                continue
            i = bisect.bisect_right(starts, v) - 1
            parent = starts[i] if i >= 0 else 0
            found.setdefault(v, (parent, []))[1].append(BASE + off)

    # Addresses materialized in code with `lis rA,hi ; addi rD,rA,lo` (callbacks passed to
    # registration functions, e.g. Lua C functions). Skip references from inside the target's own
    # function: those are usually switch-table bases or local labels.
    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else 0

    for a, b in code:
        for off in range(a, min(b, len(img)), 4):
            w = struct.unpack_from(">I", img, off)[0]
            if w >> 26 != 14:  # addi
                continue
            ra = (w >> 16) & 31
            if ra == 0:
                continue
            simm = w & 0xFFFF
            simm = simm - 0x10000 if simm & 0x8000 else simm
            for k in range(1, 33):
                if off - 4 * k < a:
                    break
                p = struct.unpack_from(">I", img, off - 4 * k)[0]
                if p >> 26 == 15 and (p >> 16) & 31 == 0 and (p >> 21) & 31 == ra:  # lis ra,hi
                    v = (((p & 0xFFFF) << 16) + simm) & 0xFFFFFFFF
                    break
            else:
                continue
            if v & 3 or not (lo <= v < hi) or v in start_set or not in_code(v):
                continue
            if not is_terminator(u32(v - 4)):
                continue
            ref = BASE + off
            if func_of(ref) == func_of(v):
                continue
            found.setdefault(v, (func_of(v), []))[1].append(ref)

    for v in list(found):
        if v in rejected or u32(v) == 0:  # rejected, or zero padding rather than code
            del found[v]

    print(f"# code {lo:#x}-{hi:#x}; {len(starts)} registered functions; "
          f"{len(found)} candidate missing function starts", file=sys.stderr)
    boundaries = sorted(start_set | set(found))
    for v in sorted(found):
        parent, refs = found[v]
        j = bisect.bisect_right(boundaries, v)
        nxt = boundaries[j] if j < len(boundaries) else hi
        if want_toml:
            print(f"0x{v:08X} = {{ size = 0x{nxt - v:X} }}  # inside sub_{parent:08X}; "
                  f"{len(refs)} data ref(s), first at 0x{refs[0]:08X}")
        else:
            print(f"0x{v:08X}  in sub_{parent:08X} (next 0x{nxt:08X})  refs={len(refs)} first@0x{refs[0]:08X}")


if __name__ == "__main__":
    main()
