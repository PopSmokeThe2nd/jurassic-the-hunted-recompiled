"""Minimal DDS <-> RGBA tools (no dependencies): decode DXT1/DXT3/DXT5/A8R8G8B8 DDS files, read and
write PNG, and encode DXT5 / A8R8G8B8 DDS. Used to inspect and replace the game's GUI textures.

  python tools/dds.py topng in.dds out.png
  python tools/dds.py info in.dds
"""
import struct
import sys
import zlib


# ---- PNG -------------------------------------------------------------------------------------

def read_png(path):
    """Returns (w, h, rgba bytes). Supports 8-bit RGB/RGBA/grey(+alpha)/palette, non-interlaced."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and interlace == 0, f'{path}: unsupported PNG ({depth}-bit, interlace {interlace})'
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * channels
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            o = (y * w + x) * 4
            if ctype == 6:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif ctype == 0:
                out[o:o + 3] = bytes([line[x]]) * 3
                out[o + 3] = 255
            elif ctype == 4:
                out[o:o + 3] = bytes([line[x * 2]]) * 3
                out[o + 3] = line[x * 2 + 1]
            else:
                i = line[x]
                out[o:o + 3] = plte[i * 3:i * 3 + 3]
                out[o + 3] = trns[i] if trns and i < len(trns) else 255
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# ---- DDS decode ------------------------------------------------------------------------------

def info(data):
    h, w = struct.unpack('<II', data[12:20])
    mips = max(1, struct.unpack('<I', data[28:32])[0])
    pf_flags, fourcc, bits = struct.unpack('<I4sI', data[80:92])
    masks = struct.unpack('<IIII', data[92:108])
    fmt = fourcc.decode('latin1') if pf_flags & 4 else f'RGB{bits}'
    return w, h, mips, fmt, masks


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _color_block(b, rgba, w, bx, by, dxt1):
    c0, c1, bits = struct.unpack('<HHI', b)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [p0 + (255,), p1 + (255,),
               tuple((2 * a + c) // 3 for a, c in zip(p0, p1)) + (255,),
               tuple((a + 2 * c) // 3 for a, c in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + c) // 2 for a, c in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    for i in range(16):
        x, y = bx + (i & 3), by + (i >> 2)
        o = (y * w + x) * 4
        rgba[o:o + 4] = bytes(pal[(bits >> (2 * i)) & 3])


def decode(data):
    """Top mip of a DDS file as (w, h, rgba)."""
    w, h, _, fmt, masks = info(data)
    body = data[128:]
    rgba = bytearray(w * h * 4)
    if fmt in ('DXT1', 'DXT3', 'DXT5'):
        bs = 8 if fmt == 'DXT1' else 16
        p = 0
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                blk = body[p:p + bs]
                p += bs
                _color_block(blk[-8:], rgba, w, bx, by, fmt == 'DXT1')
                if fmt == 'DXT3':
                    a = int.from_bytes(blk[:8], 'little')
                    for i in range(16):
                        rgba[((by + (i >> 2)) * w + bx + (i & 3)) * 4 + 3] = ((a >> (4 * i)) & 15) * 17
                elif fmt == 'DXT5':
                    a0, a1 = blk[0], blk[1]
                    if a0 > a1:
                        pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                    else:
                        pal = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                    ab = int.from_bytes(blk[2:8], 'little')
                    for i in range(16):
                        rgba[((by + (i >> 2)) * w + bx + (i & 3)) * 4 + 3] = pal[(ab >> (3 * i)) & 7]
    elif fmt == 'RGB32' and masks == (0xff0000, 0xff00, 0xff, 0xff000000):
        for i in range(w * h):
            b, g, r, a = body[i * 4:i * 4 + 4]
            rgba[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    else:
        raise ValueError(f'unsupported DDS format {fmt} {masks}')
    return w, h, bytes(rgba)


# ---- DDS encode ------------------------------------------------------------------------------

def _header(w, h, fourcc=None, linear_size=0):
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x80000 if fourcc else 0x8)
    pitch = linear_size if fourcc else w * 4
    hdr = struct.pack('<4sIIIIIII', b'DDS ', 124, flags, h, w, pitch, 0, 0) + b'\0' * 44
    if fourcc:
        pf = struct.pack('<II4sIIIII', 32, 0x4, fourcc, 0, 0, 0, 0, 0)
    else:
        pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0' * 4, 32, 0xff0000, 0xff00, 0xff, 0xff000000)
    return hdr + pf + struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)


def encode_argb(w, h, rgba):
    body = bytearray(w * h * 4)
    for i in range(w * h):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        body[i * 4:i * 4 + 4] = bytes((b, g, r, a))
    return _header(w, h) + bytes(body)


def _to565(c):
    return (min(31, (c[0] * 31 + 127) // 255) << 11) | (min(63, (c[1] * 63 + 127) // 255) << 5) | \
        min(31, (c[2] * 31 + 127) // 255)


def encode_dxt5(w, h, rgba):
    """Simple bounding-box DXT5 encoder (good enough for flat-shaded UI icons)."""
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            px = [rgba[((by + (i >> 2)) * w + bx + (i & 3)) * 4:((by + (i >> 2)) * w + bx + (i & 3)) * 4 + 4]
                  for i in range(16)]
            # alpha: 8-level ramp between max and min
            amax, amin = max(p[3] for p in px), min(p[3] for p in px)
            abits = 0
            if amax != amin:
                for i, p in enumerate(px):
                    t = round((p[3] - amin) * 7 / (amax - amin))  # 0 = amin .. 7 = amax
                    code = {7: 0, 0: 1}.get(t, 8 - t)  # a0=amax, a1=amin, codes 2..7 interpolate
                    abits |= code << (3 * i)
            out += bytes((amax, amin)) + abits.to_bytes(6, 'little')
            # colour: bounding box of the visible pixels
            vis = [p for p in px if p[3] > 8] or px
            lo = tuple(min(p[k] for p in vis) for k in range(3))
            hi = tuple(max(p[k] for p in vis) for k in range(3))
            c0, c1 = _to565(hi), _to565(lo)
            if c0 < c1:
                c0, c1, hi, lo = c1, c0, lo, hi
            pal = [_rgb565(c0), _rgb565(c1)]
            pal += [tuple((2 * a + b) // 3 for a, b in zip(pal[0], pal[1])),
                    tuple((a + 2 * b) // 3 for a, b in zip(pal[0], pal[1]))]
            bits = 0
            if c0 != c1:
                for i, p in enumerate(px):
                    best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
                    bits |= best << (2 * i)
            out += struct.pack('<HHI', c0, c1, bits)
    return _header(w, h, b'DXT5', len(out)) + bytes(out)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'topng':
        w, h, px = decode(open(sys.argv[2], 'rb').read())
        write_png(sys.argv[3], w, h, px)
    elif cmd == 'info':
        print(info(open(sys.argv[2], 'rb').read()))
