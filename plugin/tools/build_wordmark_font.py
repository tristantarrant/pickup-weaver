#!/usr/bin/env python3
"""Builds the wordmark font for the dataforte logo.

Takes the Bitcount Grid Double variable font (SIL Open Font License), instances it
at weight 700, subsets it to printable ASCII and renames the family to
"dataforte Wordmark" (the original name is not used for the modified font).

Usage:
  python3 plugin/tools/build_wordmark_font.py <BitcountGridDouble[...].ttf> <OFL.txt>

Writes plugin/assets/dataforte-wordmark.ttf and plugin/assets/OFL-Bitcount.txt.
The upstream font is available from https://fonts.google.com/specimen/Bitcount+Grid+Double
(or https://github.com/google/fonts/tree/main/ofl/bitcountgriddouble).
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "assets"))

FAMILY = "dataforte Wordmark"
STYLE = "Regular"
PS_NAME = "dataforteWordmark-Regular"
UNICODES = "U+0020,U+0061,U+0064,U+0065,U+0066,U+006F,U+0072,U+0074"   # "dataforte" + space


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, ofl = sys.argv[1], sys.argv[2]

    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont

    font = TTFont(src)

    # subset to the glyphs a wordmark needs (before instancing: gvar subsetting of
    # an already-instanced variable font is not supported by fontTools)
    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.drop_tables += ["DSIG"]
    sub = subset.Subsetter(options=opts)
    codes = []
    for part in UNICODES.split(","):
        part = part.replace("U+", "").replace("u+", "")
        if "-" in part:
            lo, hi = part.split("-")
            codes += list(range(int(lo, 16), int(hi, 16) + 1))
        else:
            codes.append(int(part, 16))
    sub.populate(unicodes=codes)
    sub.subset(font)

    # pin the axes: static weight (default 400 keeps the distinct dot grid)
    instantiateVariableFont(font, {"wght": int(sys.argv[3]) if len(sys.argv) > 3 else 400}, inplace=True)
    for tag in ("gvar", "fvar", "avar", "HVAR", "MVAR"):
        if tag in font:
            del font[tag]

    # flatten composite glyphs into plain contours: the dot glyphs are composites,
    # and NanoVG's older stb_truetype rasterizer does not draw them
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    glyf = font["glyf"]
    glyphSet = font.getGlyphSet()
    for name in font.getGlyphOrder():
        if glyf[name].numberOfContours == -1:
            rec = DecomposingRecordingPen(glyphSet)
            glyphSet[name].draw(rec)
            pen = TTGlyphPen(None)
            rec.replay(pen)
            glyf[name] = pen.glyph()

    # rename the modified font (copyright, trademark and license notices are kept)
    names = font["name"]
    renames = {
        1: FAMILY,
        2: STYLE,
        3: "%s; %s" % (FAMILY, PS_NAME),
        4: "%s %s" % (FAMILY, STYLE),
        6: PS_NAME,
        16: FAMILY,
        17: STYLE,
    }
    for rec in names.names:
        if rec.nameID in renames:
            rec.string = renames[rec.nameID].encode(rec.getEncoding())

    os.makedirs(ASSETS, exist_ok=True)
    out = os.path.join(ASSETS, "dataforte-wordmark.ttf")
    font.save(out)
    shutil.copyfile(ofl, os.path.join(ASSETS, "OFL-Bitcount.txt"))
    print("wrote %s (%d bytes) + OFL-Bitcount.txt" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
