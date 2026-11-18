"""Find code that references an address (lis + addi/ori/lwz/lfs/stw/... pairs) in the image.

usage: python -I tools/xref.py build/default.bin <addr|"string"> [...]
  A quoted argument that isn't hex is searched for as a NUL-terminated ASCII string first.
Prints each referencing instruction address and its containing function (from the generated
function table, if generated/default/jurassichunted_init.cpp exists).
"""
import bisect, os, re, struct, sys

BASE = 0x82000000
D_FORM = {14: "addi", 24: "ori", 32: "lwz", 34: "lbz", 40: "lhz", 48: "lfs", 50: "lfd",
          36: "stw", 38: "stb", 44: "sth", 52: "stfs", 54: "stfd", 58: "ld"}


def main():
    img = open(sys.argv[1], "rb").read()
    init = os.path.join(os.path.dirname(sys.argv[1]), "..", "generated", "default", "jurassichunted_init.cpp")
    starts = []
    if os.path.exists(init):
        starts = sorted(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", open(init).read()))

    targets = []
    for arg in sys.argv[2:]:
        if re.fullmatch(r"(0x)?[0-9A-Fa-f]{8}", arg):
            targets.append((arg, int(arg, 16)))
        else:
            needle = arg.encode() + b"\0"
            pos = img.find(needle)
            while pos > 0 and img[pos - 1] != 0:  # require start of string
                pos = img.find(needle, pos + 1)
            if pos < 0:
                print(f"{arg!r}: not found")
                continue
            targets.append((arg, BASE + pos))

    text_lo, text_hi = 0x100000, 0x706CC8
    for name, t in targets:
        print(f"== {name} @ {t:#010x}")
        for off in range(text_lo, text_hi, 4):
            w = struct.unpack_from(">I", img, off)[0]
            op = w >> 26
            if op not in D_FORM:
                continue
            ra = (w >> 16) & 31
            if ra == 0:
                continue
            d = w & 0xFFFF
            if op == 24:  # ori: unsigned
                lo_part = d
            else:
                lo_part = d - 0x10000 if d & 0x8000 else d
            for k in range(1, 33):
                p = struct.unpack_from(">I", img, off - 4 * k)[0]
                if p >> 26 == 15 and (p >> 16) & 31 == 0 and (p >> 21) & 31 == ra:
                    v = (((p & 0xFFFF) << 16) + lo_part) & 0xFFFFFFFF
                    if v == t:
                        ref = BASE + off
                        fn = ""
                        if starts:
                            i = bisect.bisect_right(starts, ref) - 1
                            fn = f" in sub_{starts[i]:08X}" if i >= 0 else ""
                        print(f"  {ref:#010x} {D_FORM[op]}{fn}")
                    break


if __name__ == "__main__":
    main()
