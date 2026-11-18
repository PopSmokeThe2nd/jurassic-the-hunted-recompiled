"""Dump XEX2 header info: optional headers, imports, base, entry, compression/encryption."""
import struct, sys

KEYS = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO", 0x000005FF: "DELTA_PATCH_DESC",
    0x00000405: "BASE_REFERENCE", 0x00008001: "BOUNDING_PATH", 0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS", 0x00010100: "ENTRY_POINT", 0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES", 0x00018002: "CHECKSUM_TIMESTAMP", 0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP", 0x000183FF: "ORIGINAL_PE_NAME", 0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO", 0x00020200: "DEFAULT_STACK_SIZE", 0x00020301: "DEFAULT_FS_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE", 0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS", 0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO", 0x000400FF: "SERVICE_ID_LIST", 0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS", 0x00040404: "LAN_KEY", 0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS", 0x000407FF: "ALTERNATE_TITLE_IDS", 0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main():
    d = open(sys.argv[1], "rb").read()
    assert d[:4] == b"XEX2", d[:4]
    flags, pe_off, _, sec_off, n_opt = struct.unpack_from(">IIIII", d, 4)
    print(f"module_flags=0x{flags:X} pe_data_offset=0x{pe_off:X} security_offset=0x{sec_off:X} opt_headers={n_opt}")
    image_size = be32(d, sec_off + 4)
    load_addr = be32(d, sec_off + 0x110)
    print(f"image_size=0x{image_size:X} load_address=0x{load_addr:X}")
    for i in range(n_opt):
        key, val = struct.unpack_from(">II", d, 0x18 + i * 8)
        name = KEYS.get(key, f"0x{key:08X}")
        if key & 0xFF == 0x00:
            print(f"  {name}: 0x{val:X}")
        elif key & 0xFF == 0x01:
            print(f"  {name}: 0x{val:X}")
        else:
            print(f"  {name}: @0x{val:X}")
        if key == 0x000003FF:
            size, enc, comp = struct.unpack_from(">IHH", d, val)
            print(f"    encryption={enc} ({'normal/AES' if enc else 'none'}) compression={comp} "
                  f"({ {0:'none',1:'basic',2:'LZX',3:'delta'}.get(comp,comp)})")
        if key == 0x00010100:
            print(f"    entry=0x{val:X}")
        if key == 0x000103FF:
            size = be32(d, val)
            str_size, n_libs = struct.unpack_from(">II", d, val + 4)
            names = d[val + 12: val + 12 + str_size].split(b"\0")
            names = [n.decode() for n in names if n]
            p = val + 12 + str_size
            for _ in range(n_libs):
                lsize = be32(d, p)
                ver = be32(d, p + 0x20)
                name_idx, count = struct.unpack_from(">HH", d, p + 0x24)
                print(f"    import {names[name_idx]} v{ver>>28}.{(ver>>24)&0xF}.{(ver>>8)&0xFFFF}.{ver&0xFF} records={count}")
                p += lsize
        if key == 0x000200FF:
            size = be32(d, val)
            n = (size - 4) // 16
            for j in range(n):
                o = val + 4 + j * 16
                nm = d[o:o + 8].rstrip(b"\0").decode()
                a, b_, c, e = struct.unpack_from(">HHHH", d, o + 8)
                print(f"    lib {nm:<10} {a}.{b_}.{c}.{e & 0x7FFF}")
        if key == 0x000183FF:
            size = be32(d, val)
            print(f"    name={d[val+4:val+size].rstrip(b'\\0').decode()}")
        if key == 0x00040006:
            media, ver, base_ver, tid = struct.unpack_from(">IIII", d, val)
            print(f"    title_id=0x{tid:08X} version=0x{ver:08X}")


if __name__ == "__main__":
    main()
