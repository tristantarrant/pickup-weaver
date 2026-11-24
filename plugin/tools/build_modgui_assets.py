#!/usr/bin/env python3
"""Builds the modgui assets inside plugin/modgui/:

  knobs.png       61-frame knob sprite, rendered by pw_render_ui (same painter as the GUI)
  background.png  the pedal face: charcoal + brass frame, dataforte branding,
                  pickup art (rendered by pw_render_ui) and baked control labels
  icon_static.html  plain-HTML copy of icon.html for previewing/screenshotting
  screenshot.png  real-size pedal render (headless Chrome)
  thumbnail.png   screenshot scaled to 64 px height

Run after building pw_render_ui:  python3 plugin/tools/build_modgui_assets.py
"""

import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.normpath(os.path.join(HERE, ".."))
MODGUI = os.path.join(PLUGIN, "modgui")
FONTS = "/usr/share/fonts/truetype/dejavu"
BASE_W, BASE_H = 449, 430

# knob layout, must match modgui/stylesheet.css
KNOBS = [
    ("AMOUNT", 40, 195), ("GAIN", 140, 195), ("TONE", 240, 195), ("VOLUME", 340, 195),
    ("BRIGHT", 180, 280), ("BODY MIX", 280, 280), ("THICKNESS", 370, 280),
]
KNOB_SIZE = 60


def tracked_text(draw, xy, s, font, fill, tracking=1.0):
    x, y = xy
    for ch in s:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x


def main():
    from PIL import Image, ImageDraw, ImageFont

    render_py = os.path.join(HERE, "render.py")

    # ---- knob sprite ----
    knobs_png = os.path.join(MODGUI, "knobs.png")
    subprocess.run(["python3", render_py, "knobs", knobs_png, "61", str(KNOB_SIZE)], check=True)

    # ---- pickup art (humbucker), keyed ----
    art_tmp = "/tmp/opencode/pw_art.png"
    subprocess.run(["python3", render_py, "art", "2", art_tmp, "360"], check=True)
    art = Image.open(art_tmp).convert("RGBA").resize((120, 120), Image.LANCZOS)

    # ---- pedal face ----
    img = Image.new("RGB", (BASE_W, BASE_H))
    d = ImageDraw.Draw(img)
    # vertical gradient background
    for y in range(BASE_H):
        t = y / (BASE_H - 1)
        c = (int(38 - 12 * t), int(38 - 12 * t), int(44 - 13 * t))
        d.line([(0, y), (BASE_W, y)], fill=c)
    # vignette (darken the edges, fade to nothing in the middle)
    vign = Image.new("L", (BASE_W, BASE_H), 0)
    vd = ImageDraw.Draw(vign)
    for i in range(90):
        a = int(110 * (1.0 - i / 90.0) ** 2)
        vd.rectangle([i, i, BASE_W - 1 - i, BASE_H - 1 - i], outline=a)
    img = Image.composite(Image.new("RGB", img.size, (12, 12, 16)), img, vign)
    d = ImageDraw.Draw(img)

    # brass frame
    brass = (201, 168, 106)
    d.rounded_rectangle([5, 5, BASE_W - 6, BASE_H - 6], radius=10, outline=brass, width=2)
    d.rounded_rectangle([10, 10, BASE_W - 11, BASE_H - 11], radius=8, outline=(56, 56, 63), width=1)

    # corner screws
    for cx, cy in ((20, 20), (BASE_W - 21, 20), (20, BASE_H - 21), (BASE_W - 21, BASE_H - 21)):
        d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=(120, 115, 105), outline=(14, 14, 18))
        d.line([(cx - 2, cy), (cx + 2, cy)], fill=(40, 40, 46))

    # branding
    f_brand = ImageFont.truetype(os.path.join(PLUGIN, "assets", "dataforte-wordmark.ttf"), 17)
    f_sub = ImageFont.truetype(os.path.join(FONTS, "DejaVuSans.ttf"), 10)
    f_lab = ImageFont.truetype(os.path.join(FONTS, "DejaVuSans.ttf"), 8)
    tracked_text(d, (18, 12), "dataforte", f_brand, brass, 0.8)
    w_sub = sum(d.textlength(c, font=f_sub) + 2.0 for c in "PICKUP WEAVER")
    tracked_text(d, (BASE_W - 20 - w_sub, 17), "PICKUP WEAVER", f_sub, (160, 154, 140), 2.0)

    # section labels above the selects
    d.text((16, 31), "SOURCE", font=f_lab, fill=(150, 140, 115))
    d.text((144, 31), "MODEL", font=f_lab, fill=(150, 140, 115))

    # pickup art
    img.paste(art, ((BASE_W - 120) // 2, 74), art)

    # knob labels
    for label, left, top in KNOBS:
        cx = left + KNOB_SIZE // 2
        wl = sum(d.textlength(c, font=f_lab) + 0.8 for c in label)
        tracked_text(d, (cx - wl / 2, top + KNOB_SIZE + 5), label, f_lab, (160, 154, 140), 0.8)

    img.save(os.path.join(MODGUI, "background.png"))

    # ---- static preview html + screenshot ----
    with open(os.path.join(MODGUI, "icon.html")) as f:
        html = f.read()
    html = html.replace("{{#effect.ports.audio.input}}", "").replace("{{/effect.ports.audio.input}}", "")
    html = html.replace("{{#effect.ports.audio.output}}", "").replace("{{/effect.ports.audio.output}}", "")
    html = html.replace("{{{cns}}}", "").replace("{{{ns}}}", "")
    html = html.replace("/resources/", "")
    with open(os.path.join(MODGUI, "stylesheet.css")) as f:
        css = f.read().replace("{{{cns}}}", "").replace("{{{ns}}}", "").replace("/resources/", "")

    static = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
html,body{margin:0;padding:0;width:%dpx;height:%dpx;background:#101014;overflow:hidden}
%s
</style></head><body>%s</body></html>
""" % (BASE_W, BASE_H, css, html)
    static_path = os.path.join(MODGUI, "icon_static.html")
    with open(static_path, "w") as f:
        f.write(static)

    shot = os.path.join(MODGUI, "screenshot.png")
    chrome = ["google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
              "--hide-scrollbars", "--force-device-scale-factor=1",
              "--window-size=%d,%d" % (BASE_W, BASE_H),
              "--screenshot=" + shot, "file://" + static_path]
    try:
        subprocess.run(chrome, check=True, capture_output=True)
        print("screenshot via headless Chrome")
    except Exception as ex:  # fall back to the face without live controls
        print("headless Chrome failed (%s); using the pedal face as screenshot" % ex)
        img.save(shot)

    thumb = Image.open(shot)
    h = 64
    w = max(1, round(thumb.width * h / thumb.height))
    thumb.resize((w, h), Image.LANCZOS).save(os.path.join(MODGUI, "thumbnail.png"))
    print("wrote knobs.png, background.png, icon_static.html, screenshot.png, thumbnail.png")


if __name__ == "__main__":
    main()
