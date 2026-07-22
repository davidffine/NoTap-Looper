"""
Rasterise NoTap's IconView geometry so it can actually be LOOKED at.

Mirrors IconView.kt exactly: a 0..1 icon box, Android arc semantics (0deg at
3 o'clock, positive = clockwise, y down), round caps/joins, and the same
fill-vs-stroke split that onDraw uses. Supersampled 8x then downscaled, because
ImageDraw has no antialiasing and jaggies would hide the real defects.

Usage: py -3.14 render_icons.py <out.png> [spec_module]
"""
import math, sys
from PIL import Image, ImageDraw, ImageFont

SS = 8                       # supersample factor
BG      = (11, 14, 21)
SURFACE = (17, 22, 36)
STROKE  = (51, 64, 92)
HI      = (234, 240, 251)
MID     = (142, 156, 182)
LO      = (88, 98, 119)
CYAN    = (37, 213, 240)
RED     = (255, 59, 96)
GREEN   = (52, 226, 155)
AMBER   = (255, 176, 32)

W_DEFAULT = 0.092


# ---------- geometry helpers (0..1 icon-box space) ----------
def arc(l, t, r, b, start, sweep, steps=64):
    cx, cy, rx, ry = (l + r) / 2, (t + b) / 2, (r - l) / 2, (b - t) / 2
    return [(cx + rx * math.cos(math.radians(start + sweep * i / steps)),
             cy + ry * math.sin(math.radians(start + sweep * i / steps)))
            for i in range(steps + 1)]


def rr(l, t, r, b, rad, steps=10):
    pts = []
    for (cx, cy, a0) in ((r - rad, t + rad, -90), (r - rad, b - rad, 0),
                         (l + rad, b - rad, 90), (l + rad, t + rad, 180)):
        for i in range(steps + 1):
            a = math.radians(a0 + 90 * i / steps)
            pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts


def quad(p0, p1, p2, steps=16):
    out = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        out.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
    return out


def star(cx, cy, r, waist):
    pts = []
    pts += quad((cx, cy - r), (cx + waist, cy - waist), (cx + r, cy))
    pts += quad((cx + r, cy), (cx + waist, cy + waist), (cx, cy + r))
    pts += quad((cx, cy + r), (cx - waist, cy + waist), (cx - r, cy))
    pts += quad((cx - r, cy), (cx - waist, cy - waist), (cx, cy - r))
    return pts


def circle(cx, cy, r, steps=40):
    return [(cx + r * math.cos(2 * math.pi * i / steps),
             cy + r * math.sin(2 * math.pi * i / steps)) for i in range(steps)]


# ---------- drawing ----------
def draw_icon(img, spec, ox, oy, size, color, weight=W_DEFAULT):
    """ox/oy/size are in FINAL pixels; drawing happens supersampled."""
    d = ImageDraw.Draw(img)
    s = size * 0.78 * SS
    bx = (ox * SS) + (size * SS - s) / 2
    by = (oy * SS) + (size * SS - s) / 2
    lw = max(1, int(round(s * weight)))

    def P(pts):
        return [(bx + x * s, by + y * s) for (x, y) in pts]

    for pts in spec.get('fill', []):
        d.polygon(P(pts), fill=color)
    # even-odd group: first contour filled, the rest punched out (RGBA layer)
    for group in spec.get('holed', []):
        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        dl = ImageDraw.Draw(layer)
        dl.polygon(P(group[0]), fill=color + (255,))
        for hole in group[1:]:
            dl.polygon(P(hole), fill=(0, 0, 0, 0))
        img.paste(layer, (0, 0), layer)
    for pts in spec.get('stroke', []):
        q = P(pts)
        if len(q) < 2:
            continue
        d.line(q, fill=color, width=lw, joint='curve')
        for (x, y) in (q[0], q[-1]):          # round caps
            d.ellipse([x - lw / 2, y - lw / 2, x + lw / 2, y + lw / 2], fill=color)


def head(px, py, dx, dy, size):
    """Filled triangle at (px,py) pointing along (dx,dy)."""
    m = math.hypot(dx, dy) or 1.0
    ux, uy = dx / m, dy / m
    nx, ny = -uy, ux
    return [(px + ux * size, py + uy * size),
            (px - ux * size * 0.35 + nx * size * 0.8, py - uy * size * 0.35 + ny * size * 0.8),
            (px - ux * size * 0.35 - nx * size * 0.8, py - uy * size * 0.35 - ny * size * 0.8)]


def arc_head(l, t, r, b, start, sweep, size):
    """Arrowhead sitting at the END of an arc, aimed along its tangent."""
    cx, cy, rx, ry = (l + r) / 2, (t + b) / 2, (r - l) / 2, (b - t) / 2
    e = math.radians(start + sweep)
    px, py = cx + rx * math.cos(e), cy + ry * math.sin(e)
    sgn = 1.0 if sweep >= 0 else -1.0
    return head(px, py, -math.sin(e) * sgn, math.cos(e) * sgn, size)


def gear(r_out, r_in, r_hole, teeth=8, tw=13.0, bevel=6.0):
    pts = []
    step = 360.0 / teeth
    for i in range(teeth):
        a = i * step
        for (r, ang) in ((r_in, a - step / 2 + bevel), (r_out, a - tw),
                         (r_out, a + tw), (r_in, a + step / 2 - bevel)):
            pts.append((0.5 + r * math.cos(math.radians(ang)),
                        0.5 + r * math.sin(math.radians(ang))))
    return [pts, circle(0.5, 0.5, r_hole, 44)]


def make_sheet(icons, path, title):
    names = list(icons.keys())
    cols = 6
    rows = (len(names) + cols - 1) // cols
    CW, CH = 150, 108
    Wpx, Hpx = cols * CW + 24, rows * CH + 90
    img = Image.new('RGB', (Wpx * SS, Hpx * SS), BG)
    d = ImageDraw.Draw(img)

    for i, name in enumerate(names):
        cx = 12 + (i % cols) * CW
        cy = 60 + (i // cols) * CH
        d.rounded_rectangle([cx * SS, cy * SS, (cx + CW - 10) * SS, (cy + CH - 10) * SS],
                            radius=14 * SS, fill=SURFACE, outline=STROKE, width=SS)
        col = CYAN if name.startswith('VOLUME') else AMBER if name == 'CROWN' else \
              RED if name == 'TRASH' else HI
        # 48 / 24 / 15 px — the sizes these are actually used at
        draw_icon(img, icons[name], cx + 10, cy + 16, 48, col)
        draw_icon(img, icons[name], cx + 62, cy + 28, 24, col)
        draw_icon(img, icons[name], cx + 92, cy + 33, 15, col)
        draw_icon(img, icons[name], cx + 116, cy + 36, 12, col)

    img = img.resize((Wpx, Hpx), Image.LANCZOS)
    d2 = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("arialbd.ttf", 15)
        fs = ImageFont.truetype("arial.ttf", 10)
    except Exception:
        f = fs = ImageFont.load_default()
    d2.text((14, 20), title, font=f, fill=MID)
    for i, name in enumerate(names):
        cx = 12 + (i % cols) * CW
        cy = 60 + (i // cols) * CH
        d2.text((cx + 12, cy + CH - 28), name, font=fs, fill=LO)
    img.save(path)
    print("wrote", path, img.size)


# ---------- the spec, mirroring IconView.kt ----------
SPEAKER = [(0.10, 0.36), (0.26, 0.36), (0.46, 0.16), (0.46, 0.84), (0.26, 0.64), (0.10, 0.64)]

ICONS = {
    'PLAY':   {'fill': [[(0.26, 0.14), (0.84, 0.5), (0.26, 0.86)]]},
    'PAUSE':  {'fill': [rr(0.26, 0.15, 0.41, 0.85, 0.05), rr(0.59, 0.15, 0.74, 0.85, 0.05)]},
    'STOP':   {'fill': [rr(0.22, 0.22, 0.78, 0.78, 0.08)]},
    'VOLUME_HIGH': {'fill': [SPEAKER],
                    'stroke': [arc(0.34, 0.24, 0.78, 0.76, -55, 110),
                               arc(0.28, 0.12, 0.92, 0.88, -52, 104)]},
    'VOLUME_LOW':  {'fill': [SPEAKER], 'stroke': [arc(0.34, 0.24, 0.78, 0.76, -55, 110)]},
    'VOLUME_OFF':  {'fill': [SPEAKER],
                    'stroke': [[(0.60, 0.36), (0.88, 0.64)], [(0.88, 0.36), (0.60, 0.64)]]},
    'SPARKLE': {'fill': [star(0.40, 0.42, 0.38, 0.028), star(0.79, 0.79, 0.19, 0.016)]},
    'SLIDERS': {'stroke': [[(0.12, 0.24), (0.88, 0.24)], [(0.12, 0.5), (0.88, 0.5)],
                           [(0.12, 0.76), (0.88, 0.76)]],
                'fill': [circle(0.66, 0.24, 0.085), circle(0.36, 0.5, 0.085),
                         circle(0.56, 0.76, 0.085)]},
    'CROWN':  {'fill': [[(0.09, 0.30), (0.28, 0.56), (0.50, 0.20), (0.72, 0.56), (0.91, 0.30),
                         (0.85, 0.82), (0.15, 0.82)]]},
    'TRASH':  {'stroke': [[(0.14, 0.28), (0.86, 0.28)],
                          [(0.38, 0.28), (0.40, 0.16), (0.60, 0.16), (0.62, 0.28)],
                          [(0.22, 0.28), (0.28, 0.86), (0.72, 0.86), (0.78, 0.28)],
                          [(0.42, 0.42), (0.44, 0.72)], [(0.58, 0.42), (0.56, 0.72)]]},
    'LOCK':   {'stroke': [rr(0.19, 0.45, 0.81, 0.87, 0.10) + [rr(0.19, 0.45, 0.81, 0.87, 0.10)[0]],
                          arc(0.32, 0.15, 0.68, 0.51, 180, 180)]},
    'UNDO':   {'stroke': [arc(0.24, 0.26, 0.84, 0.86, 200, 232)],
               'fill':   [arc_head(0.24, 0.26, 0.84, 0.86, 200, 250, 0.165)]},
    'GEAR':   {'holed': [gear(0.47, 0.345, 0.155)]},
    'MAIL':   {'stroke': [rr(0.10, 0.24, 0.90, 0.76, 0.07) + [rr(0.10, 0.24, 0.90, 0.76, 0.07)[0]],
                          [(0.10, 0.28), (0.50, 0.56), (0.90, 0.28)]]},
    'BULB':   {'stroke': [circle(0.5, 0.38, 0.27) + [circle(0.5, 0.38, 0.27)[0]],
                          [(0.38, 0.65), (0.38, 0.76)], [(0.62, 0.65), (0.62, 0.76)],
                          [(0.38, 0.72), (0.62, 0.72)], [(0.42, 0.85), (0.58, 0.85)]]},
    'CLOSE':  {'stroke': [[(0.20, 0.20), (0.80, 0.80)], [(0.80, 0.20), (0.20, 0.80)]]},
    'DOWNLOAD': {'stroke': [[(0.5, 0.14), (0.5, 0.60)],
                            [(0.30, 0.40), (0.5, 0.60), (0.70, 0.40)],
                            [(0.16, 0.72), (0.16, 0.86), (0.84, 0.86), (0.84, 0.72)]]},
    'FADE':   {'fill': [rr(0.08, 0.14, 0.20, 0.86, 0.045), rr(0.26, 0.27, 0.38, 0.86, 0.045),
                        rr(0.44, 0.40, 0.56, 0.86, 0.045), rr(0.62, 0.55, 0.74, 0.86, 0.045),
                        rr(0.80, 0.70, 0.92, 0.86, 0.045)]},
    'HEADPHONES': {'stroke': [arc(0.14, 0.16, 0.86, 0.80, 180, 180),
                              rr(0.10, 0.46, 0.30, 0.84, 0.07) + [rr(0.10, 0.46, 0.30, 0.84, 0.07)[0]],
                              rr(0.70, 0.46, 0.90, 0.84, 0.07) + [rr(0.70, 0.46, 0.90, 0.84, 0.07)[0]]]},
    'SOLO':   {'fill': [rr(0.42, 0.14, 0.58, 0.86, 0.05)],
               'stroke': [rr(0.12, 0.34, 0.28, 0.66, 0.05) + [rr(0.12, 0.34, 0.28, 0.66, 0.05)[0]],
                          rr(0.72, 0.34, 0.88, 0.66, 0.05) + [rr(0.72, 0.34, 0.88, 0.66, 0.05)[0]]]},
}

if __name__ == '__main__':
    make_sheet(ICONS, sys.argv[1] if len(sys.argv) > 1 else 'icons_before.png',
               "NOTAP ICONS  -  48 / 24 / 15 / 12 px  (12px = the crown badge)")
