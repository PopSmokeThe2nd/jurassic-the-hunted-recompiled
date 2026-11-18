"""Name the C++ class of a vtable (MSVC RTTI), or list vtables for a class name.

usage: python -I tools/vtable_name.py build/default.bin 0x82009C8C [0x...]
       python -I tools/vtable_name.py build/default.bin --class ActorProperties
"""
import struct, sys

BASE = 0x82000000


def main():
    img = open(sys.argv[1], "rb").read()

    def u32(a):
        return struct.unpack_from(">I", img, a - BASE)[0]

    def name_of(vt):
        col = u32(vt - 4)
        if not (BASE <= col < BASE + len(img)):
            return None
        td = u32(col + 12)
        if not (BASE <= td < BASE + len(img)):
            return None
        raw = img[td - BASE + 8: td - BASE + 120].split(b"\0")[0]
        return raw.decode("ascii", "replace")

    if sys.argv[2] == "--class":
        want = f".?AV{sys.argv[3]}@@".encode()
        pos = img.find(want + b"\0")
        td = BASE + pos - 8
        b = struct.pack(">I", td)
        i = img.find(b)
        while i >= 0:
            col = BASE + i - 12
            if u32(col) == 0:
                cb = struct.pack(">I", col)
                j = img.find(cb)
                while j >= 0:
                    print(f"{sys.argv[3]}: vtable {BASE + j + 4:#x} (offset {u32(col + 4)})")
                    j = img.find(cb, j + 1)
            i = img.find(b, i + 1)
        return
    for arg in sys.argv[2:]:
        vt = int(arg, 16)
        print(f"{vt:#010x}: {name_of(vt)}")


if __name__ == "__main__":
    main()
