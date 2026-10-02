#!/usr/bin/env python3
"""Generate the OMAWIN365 logo (assets/logo.svg, assets/logo.txt), horizontal
header (assets/header.svg), app icon (assets/omawin365.svg, the Windows 365
tile on its own), and README banner (docs/banner.svg; render docs/banner.png
with rsvg-convert -w 1600).

Everything lives on one pixel grid. The o/m/a glyphs are copied from
Omarchy's logo (15px cells); the rest are drawn to match. A pixel-art
Windows 365 Cloud PC tile sits above the wordmark.
"""

import pathlib

CELL = 15

GLYPHS = {
    "o": """
..#####..
.#######.
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
.#######.
..#####..""",
    "m": """
..###########..
.#############.
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
.##...###...##.
..#...###...#..""",
    "a": """
...#######
..########
.###...###
.###...###
.###...###
.###...###
.###...###
##########
##########
.###...###
.###...###
.###...###
.###...###
.###...###
.###...##.
.###...#..""",
    "w": """
..#...###...#..
.##...###...##.
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
###...###...###
.#############.
..###########..""",
    "i": """
.#.
###
...
###
###
###
###
###
###
###
###
###
###
###
##.
#..""",
    "n": """
..#####..
.#######.
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
###...###
.##...##.
..#...#..""",
    "3": """
.#######.
#########
#.....###
......###
......###
......###
......###
..#######
..#######
......###
......###
......###
......###
#.....###
#########
.#######.""",
    "6": """
...#######
..#######.
.###......
.###......
.###......
.###......
.###......
##########
##########
.###...###
.###...###
.###...###
.###...###
.###...###
.#########
..#######.""",
    "5": """
.#########
.#########
.###......
.###......
.###......
.###......
.###......
#########.
#########.
.......###
.......###
.......###
.......###
.#.....###
.#########
..#######.""",
}

# Windows 365 tile gradients (light, dark), back to front, from the official logo.
TILES = [("#0078d4", "#0c59a4"), ("#28afea", "#0078d4"), ("#50e6ff", "#1493df")]
WHITE = "#ffffff"


def _mix(a, b, t):
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ca, cb))


TILE_SHADES = [[_mix(light, dark, t / 2) for t in range(3)] for light, dark in TILES]


def wordmark(text, gap=2):
    rows = [g.strip("\n").split("\n") for g in (GLYPHS[c] for c in text)]
    width = sum(len(r[0]) for r in rows) + gap * (len(rows) - 1)
    out = {}
    x0 = 0
    for g in rows:
        for y, line in enumerate(g):
            for x, ch in enumerate(line):
                if ch == "#":
                    out[(x0 + x, y)] = "text"
        x0 += len(g[0]) + gap
    return out, width, len(rows[0])


def tile(label=True):
    """Stacked-tile mark: three rounded tiles, "win" over "365" on the front.

    The stack follows Wikimedia's Windows365-logo.svg; the label replaces its
    white 2x2 squares. It is drawn at twice the wordmark's resolution so the
    label can use the wordmark's own glyphs. Each tile keeps a clear border
    over the one behind it, so the stack still reads without colour.
    label=False draws the plain stack at wordmark resolution, for logo.txt.
    """
    k = 2 if label else 1
    size, step, pad = 23 * k, 4 * k, k
    corner = [3, 2, 1, 1] if label else [2, 1]  # pixels trimmed from each end of the first rows
    full = size + step * 2
    px = {}

    def cells(x0, y0, pad=0):
        for y in range(-pad, size + pad):
            edge = min(y, size - 1 - y) + pad
            trim = corner[edge] if edge < len(corner) else 0
            for x in range(trim - pad, size + pad - trim):
                yield x0 + x, y0 + y

    for i in range(len(TILES)):
        x0, y0 = step * (2 - i), step * i
        for c in cells(x0, y0, pad):
            px.pop(c, None)
        for x, y in cells(x0, y0):
            # Stepped diagonal gradient, lighter towards the top left.
            t = min(2, ((x - x0) + (y - y0)) * 3 // (size * 2))
            px[(x, y)] = TILE_SHADES[i][t]
    if label:
        lines = [wordmark(word)[:2] for word in ("win", "365")]  # (pixels, width) per line
        gap = 3
        height = 16 * 2 + gap
        y = step * 2 + (size - height) // 2
        for (cells_, width) in lines:
            x = (size - width + 1) // 2
            for (cx, cy) in cells_:
                px[(x + cx, y + cy)] = WHITE
            y += 16 + gap
    return px, full, full


def build(fine=True):
    """Mark above the wordmark. fine=True works on the mark's 2x grid (for the
    SVG, drawn at half a cell); fine=False uses the plain 1x stack (for logo.txt)."""
    text, tw, th = wordmark("omawin365")
    fl, fw, fh = tile(label=fine)
    spacing = 5
    if fine:
        text = {(2 * x + dx, 2 * y + dy): c for (x, y), c in text.items() for dx in (0, 1) for dy in (0, 1)}
        tw, th, spacing = tw * 2, th * 2, spacing * 2
    width = max(tw, fw)
    fx = (width - fw) // 2
    pixels = {(x + fx, y): c for (x, y), c in fl.items()}
    ty = fh + spacing
    pixels.update({(x + (width - tw) // 2, y + ty): c for (x, y), c in text.items()})
    return pixels, width, ty + th


def paths(pixels, cell=CELL, text_attr='class="t"'):
    by_colour = {}
    for (x, y), c in sorted(pixels.items(), key=lambda p: (p[0][1], p[0][0])):
        by_colour.setdefault(c, []).append((x, y))
    out = []
    for c, cells in by_colour.items():
        # Merge horizontal runs to keep the file small.
        runs, d = [], []
        for x, y in cells:
            if runs and runs[-1][1] == y and runs[-1][0] + runs[-1][2] == x:
                runs[-1][2] += 1
            else:
                runs.append([x, y, 1])
        for x, y, n in runs:
            d.append(f"M{x * cell} {y * cell}h{n * cell}v{cell}h-{n * cell}z")
        attr = text_attr if c == "text" else f'fill="{c}"'
        out.append(f'<path {attr} d="{"".join(d)}"/>')
    return "".join(out)


def svg(pixels, width, height, cell=CELL):
    w, h = round(width * cell), round(height * cell)
    style = ""
    if "text" in pixels.values():
        style = "<style>.t{fill:#1a1b26}@media (prefers-color-scheme:dark){.t{fill:#e8e8e8}}</style>"
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
        + style
        + paths(pixels, cell)
        + "</svg>\n"
    )


def header():
    """Transparent horizontal branding, using the README banner's composition."""
    mark, mw, mh = tile()
    text, tw, th = wordmark("omawin365")
    cell, gap = 7, 48
    mcell = cell / 2
    width, height = round(mw * mcell + gap + tw * cell), round(mh * mcell)
    tx = round(mw * mcell + gap)
    ty = round((height - th * cell) / 2)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title">'
        '<title id="title">OMAWIN365</title>'
        '<style>.t{fill:#1a1b26}@media (prefers-color-scheme:dark){.t{fill:#e8e8e8}}</style>'
        + paths(mark, mcell)
        + f'<g transform="translate({tx},{ty})">{paths(text, cell)}</g>'
        + "</svg>\n"
    )


def banner():
    """README hero banner in the same template as the vichong/omarchy-* plugins."""
    mark, mw, mh = tile()
    text, tw, th = wordmark("omawin365")
    cell, gap = 7, 48
    mcell = cell / 2  # the mark is drawn on a 2x grid
    x0 = round((1280 - mw * mcell - tw * cell - gap) / 2)
    my = 118
    ty = round(my + (mh * mcell - th * cell) / 2)
    font = 'font-family="JetBrainsMono Nerd Font, JetBrains Mono, monospace"'
    stops = "".join(
        f'<stop offset="{i / 3:.2f}" stop-color="{c}"/>'
        for i, c in enumerate(("#0c59a4", "#0078d4", "#28afea", "#50e6ff"))
    )
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="640" viewBox="0 0 1280 640">
<!-- Generated by tools/logo.py; render docs/banner.png with rsvg-convert -w 1600. -->
<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#171a21"/><stop offset="1" stop-color="#0f1116"/>
  </linearGradient>
  <linearGradient id="rule" x1="0" y1="0" x2="1" y2="0">{stops}</linearGradient>
</defs>
<rect width="1280" height="640" fill="url(#bg)"/>
<rect x="0" y="0" width="1280" height="6" fill="url(#rule)"/>
<g transform="translate({x0},{my})">{paths(mark, mcell)}</g>
<g transform="translate({round(x0 + mw * mcell + gap)},{ty})">{paths(text, cell, 'fill="#e6e8ec"')}</g>
<text x="640" y="450" text-anchor="middle" {font} font-size="30" fill="#e6e8ec">Windows 365 Cloud PCs, native on Omarchy</text>
<text x="640" y="498" text-anchor="middle" {font} font-size="22" fill="#8a919e">Microsoft sign-in · FIDO2 security keys via WebAuthn · FreeRDP under the hood</text>
<text x="640" y="580" text-anchor="middle" {font} font-size="16" fill="#4f5563">community app · native Qt client · tested with Windows 365 Enterprise</text>
<text x="640" y="606" text-anchor="middle" {font} font-size="16" fill="#4f5563">not affiliated with or endorsed by Microsoft or Omarchy</text>
</svg>
"""


def txt(pixels, width, height):
    # Two grid rows per terminal line, using half blocks like Omarchy's logo.txt.
    lines = []
    for y in range(0, height, 2):
        line = ""
        for x in range(width):
            # The white panes become holes, so the tile reads without colour.
            top = pixels.get((x, y), WHITE) != WHITE
            bot = pixels.get((x, y + 1), WHITE) != WHITE
            line += "█" if top and bot else "▀" if top else "▄" if bot else " "
        lines.append(line.rstrip())
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent / "assets"
    (root / "logo.svg").write_text(svg(*build(), cell=CELL / 2))
    (root / "header.svg").write_text(header())
    (root / "logo.txt").write_text(txt(*build(fine=False)))
    (root / "omawin365.svg").write_text(svg(*tile(), cell=CELL / 2))
    (root.parent / "docs" / "banner.svg").write_text(banner())
