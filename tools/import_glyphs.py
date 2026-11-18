"""Copies controller button images into assets/glyphs/ under the names the launcher and game use.

  python tools/import_glyphs.py "<PlayStation folder>" "<Xbox folder>"

The PlayStation Options/Share/Select/Start images carry a black text label above or below the
button; only the button part is kept (the launcher writes the name next to it).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dds  # noqa: E402

PS = {
    'ps_cross': 'PlayStation_button_Cross', 'ps_circle': 'PlayStation_button_Circle',
    'ps_square': 'PlayStation_button_Square', 'ps_triangle': 'PlayStation_button_Triangle',
    'ps_l1': 'PlayStation_button_L1', 'ps_r1': 'PlayStation_button_R1',
    'ps_l2': 'PlayStation_4_button_L2', 'ps_r2': 'PlayStation_4_button_R2',
    'ps_l3': 'PlayStation_button_L3', 'ps_r3': 'PlayStation_button_R3',
    'ps_ls': 'PlayStation_button_analog_L', 'ps_rs': 'PlayStation_button_analog_R',
    'ps_dpad_up': 'PlayStation_Up_button', 'ps_dpad_down': 'PlayStation_Down_button',
    'ps_dpad_left': 'PlayStation_Left_button', 'ps_dpad_right': 'PlayStation_Right_button',
    'ps_share': 'PlayStation_4_Share_button', 'ps_options': 'PlayStation_4_Options_button',
    'ps_select': 'PlayStation_button_Select', 'ps_start': 'PlayStation_button_Start',
    'ps_home': 'PlayStation_button_Home',
}
XBOX = {
    'xbox_a': 'Xbox_button_A', 'xbox_b': 'Xbox_button_B', 'xbox_x': 'Xbox_button_X',
    'xbox_y': 'Xbox_button_Y', 'xbox_lb': 'Xbox_Left_Bumper', 'xbox_rb': 'Xbox_Right_Bumper',
    'xbox_lt': 'Xbox_Left_Trigger', 'xbox_rt': 'Xbox_Right_Trigger',
    'xbox_ls': 'Xbox_Left_stick', 'xbox_rs': 'Xbox_Right_stick',
    'xbox_ls_click': 'Xbox_Left_stick_button', 'xbox_rs_click': 'Xbox_Right_stick_button',
    'xbox_dpad_up': 'Xbox_D-Pad_Up', 'xbox_dpad_down': 'Xbox_D-Pad_Down',
    'xbox_dpad_left': 'Xbox_D-Pad_Left', 'xbox_dpad_right': 'Xbox_D-Pad_Right',
    'xbox_back': 'Xbox_Back_button', 'xbox_start': 'Xbox_Start_button',
}
LABELLED = {'ps_share', 'ps_options', 'ps_select', 'ps_start'}


def strip_label(w, h, px):
    """Keeps the band of rows (separated by fully transparent rows) that has no pure-black text."""
    opaque = [any(px[(y * w + x) * 4 + 3] > 16 for x in range(w)) for y in range(h)]
    bands, y = [], 0
    while y < h:
        if opaque[y]:
            y0 = y
            while y < h and opaque[y]:
                y += 1
            bands.append((y0, y))
        y += 1

    def has_text(b):
        return sum(1 for yy in range(*b) for x in range(w)
                   if px[(yy * w + x) * 4 + 3] > 128 and max(px[(yy * w + x) * 4:(yy * w + x) * 4 + 3]) < 12)

    y0, y1 = min(bands, key=has_text)
    return w, y1 - y0, px[y0 * w * 4:y1 * w * 4]


def main(ps_dir, xbox_dir):
    out = os.path.join(os.path.dirname(__file__), '..', 'assets', 'glyphs')
    os.makedirs(out, exist_ok=True)
    for table, folder in ((PS, ps_dir), (XBOX, xbox_dir)):
        for gid, name in table.items():
            w, h, px = dds.read_png(os.path.join(folder, name + '.svg.png'))
            if gid in LABELLED:
                w, h, px = strip_label(w, h, px)
            dds.write_png(os.path.join(out, gid + '.png'), w, h, px)
            print(f'{gid:16s} {w}x{h}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
