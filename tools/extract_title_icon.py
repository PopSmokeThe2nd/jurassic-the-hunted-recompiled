"""Extract the game's dashboard title image (PNG) from default.xex's title resource (XDBF/SPA).

usage: python -I tools/extract_title_icon.py game/default.xex build/default.bin out.png
  default.bin is the decrypted image from tools/xex_extract.ps1 (resources live inside it).
"""
import struct, sys

BASE = 0x82000000
NS_IMAGE, TITLE_IMAGE_ID = 2, 0x8000


def main():
    xex = open(sys.argv[1], "rb").read()
    img = open(sys.argv[2], "rb").read()
    n_opt = struct.unpack_from(">I", xex, 0x14)[0]
    res_off = None
    for i in range(n_opt):
        key, val = struct.unpack_from(">II", xex, 0x18 + 8 * i)
        if key == 0x000002FF:  # RESOURCE_INFO
            res_off = val
    if res_off is None:
        sys.exit("no resource header")
    size = struct.unpack_from(">I", xex, res_off)[0]
    for e in range(res_off + 4, res_off + size, 16):
        name = xex[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        addr, length = struct.unpack_from(">II", xex, e + 8)
        print(f"resource {name!r} at {addr:#x}, {length} bytes")
        data = img[addr - BASE: addr - BASE + length]
        if data[:4] != b"XDBF":
            continue
        _, table_len, count, free_len, _ = struct.unpack_from(">IIIII", data, 4)
        data_base = 24 + table_len * 18 + free_len * 8
        for k in range(count):
            ns, ident, off, ln = struct.unpack_from(">HQII", data, 24 + 18 * k)
            if ns == NS_IMAGE and ident == TITLE_IMAGE_ID:
                png = data[data_base + off: data_base + off + ln]
                if png[:8] != b"\x89PNG\r\n\x1a\n":
                    sys.exit("title image isn't a PNG")
                open(sys.argv[3], "wb").write(png)
                w, h = struct.unpack_from(">II", png, 16)
                print(f"title image: {w}x{h} PNG -> {sys.argv[3]}")
                return
    sys.exit("title image not found")


if __name__ == "__main__":
    main()
