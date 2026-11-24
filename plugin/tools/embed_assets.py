#!/usr/bin/env python3
"""Embeds GUI assets into plugin/src/UIAssets.{hpp,cpp}.

Currently embeds the two UI fonts, subsetted to the glyphs the GUI actually uses
(ASCII printable plus a few typographic characters). All drawing is vector
(NanoVG), so no images are embedded; if generated pickup artwork is added under
plugin/gui/ later, extend this script to embed those PNGs as well.

Run from anywhere:  python3 plugin/tools/embed_assets.py
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))

FONTS = [
    # (array name, source ttf, subset it first?)
    ("kFontSans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", True),
    ("kFontSansBold", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", True),
    # dataforte logo face: Bitcount Grid Double (OFL), instanced/subsetted/renamed
    # by tools/build_wordmark_font.py
    ("kFontLogo", os.path.join(HERE, "..", "assets", "dataforte-wordmark.ttf"), False),
]

# ASCII printable + degree sign + middle dot + angle quotes + minus sign
UNICODES = "U+0020-007E,U+00B0,U+00B7,U+2039,U+203A,U+2212"


def subset(ttf_path):
    """Subset a font with fontTools and return its bytes."""
    from fontTools import subset

    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.drop_tables += ["DSIG"]
    font = subset.load_font(ttf_path, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=[int(u, 16) for u in expand_unicodes(UNICODES)])
    subsetter.subset(font)
    out = os.path.join(os.environ.get("TMPDIR", "/tmp"), "pw_subset.ttf")
    subset.save_font(font, out, opts)
    with open(out, "rb") as f:
        return f.read()


def expand_unicodes(spec):
    """'U+0020-007E,U+00B0' -> ['20', '7e', 'b0', ...] (hex, no prefix)."""
    codes = []
    for part in spec.split(","):
        part = part.strip().replace("U+", "").replace("u+", "")
        if "-" in part:
            lo, hi = part.split("-")
            for cp in range(int(lo, 16), int(hi, 16) + 1):
                codes.append("%x" % cp)
        else:
            codes.append(part)
    return codes


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 20):
        chunk = data[i : i + 20]
        lines.append("    " + ",".join(str(b) for b in chunk) + ",")
    body = "\n".join(lines)
    return (
        "const unsigned char %s[] = {\n%s\n};\n"
        "const unsigned int %sSize = sizeof(%s);\n" % (name, body, name, name)
    )


def main():
    arrays = []
    decls = []
    for name, path, do_subset in FONTS:
        if not os.path.exists(path):
            sys.exit("missing font: %s" % path)
        if do_subset:
            data = subset(path)
        else:
            with open(path, "rb") as f:
                data = f.read()
        arrays.append(c_array(name, data))
        decls.append(
            "extern const unsigned char %s[];\nextern const unsigned int %sSize;" % (name, name)
        )
        print("embedded %s: %d bytes (subsetted from %s)" % (name, len(data), path))

    hpp = (
        "// UIAssets.hpp — asset data embedded by plugin/tools/embed_assets.py. DO NOT EDIT.\n"
        "#pragma once\n\n"
        "namespace pw {\n"
        "namespace ui {\n\n"
        + "\n".join(decls)
        + "\n\n"
        "} // namespace ui\n"
        "} // namespace pw\n"
    )
    cpp = (
        "// UIAssets.cpp — asset data embedded by plugin/tools/embed_assets.py. DO NOT EDIT.\n"
        "#include \"UIAssets.hpp\"\n\n"
        "namespace pw {\n"
        "namespace ui {\n\n"
        + "\n".join(arrays)
        + "\n"
        "} // namespace ui\n"
        "} // namespace pw\n"
    )
    with open(os.path.join(SRC, "UIAssets.hpp"), "w") as f:
        f.write(hpp)
    with open(os.path.join(SRC, "UIAssets.cpp"), "w") as f:
        f.write(cpp)
    print("wrote %s/UIAssets.{hpp,cpp}" % SRC)


if __name__ == "__main__":
    main()
