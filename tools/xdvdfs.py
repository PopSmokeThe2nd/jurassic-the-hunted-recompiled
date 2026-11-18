"""Minimal XDVDFS (Xbox 360 XGD2) reader: list and extract files."""
import struct, sys, os

SECTOR = 2048
PARTITIONS = [0x0, 0xFD90000, 0x2080000, 0x18300000]  # XSF, XGD2, XGD1, XGD3
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def find_partition(f):
    for p in PARTITIONS:
        f.seek(p + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return p
    raise SystemExit("XDVDFS volume descriptor not found")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    out = []
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode("ascii", "replace")
        path = prefix + name
        if attr & 0x10:
            if fsize:
                out += walk(f, base, start, fsize, path + "/")
        else:
            out.append((path, start, fsize))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return out


def main():
    iso = sys.argv[1]
    cmd = sys.argv[2] if len(sys.argv) > 2 else "list"
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sec, root_size = struct.unpack("<II", f.read(8))
        files = sorted(walk(f, base, root_sec, root_size))
        if cmd == "list":
            total = 0
            for p, s, n in files:
                print(f"{n:>12}  {p}")
                total += n
            print(f"partition=0x{base:X} files={len(files)} total={total}")
        elif cmd == "extract":
            outdir = sys.argv[3]
            wanted = set(sys.argv[4:])
            for p, s, n in files:
                if wanted and p not in wanted:
                    continue
                dst = os.path.join(outdir, *p.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                f.seek(base + s * SECTOR)
                with open(dst, "wb") as o:
                    left = n
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        o.write(chunk)
                        left -= len(chunk)
                print("extracted", p, n)


if __name__ == "__main__":
    main()
