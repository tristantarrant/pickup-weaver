#!/usr/bin/env python3
"""Renders GUI screenshots and modgui assets headlessly.

Runs plugin/build/pw_render_ui under Xvfb and converts its PPM output:
  ui    -> RGB png (GUI screenshot at a given state)
  knobs -> RGBA png with the magenta background keyed out (modgui knob sprite)
  art   -> RGBA png with the magenta background keyed out (pickup art)

Usage:
  render.py ui <out.png> [width height] [sym=value ...] [hover=kind[:index]] [mx=px my=px]
  render.py knobs <out.png> [frames] [size]
  render.py art <family> <out.png> [size] [bright]

`hover=kind[:index]` (kind: knob, tile, row, toggle, pad, prev, next, bypass)
plus `mx`/`my` draw the UI as if the pointer were over that control, which is
how tooltips are verified headlessly.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "pw_render_ui")


def key_magenta(rgb_image):
    """Convert an image rendered on a pure magenta background to RGBA.

    observed = a*fg + (1-a)*(255,0,255); the magenta-ness
    m = (r+b)/2 - g is 255 for the background and ~0 for grays/brass/cream,
    so alpha = 255 - m and fg = (observed - (1-a)*bg) / a.
    """
    from PIL import Image

    px = rgb_image.convert("RGB")
    out = Image.new("RGBA", px.size)
    d = px.load()
    od = out.load()
    for y in range(px.size[1]):
        for x in range(px.size[0]):
            r, g, b = d[x, y]
            m = (r + b) // 2 - g
            a = 255 - m
            if a <= 0:
                od[x, y] = (0, 0, 0, 0)
            elif a >= 255:
                od[x, y] = (r, g, b, 255)
            else:
                ia = a / 255.0
                fr = int(max(0, min(255, (r - (1 - ia) * 255) / ia)))
                fg = int(max(0, min(255, g / ia)))
                fb = int(max(0, min(255, (b - (1 - ia) * 255) / ia)))
                od[x, y] = (fr, fg, fb, a)
    return out


def run_ppm(args):
    # args: [mode, ...mode args]; the output path is inserted right after the mode
    fd, ppm = tempfile.mkstemp(suffix=".ppm", dir="/tmp/opencode")
    os.close(fd)
    try:
        env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1")
        subprocess.run(
            ["xvfb-run", "-a", "-s", "-extension MIT-SHM", RENDER, args[0], ppm] + args[1:],
            check=True, env=env,
        )
        from PIL import Image

        return Image.open(ppm)
    finally:
        if os.path.exists(ppm):
            os.unlink(ppm)


def main():
    from PIL import Image

    if not os.path.exists(RENDER):
        sys.exit("renderer not built: %s (cmake --build plugin/build --target pw_render_ui)" % RENDER)

    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "ui":
        out = sys.argv[2]
        rest = sys.argv[3:]
        w, h, state = "1180", "640", rest
        if rest and rest[0].isdigit():
            w = rest[0]
            if len(rest) > 1 and rest[1].isdigit():
                h = rest[1]
                state = rest[2:]
            else:
                state = rest[1:]
        img = run_ppm(["ui", w, h] + state)
        img.save(out)
    elif mode == "knobs":
        out = sys.argv[2]
        frames = sys.argv[3] if len(sys.argv) > 3 else "61"
        size = sys.argv[4] if len(sys.argv) > 4 else "60"
        img = run_ppm(["knobs", frames, size])
        key_magenta(img).save(out)
    elif mode == "art":
        family = sys.argv[2]
        out = sys.argv[3]
        size = sys.argv[4] if len(sys.argv) > 4 else "360"
        bright = sys.argv[5] if len(sys.argv) > 5 else ""
        img = run_ppm(["art", family, size] + ([bright] if bright else []))
        key_magenta(img).save(out)
    else:
        sys.exit(__doc__)
    print("wrote", out)


if __name__ == "__main__":
    main()
