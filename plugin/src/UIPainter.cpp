// UIPainter.cpp — vector drawing and interaction geometry for the Pickup Weaver GUI.
// Everything is drawn with NanoVG (no raster assets): the GUI scales crisply at any
// size, and the same code renders the modgui knob sprite via tools/render_ui.cpp.

#include "UIPainter.hpp"
#include "UIAssets.hpp"
#include "PluginParams.hpp"
#include "PickupWeaverTables.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nanovg/nanovg.h"

namespace pw {
namespace ui {

using namespace pw::plugin;

// ---------------------------------------------------------------- helpers

static inline NVGcolor rgbi(int r, int g, int b)
{
    return nvgRGB((unsigned char)r, (unsigned char)g, (unsigned char)b);
}

static inline NVGcolor rgbai(int r, int g, int b, int a)
{
    return nvgRGBA((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

static const NVGcolor kBrass       = rgbi(201, 168, 106);
static const NVGcolor kBrassBright = rgbi(232, 205, 138);
static const NVGcolor kBrassDark   = rgbi(138, 116, 74);
static const NVGcolor kText        = rgbi(236, 231, 219);
static const NVGcolor kTextMid     = rgbi(160, 154, 140);
static const NVGcolor kTextDim     = rgbi(106, 101, 92);
static const NVGcolor kCardBorder  = rgbi(58, 58, 65);

static void text(NVGcontext* vg, float x, float y, const char* str, float size,
                 NVGcolor color, int align, bool bold, float tracking = 0.0f)
{
    nvgFontFace(vg, bold ? "bold" : "sans");
    nvgFontSize(vg, size);
    nvgFontBlur(vg, 0.0f);
    nvgTextLetterSpacing(vg, tracking);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, color);
    nvgText(vg, x, y, str, nullptr);
}

static void rr(NVGcontext* vg, float x, float y, float w, float h, float r)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, r);
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- model tables

struct ListEntry { uint8_t isHeader; int8_t model; int8_t group; };

static const ListEntry kListEntries[35] = {
    {1, -1, 0}, {0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0},
    {1, -1, 1}, {0, 7, 1}, {0, 8, 1},
    {1, -1, 2}, {0, 9, 2}, {0, 10, 2}, {0, 11, 2}, {0, 12, 2}, {0, 13, 2}, {0, 14, 2},
    {0, 15, 2}, {0, 16, 2}, {0, 17, 2},
    {1, -1, 3}, {0, 18, 3},
    {1, -1, 4}, {0, 19, 4}, {0, 20, 4}, {0, 21, 4}, {0, 22, 4}, {0, 23, 4}, {0, 24, 4},
    {0, 25, 4}, {0, 26, 4},
    {1, -1, 5}, {0, 27, 5}, {0, 28, 5},
};

static const char* const kGroupNames[6] = {
    "SINGLE COIL", "P90", "HUMBUCKER", "PIEZO", "ACOUSTIC", "CUSTOM"
};

int familyOfModel(int model)
{
    if (model <= 6) return 0;
    if (model <= 8) return 1;
    if (model <= 17) return 2;
    if (model == 18) return 3;
    if (model <= 26) return 4;
    return 5;
}

struct KnobDef { int param; const char* label; };
static const KnobDef kKnobs[7] = {
    { kAmount, "Mix" }, { kGain, "Gain" }, { kBrightness, "Brightness" },
    { kBodyAmount, "Mix" }, { kBodyThickness, "Thickness" },
    { kVolume, "Volume" }, { kTone, "Tone" },
};

// Hover hints, one per kKnobs entry / source tile.
static const char* const kKnobHints[7] = {
    "Morph between the source pickup and the target model",
    "Output level after the pickup model (-20 to +20 dB)",
    "Pickup and cable brightness (resonance and load)",
    "How much of the hollow-body resonance is mixed in",
    "Body depth: sets the lowest resonance mode",
    "Volume-pot position (with its treble bleed)",
    "Tone-pot position (10 = fully open)",
};

static const char* const kTileHints[4] = {
    "Source pickup: humbucker",
    "Source pickup: single coil",
    "Source pickup: bright single coil",
    "Source pickup: piezo under-saddle strip",
};

const char* tooltipText(const Hit& h)
{
    switch (h.type)
    {
    case Hit::KNOB:
        for (int k = 0; k < 7; ++k)
            if (kKnobs[k].param == h.index)
                return kKnobHints[k];
        return nullptr;
    case Hit::TILE:   return (h.index >= 0 && h.index < 4) ? kTileHints[h.index] : nullptr;
    case Hit::TOGGLE: return "Invert the hollow-body resonance (phase)";
    case Hit::BYPASS: return "Bypass the whole effect";
    case Hit::PREV:   return "Previous target model";
    case Hit::NEXT:   return "Next target model";
    case Hit::PAD:    return "Drag to shape the E-Custom and AC-Custom models";
    default:          return nullptr;
    }
}

static const char* const kTileLabels[4][2] = {
    { "Humbucker", nullptr },
    { "Single Coil", nullptr },
    { "Single Coil", "(Bright)" },
    { "Piezo", nullptr },
};

// ---------------------------------------------------------------- params / values

void Params::setDefaults()
{
    for (uint32_t i = 0; i < kParamCount; ++i)
        v[i] = kSpecs[i].def;
}

void valueText(int param, float value, char* buf, size_t cap)
{
    const ParamSpec& s = kSpecs[param];
    if (s.isBool)
    {
        std::snprintf(buf, cap, "%s", value > 0.5f ? "on" : "off");
        return;
    }
    switch (param)
    {
    case kGain:         std::snprintf(buf, cap, "%+.1f dB", value); break;
    case kVolume:
    case kTone:         std::snprintf(buf, cap, "%.2f", value); break;
    case kBodyThickness: std::snprintf(buf, cap, "%.0f mm", value); break;
    case kBrightness:   std::snprintf(buf, cap, "%+.0f%%", value); break;
    case kCustomX:
    case kCustomY:
    case kAmount:
    case kBodyAmount:   std::snprintf(buf, cap, "%.0f%%", value); break;
    default:            std::snprintf(buf, cap, "%.0f", value); break;
    }
}

// ---------------------------------------------------------------- layout

Layout makeLayout(float w, float h)
{
    Layout L;
    const float s = w / (float)kBaseWidth;
    const float oy = (h - (float)kBaseHeight * s) * 0.5f;
    L.s = s;

    const auto R = [&](float x, float y, float ww, float hh) {
        return RectF{ x * s, oy + y * s, ww * s, hh * s };
    };

    L.header = R(16, 16, 1148, 48);
    L.bypassBtn = R(1054, 25, 110, 30);

    L.srcPanel = R(16, 76, 240, 548);
    for (int i = 0; i < 4; ++i)
        L.tiles[i] = R(26, 112 + i * 126, 220, 114);

    L.tgtPanel = R(268, 76, 432, 548);
    L.art = R(280, 112, 100, 100);
    L.groupLbl = R(392, 118, 220, 16);
    L.nameLbl = R(392, 136, 230, 30);
    L.nextBtn = R(652, 144, 36, 36);
    L.prevBtn = R(610, 144, 36, 36);
    L.listArea = R(280, 224, 408, 388);
    L.listRowH = 20.0f * s;
    L.listColW = 198.0f * s;

    L.ctlPanel = R(712, 76, 300, 548);
    {
        static const int kRowOf[7] = { 0, 0, 0, 1, 1, 2, 2 };
        static const int kColOf[7] = { 0, 1, 2, 0, 1, 0, 1 };
        static const float kRowCy[3] = { 156.0f, 318.0f, 512.0f };
        static const int kRowN[3] = { 3, 3, 2 };
        for (int k = 0; k < 7; ++k)
        {
            const int row = kRowOf[k], col = kColOf[k];
            const float cw = 280.0f / (float)kRowN[row];
            const float cx = 722.0f + col * cw + cw * 0.5f;
            const float cy = kRowCy[row];
            L.knobCell[k] = RectF{ (cx - 42.0f) * s, oy + (cy - 42.0f) * s, 84.0f * s, 84.0f * s };
        }
        // the hollow-body phase switch takes the third cell of the body row
        const float cw = 280.0f / (float)kRowN[1];
        const float cx = 722.0f + 2 * cw + cw * 0.5f;
        L.phaseCell = RectF{ (cx - 42.0f) * s, oy + (kRowCy[1] - 42.0f) * s, 84.0f * s, 84.0f * s };
    }

    L.customPanel = R(1024, 76, 140, 548);
    L.pad = R(1036, 136, 116, 116);
    L.padReadout = R(1036, 260, 116, 16);

    return L;
}

Hit hitTest(const Layout& L, float x, float y)
{
    Hit h;
    if (L.bypassBtn.contains(x, y)) { h.type = Hit::BYPASS; return h; }

    for (int i = 0; i < 4; ++i)
        if (L.tiles[i].contains(x, y)) { h.type = Hit::TILE; h.index = i; return h; }

    if (L.prevBtn.contains(x, y)) { h.type = Hit::PREV; return h; }
    if (L.nextBtn.contains(x, y)) { h.type = Hit::NEXT; return h; }

    if (L.listArea.contains(x, y))
    {
        const int col = (x < L.listArea.x + L.listColW + 6.0f * L.s) ? 0 : 1;
        const int row = (int)((y - L.listArea.y) / L.listRowH);
        const int e = col * 18 + row;
        if (e >= 0 && e < 35 && !kListEntries[e].isHeader)
        {
            h.type = Hit::ROW;
            h.index = kListEntries[e].model;
            return h;
        }
    }

    for (int k = 0; k < 7; ++k)
    {
        RectF c = L.knobCell[k];
        c.h += 36.0f * L.s;   // include label/value area
        if (c.contains(x, y)) { h.type = Hit::KNOB; h.index = kKnobs[k].param; return h; }
    }

    {
        RectF c = L.phaseCell;
        c.h += 36.0f * L.s;   // include label/value area
        if (c.contains(x, y)) { h.type = Hit::TOGGLE; h.index = kBodyPhase; return h; }
    }

    if (L.pad.contains(x, y)) { h.type = Hit::PAD; return h; }

    return h;
}

// ---------------------------------------------------------------- painter init

bool Painter::init(NVGcontext* vg)
{
    fFontRegular = nvgCreateFontMem(vg, "sans", const_cast<unsigned char*>(kFontSans),
                                    (int)kFontSansSize, 0);
    fFontBold = nvgCreateFontMem(vg, "bold", const_cast<unsigned char*>(kFontSansBold),
                                 (int)kFontSansBoldSize, 0);
    fFontLogo = nvgCreateFontMem(vg, "logo", const_cast<unsigned char*>(kFontLogo),
                                 (int)kFontLogoSize, 0);
    return fFontRegular >= 0 && fFontBold >= 0 && fFontLogo >= 0;
}

void Painter::destroy(NVGcontext*) {}

// ---------------------------------------------------------------- pieces

static void drawArrowButton(NVGcontext* vg, const RectF& r, bool left, bool hover)
{
    rr(vg, r.x, r.y, r.w, r.h, 7.0f);
    nvgFillColor(vg, hover ? rgbi(44, 44, 50) : rgbi(37, 37, 43));
    nvgFill(vg);
    nvgStrokeColor(vg, hover ? kBrass : kCardBorder);
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const float d = r.w * 0.18f;
    nvgBeginPath(vg);
    if (left)
    {
        nvgMoveTo(vg, cx + d * 0.7f, cy - d);
        nvgLineTo(vg, cx - d * 0.9f, cy);
        nvgLineTo(vg, cx + d * 0.7f, cy + d);
    }
    else
    {
        nvgMoveTo(vg, cx - d * 0.7f, cy - d);
        nvgLineTo(vg, cx + d * 0.9f, cy);
        nvgLineTo(vg, cx - d * 0.7f, cy + d);
    }
    nvgClosePath(vg);
    nvgFillColor(vg, hover ? kBrassBright : kTextMid);
    nvgFill(vg);
}

// Round on/off switch drawn in the knob's body style: same brass bezel and
// recessed cap, with a lamp in the middle and a ring that lights when engaged.
static void drawRoundToggle(NVGcontext* vg, float cx, float cy, float r, bool on, bool hot)
{
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 1.20f);
    nvgStrokeColor(vg, on ? (hot ? kBrassBright : kBrass) : rgbi(78, 74, 66));
    nvgStrokeWidth(vg, std::fmax(1.5f, r * 0.055f));
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx - r, cy - r, cx + r, cy + r,
                                       rgbi(223, 194, 136), rgbi(138, 116, 74)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(92, 77, 51));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 0.74f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx - r * 0.28f, cy - r * 0.32f, r * 0.05f, r * 1.1f,
                                       rgbi(60, 60, 68), rgbi(22, 22, 26)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(13, 13, 16));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    if (on)
    {
        nvgBeginPath(vg);
        nvgRect(vg, cx - r, cy - r, 2.0f * r, 2.0f * r);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, r * 0.05f, r * 0.95f,
                                           rgbai(232, 205, 138, 80), rgbai(232, 205, 138, 0)));
        nvgFill(vg);
    }

    // power glyph: open ring with a stem, so it reads as a switch, not a knob
    const float gr = r * 0.30f;
    nvgStrokeColor(vg, on ? kBrassBright : rgbi(104, 99, 90));
    nvgStrokeWidth(vg, std::fmax(1.6f, r * 0.075f));
    nvgLineCap(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, gr, -1.0208f, 4.1624f, NVG_CW);   // gap at the top
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx, cy - gr * 1.55f);
    nvgLineTo(vg, cx, cy + gr * 0.10f);
    nvgStroke(vg);
}

// Short description of a control, drawn next to the mouse pointer.
static void drawTooltip(NVGcontext* vg, float w, float h, float s, float mx, float my,
                        const char* str)
{
    const float maxW = 220.0f * s;
    const float padX = 9.0f * s, padY = 7.0f * s;

    nvgSave(vg);
    nvgFontFace(vg, "sans");
    nvgFontSize(vg, 11.0f * s);
    nvgFontBlur(vg, 0.0f);
    nvgTextLetterSpacing(vg, 0.2f * s);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    float b[4];
    nvgTextBoxBounds(vg, 0.0f, 0.0f, maxW, str, nullptr, b);
    const float tw = b[2] - b[0];
    const float th = b[3] - b[1];

    float bx = mx + 16.0f * s;
    float by = my + 18.0f * s;
    if (bx + tw + 2.0f * padX > w - 8.0f * s)   // flip to the left of the pointer
        bx = mx - tw - 2.0f * padX - 10.0f * s;
    if (by + th + 2.0f * padY > h - 8.0f * s)   // flip above the pointer
        by = my - th - 2.0f * padY - 12.0f * s;
    bx = std::fmax(8.0f * s, bx);
    by = std::fmax(8.0f * s, by);

    rr(vg, bx, by, tw + 2.0f * padX, th + 2.0f * padY, 6.0f * s);
    nvgFillColor(vg, rgbai(18, 18, 22, 244));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbai(201, 168, 106, 110));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgFillColor(vg, kText);
    nvgTextBox(vg, bx + padX, by + padY - b[1], maxW, str, nullptr);
    nvgRestore(vg);
}

void Painter::paintKnobSprite(NVGcontext* vg, float size, float value01) const
{
    // standalone knob for modgui sprites (no text)
    const float cx = size * 0.5f, cy = size * 0.5f, r = size * 0.34f;
    const float a0 = 2.35619449f, a1 = a0 + 4.71238898f;   // 135°..405°: gap at the bottom
    const float ang = a0 + (a1 - a0) * clamp01(value01);

    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r * 1.20f, a0, a1, NVG_CW);
    nvgStrokeColor(vg, rgbi(78, 74, 66));
    nvgStrokeWidth(vg, std::fmax(1.5f, r * 0.06f));
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx - r, cy - r, cx + r, cy + r,
                                       rgbi(223, 194, 136), rgbi(138, 116, 74)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(92, 77, 51));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 0.74f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx - r * 0.28f, cy - r * 0.32f, r * 0.05f, r * 1.1f,
                                       rgbi(60, 60, 68), rgbi(22, 22, 26)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(13, 13, 16));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, ang);
    nvgBeginPath(vg);
    nvgMoveTo(vg, r * 0.18f, 0.0f);
    nvgLineTo(vg, r * 0.60f, 0.0f);
    nvgStrokeColor(vg, rgbi(242, 234, 214));
    nvgStrokeWidth(vg, r * 0.10f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
    nvgRestore(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 0.09f);
    nvgFillColor(vg, kBrassDark);
    nvgFill(vg);
}

static void drawKnob(NVGcontext* vg, float cx, float cy, float r, float v01, bool hot)
{
    const float a0 = 2.35619449f, a1 = a0 + 4.71238898f;   // 135°..405°: gap at the bottom
    const float ang = a0 + (a1 - a0) * clamp01(v01);

    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r * 1.20f, a0, a1, NVG_CW);
    nvgStrokeColor(vg, rgbi(78, 74, 66));
    nvgStrokeWidth(vg, std::fmax(1.5f, r * 0.055f));
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    if (v01 > 0.003f)
    {
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r * 1.20f, a0, ang, NVG_CW);
        nvgStrokeColor(vg, hot ? kBrassBright : kBrass);
        nvgStroke(vg);
    }

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx - r, cy - r, cx + r, cy + r,
                                       rgbi(223, 194, 136), rgbi(138, 116, 74)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(92, 77, 51));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 0.74f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx - r * 0.28f, cy - r * 0.32f, r * 0.05f, r * 1.1f,
                                       rgbi(60, 60, 68), rgbi(22, 22, 26)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(13, 13, 16));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, ang);
    nvgBeginPath(vg);
    nvgMoveTo(vg, r * 0.18f, 0.0f);
    nvgLineTo(vg, r * 0.60f, 0.0f);
    nvgStrokeColor(vg, rgbi(242, 234, 214));
    nvgStrokeWidth(vg, r * 0.10f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
    nvgRestore(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r * 0.09f);
    nvgFillColor(vg, kBrassDark);
    nvgFill(vg);
}

// ---------------------------------------------------------------- pickup art

static void poleCircle(NVGcontext* vg, float cx, float cy, float r, bool slug)
{
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx - r * 0.3f, cy - r * 0.3f, r * 0.1f, r * 1.05f,
                                       slug ? rgbi(196, 196, 204) : rgbi(226, 226, 234),
                                       slug ? rgbi(88, 88, 96) : rgbi(98, 98, 106)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(52, 52, 58));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
}

static void screwHead(NVGcontext* vg, float cx, float cy, float r)
{
    poleCircle(vg, cx, cy, r, false);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - r * 0.55f, cy - r * 0.55f);
    nvgLineTo(vg, cx + r * 0.55f, cy + r * 0.55f);
    nvgStrokeColor(vg, rgbi(58, 58, 64));
    nvgStrokeWidth(vg, std::fmax(1.0f, r * 0.24f));
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

// Authored upright: the pickup's long axis runs along y.
static void paintPickupUpright(NVGcontext* vg, float x, float y, float w, float h,
                               int family, bool bright)
{
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;

    switch (family)
    {
    case 0:   // single coil
    {
        if (bright)
        {
            nvgBeginPath(vg);
            nvgRect(vg, x, y, w, h);
            nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, w * 0.10f, w * 0.55f,
                                                rgbai(232, 205, 138, 80), rgbai(232, 205, 138, 0)));
            nvgFill(vg);
        }
        const float bw = w * 0.34f, bh = h * 0.86f;
        rr(vg, cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, bw * 0.18f);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - bh * 0.5f, cx, cy + bh * 0.5f,
                                           rgbi(238, 230, 210), rgbi(201, 189, 157)));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(163, 151, 122));
        nvgStrokeWidth(vg, std::fmax(1.0f, w * 0.008f));
        nvgStroke(vg);
        const float pr = bw * 0.16f, span = bh * 0.76f;
        for (int i = 0; i < 6; ++i)
        {
            const float py = cy - span * 0.5f + span * i / 5.0f;
            poleCircle(vg, cx, py, pr, false);
        }
        break;
    }
    case 1:   // P90 soapbar
    {
        const float bw = w * 0.62f, bh = h * 0.72f;
        rr(vg, cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, w * 0.14f);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - bh * 0.5f, cx, cy + bh * 0.5f,
                                           rgbi(236, 223, 192), rgbi(203, 185, 142)));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(163, 147, 108));
        nvgStrokeWidth(vg, std::fmax(1.0f, w * 0.008f));
        nvgStroke(vg);
        const float pr = bw * 0.075f, span = bh * 0.66f;
        for (int i = 0; i < 6; ++i)
        {
            const float py = cy - span * 0.5f + span * i / 5.0f;
            poleCircle(vg, cx, py, pr, false);
        }
        screwHead(vg, cx - bw * 0.36f, cy + bh * 0.36f, w * 0.028f);
        screwHead(vg, cx + bw * 0.36f, cy + bh * 0.36f, w * 0.028f);
        break;
    }
    case 2:   // humbucker
    {
        const float bw = w * 0.60f, bh = h * 0.86f;
        rr(vg, cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, w * 0.05f);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx - bw * 0.5f, cy - bh * 0.5f, cx + bw * 0.5f, cy + bh * 0.5f,
                                           rgbi(226, 197, 143), rgbi(162, 130, 76)));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(111, 92, 57));
        nvgStrokeWidth(vg, std::fmax(1.0f, w * 0.008f));
        nvgStroke(vg);
        const float pr = bw * 0.075f, span = bh * 0.72f;
        for (int i = 0; i < 6; ++i)
        {
            const float py = cy - span * 0.5f + span * i / 5.0f;
            screwHead(vg, cx - bw * 0.20f, py, pr);
            poleCircle(vg, cx + bw * 0.20f, py, pr, true);
        }
        screwHead(vg, cx - bw * 0.40f, cy - bh * 0.40f, bw * 0.045f);
        screwHead(vg, cx + bw * 0.40f, cy - bh * 0.40f, bw * 0.045f);
        screwHead(vg, cx - bw * 0.40f, cy + bh * 0.40f, bw * 0.045f);
        screwHead(vg, cx + bw * 0.40f, cy + bh * 0.40f, bw * 0.045f);
        break;
    }
    case 3:   // piezo under-saddle strip
    {
        const float ww = w * 0.92f, wh = h * 0.17f;
        rr(vg, cx - ww * 0.5f, cy - wh * 0.5f - h * 0.02f, ww, wh, wh * 0.3f);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - wh, cx, cy + wh,
                                           rgbi(124, 85, 53), rgbi(78, 52, 30)));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(46, 31, 18));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        const float sw = ww * 0.82f, sh = wh * 0.42f;
        rr(vg, cx - sw * 0.5f, cy - sh * 0.5f - h * 0.045f, sw, sh, sh * 0.45f);
        nvgFillColor(vg, rgbi(227, 221, 209));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(154, 148, 138));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        nvgStrokeColor(vg, rgbai(60, 55, 48, 140));
        nvgStrokeWidth(vg, std::fmax(1.0f, sw * 0.006f));
        for (int i = 1; i < 6; ++i)
        {
            const float lx = cx - sw * 0.5f + sw * i / 6.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, lx, cy - sh * 0.5f - h * 0.045f + sh * 0.15f);
            nvgLineTo(vg, lx, cy + sh * 0.5f - h * 0.045f - sh * 0.15f);
            nvgStroke(vg);
        }
        nvgFillColor(vg, kBrass);
        nvgBeginPath(vg);
        nvgRect(vg, cx - sw * 0.5f, cy - sh * 0.5f - h * 0.045f, sw * 0.05f, sh);
        nvgRect(vg, cx + sw * 0.5f - sw * 0.05f, cy - sh * 0.5f - h * 0.045f, sw * 0.05f, sh);
        nvgFill(vg);
        break;
    }
    case 4:   // acoustic body
    {
        const float R = w * 0.30f;
        const float lx = cx, ly = cy + h * 0.08f, ur = R * 0.78f, uy = cy - h * 0.14f;
        // outline (drawn as a larger dark union underneath)
        nvgBeginPath(vg);
        nvgCircle(vg, lx, uy, ur + w * 0.018f);
        nvgCircle(vg, lx, ly, R + w * 0.018f);
        nvgFillColor(vg, rgbi(46, 32, 20));
        nvgFill(vg);
        // spruce top
        nvgBeginPath(vg);
        nvgCircle(vg, lx, uy, ur);
        nvgCircle(vg, lx, ly, R);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx, y, cx, y + h,
                                           rgbi(230, 201, 155), rgbi(181, 137, 90)));
        nvgFill(vg);
        // soundhole + rosette
        const float hx = cx, hy = cy + h * 0.005f, hr = R * 0.34f;
        nvgBeginPath(vg);
        nvgCircle(vg, hx, hy, hr * 1.24f);
        nvgStrokeColor(vg, rgbai(201, 168, 106, 210));
        nvgStrokeWidth(vg, std::fmax(1.2f, R * 0.05f));
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, hx, hy, hr);
        nvgFillPaint(vg, nvgRadialGradient(vg, hx, hy, hr * 0.1f, hr,
                                           rgbi(30, 20, 13), rgbi(10, 7, 5)));
        nvgFill(vg);
        // bridge
        rr(vg, cx - R * 0.55f, cy + h * 0.22f, R * 1.1f, h * 0.032f, 2.0f);
        nvgFillColor(vg, rgbi(36, 24, 18));
        nvgFill(vg);
        break;
    }
    default:  // custom (grid overlay)
    {
        const float bw = w * 0.60f, bh = h * 0.86f;
        rr(vg, cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, w * 0.05f);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx - bw * 0.5f, cy - bh * 0.5f, cx + bw * 0.5f, cy + bh * 0.5f,
                                           rgbi(61, 61, 70), rgbi(35, 35, 41)));
        nvgFill(vg);
        nvgStrokeColor(vg, rgbi(16, 16, 20));
        nvgStrokeWidth(vg, std::fmax(1.0f, w * 0.008f));
        nvgStroke(vg);
        const float pr = bw * 0.07f, span = bh * 0.72f;
        for (int i = 0; i < 6; ++i)
        {
            const float py = cy - span * 0.5f + span * i / 5.0f;
            nvgBeginPath(vg);
            nvgCircle(vg, cx - bw * 0.20f, py, pr);
            nvgCircle(vg, cx + bw * 0.20f, py, pr);
            nvgFillColor(vg, rgbi(46, 46, 54));
            nvgFill(vg);
            nvgStrokeColor(vg, rgbi(70, 70, 80));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        nvgStrokeColor(vg, rgbai(201, 168, 106, 130));
        nvgStrokeWidth(vg, 1.2f);
        for (int i = -1; i <= 1; ++i)
        {
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx + bw * 0.28f * i, cy - bh * 0.42f);
            nvgLineTo(vg, cx + bw * 0.28f * i, cy + bh * 0.42f);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - bw * 0.46f, cy + bh * 0.28f * i);
            nvgLineTo(vg, cx + bw * 0.46f, cy + bh * 0.28f * i);
            nvgStroke(vg);
        }
        nvgStrokeColor(vg, rgbai(232, 205, 138, 200));
        nvgStrokeWidth(vg, 1.6f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy - bh * 0.42f);
        nvgLineTo(vg, cx, cy + bh * 0.42f);
        nvgMoveTo(vg, cx - bw * 0.46f, cy);
        nvgLineTo(vg, cx + bw * 0.46f, cy);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, w * 0.028f);
        nvgFillColor(vg, kBrassBright);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, w * 0.02f, w * 0.30f,
                                           rgbai(232, 205, 138, 46), rgbai(232, 205, 138, 0)));
        nvgFill(vg);
        break;
    }
    }
}

void Painter::paintPickupArt(NVGcontext* vg, float x, float y, float w, float h,
                             int family, bool bright) const
{
    // Pickups are shown lying horizontal. The artwork above is authored upright,
    // so draw it a quarter turn clockwise with the rect's sides swapped. The two
    // exceptions are the piezo strip, which is already horizontal, and the
    // acoustic body, which is a body outline rather than a pickup.
    if (family == 3 || family == 4)
    {
        paintPickupUpright(vg, x, y, w, h, family, bright);
        return;
    }

    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, 1.5707964f);   // 90° (y grows downwards, so this is clockwise)
    paintPickupUpright(vg, -h * 0.5f, -w * 0.5f, h, w, family, bright);
    nvgRestore(vg);
}

// ---------------------------------------------------------------- panels

static void paintPanel(NVGcontext* vg, const RectF& r, float s, const char* title)
{
    rr(vg, r.x, r.y, r.w, r.h, 10.0f * s);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.x, r.y + r.h,
                                       rgbi(38, 38, 44), rgbi(33, 33, 38)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgbi(56, 56, 63));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    if (title != nullptr)
        text(vg, r.x + 12.0f * s, r.y + 13.0f * s, title, 12.0f * s, kBrass,
             NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 2.4f * s);
}

static void paintCard(NVGcontext* vg, const RectF& r, float s, bool selected, bool hovered)
{
    if (selected)
    {
        rr(vg, r.x - 3.0f * s, r.y - 3.0f * s, r.w + 6.0f * s, r.h + 6.0f * s, 11.0f * s);
        nvgFillPaint(vg, nvgBoxGradient(vg, r.x - 3.0f * s, r.y - 3.0f * s,
                                        r.w + 6.0f * s, r.h + 6.0f * s, 10.0f * s, 9.0f * s,
                                        rgbai(201, 168, 106, 70), rgbai(201, 168, 106, 0)));
        nvgFill(vg);
    }
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.42f;
    rr(vg, r.x, r.y, r.w, r.h, 8.0f * s);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, r.w * 0.05f, r.w * 0.8f,
                                       rgbi(50, 50, 57), rgbi(34, 34, 40)));
    nvgFill(vg);
    nvgStrokeColor(vg, selected ? kBrass : (hovered ? rgbi(90, 86, 78) : kCardBorder));
    nvgStrokeWidth(vg, selected ? 2.0f * s : 1.0f * s);
    nvgStroke(vg);
}

// ---------------------------------------------------------------- full paint

void Painter::paint(NVGcontext* vg, float w, float h, const Params& p,
                    const Hit& hover, const Hit& grab, float mouseX, float mouseY) const
{
    const Layout L = makeLayout(w, h);
    const float s = L.s;
    (void)fFontRegular;
    (void)fFontBold;

    // background
    nvgBeginPath(vg);
    nvgRect(vg, 0.0f, 0.0f, w, h);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0.0f, 0.0f, 0.0f, h, rgbi(31, 31, 36), rgbi(21, 21, 25)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, 0.0f, 0.0f, w, h);
    nvgFillPaint(vg, nvgRadialGradient(vg, w * 0.5f, h * 0.30f, h * 0.2f, h * 1.05f,
                                       rgbai(0, 0, 0, 0), rgbai(0, 0, 0, 96)));
    nvgFill(vg);

    const int model = clampi((int)(p.v[kPickupTo] + 0.5f), 0, 28);
    const int from = clampi((int)(p.v[kPickupFrom] + 0.5f), 0, 3);

    // ---- header ----
    {
        const float wy = L.header.y + L.header.h * 0.5f + s;
        // wordmark (shadow + brass gradient)
        text(vg, 30.0f * s, wy + 1.5f * s, "dataforte", 36.0f * s, rgbai(0, 0, 0, 130),
             NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, false, 1.0f * s);
        nvgFontFace(vg, "logo");
        nvgFontSize(vg, 36.0f * s);
        nvgTextLetterSpacing(vg, 1.0f * s);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float bounds[4];
        nvgTextBounds(vg, 30.0f * s, wy, "dataforte", nullptr, bounds);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0.0f, wy - 16.0f * s, 0.0f, wy + 16.0f * s,
                                           rgbi(242, 216, 154), rgbi(185, 148, 92)));
        nvgText(vg, 30.0f * s, wy, "dataforte", nullptr);

        const float dx = bounds[2] + 18.0f * s;
        nvgBeginPath(vg);
        nvgMoveTo(vg, dx, L.header.y + 12.0f * s);
        nvgLineTo(vg, dx, L.header.y + L.header.h - 12.0f * s);
        nvgStrokeColor(vg, rgbi(56, 56, 63));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        text(vg, dx + 14.0f * s, wy, "PICKUP WEAVER", 13.0f * s, kTextMid,
             NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, false, 3.0f * s);

        // bypass pill
        const RectF& b = L.bypassBtn;
        const bool bypassed = p.v[kBypass] > 0.5f;
        const bool bhover = hover.type == Hit::BYPASS;
        rr(vg, b.x, b.y, b.w, b.h, b.h * 0.5f);
        if (!bypassed)
            nvgFillColor(vg, bhover ? kBrassBright : kBrass);
        else
            nvgFillColor(vg, rgbi(37, 37, 43));
        nvgFill(vg);
        if (bypassed)
        {
            nvgStrokeColor(vg, bhover ? kBrass : kBrassDark);
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, b.x + 13.0f * s, b.y + b.h * 0.5f, 4.0f * s);
        nvgFillColor(vg, bypassed ? kBrass : rgbi(24, 24, 28));
        nvgFill(vg);
        text(vg, b.x + b.w - 12.0f * s, b.y + b.h * 0.5f, bypassed ? "BYPASSED" : "ACTIVE",
             12.0f * s, bypassed ? kBrass : rgbi(24, 24, 28),
             NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, true, 1.0f * s);
    }

    // ---- source panel ----
    paintPanel(vg, L.srcPanel, s, "SOURCE PICKUP");
    for (int i = 0; i < 4; ++i)
    {
        const RectF& t = L.tiles[i];
        const bool sel = from == i;
        const bool hov = hover.type == Hit::TILE && hover.index == i;
        paintCard(vg, t, s, sel, hov);
        paintPickupArt(vg, t.x + 9.0f * s, t.y + 15.0f * s, 130.0f * s, 84.0f * s,
                       i == 3 ? 3 : (i == 0 ? 2 : 0), i == 2);
        // labels sit in the column to the right of the artwork, vertically centred
        int lines = 0;
        for (int ln = 0; ln < 2; ++ln)
            if (kTileLabels[i][ln] != nullptr)
                ++lines;
        const float ly = t.y + t.h * 0.5f - (float)(lines - 1) * 7.5f * s;
        for (int ln = 0; ln < 2; ++ln)
        {
            if (kTileLabels[i][ln] == nullptr)
                break;
            text(vg, t.x + 180.0f * s, ly + ln * 15.0f * s, kTileLabels[i][ln],
                 11.0f * s, sel ? kBrassBright : kTextMid,
                 NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, sel, 0.4f * s);
        }
    }
    // ---- target panel ----
    paintPanel(vg, L.tgtPanel, s, "TARGET MODEL");
    {
        const bool hovArt = false;
        paintCard(vg, L.art, s, true, hovArt);
        paintPickupArt(vg, L.art.x + 9.0f * s, L.art.y + 9.0f * s,
                       L.art.w - 18.0f * s, L.art.h - 18.0f * s, familyOfModel(model), false);

        text(vg, L.groupLbl.x, L.groupLbl.y, kGroupNames[familyOfModel(model)], 10.5f * s,
             kBrass, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 2.2f * s);
        text(vg, L.nameLbl.x, L.nameLbl.y, tables::kPresetNames[model], 21.0f * s,
             kText, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 0.3f * s);

        drawArrowButton(vg, L.prevBtn, true, hover.type == Hit::PREV);
        drawArrowButton(vg, L.nextBtn, false, hover.type == Hit::NEXT);

        // model list (two columns)
        const float colGap = 12.0f * s;
        for (int e = 0; e < 35; ++e)
        {
            const int col = e < 18 ? 0 : 1;
            const int row = col ? e - 18 : e;
            const float rx = L.listArea.x + col * (L.listColW + colGap);
            const float ry = L.listArea.y + row * L.listRowH;
            const ListEntry& en = kListEntries[e];
            if (en.isHeader)
            {
                text(vg, rx + 2.0f * s, ry + L.listRowH * 0.5f, kGroupNames[en.group],
                     9.5f * s, kBrassDark, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, true, 1.8f * s);
                continue;
            }
            const bool sel = en.model == model;
            const bool hov = hover.type == Hit::ROW && hover.index == en.model;
            if (sel)
            {
                rr(vg, rx, ry + 1.0f * s, L.listColW, L.listRowH - 2.0f * s, 4.0f * s);
                nvgFillColor(vg, rgbai(201, 168, 106, 38));
                nvgFill(vg);
                nvgFillColor(vg, kBrass);
                nvgBeginPath(vg);
                nvgRect(vg, rx, ry + 3.0f * s, 2.5f * s, L.listRowH - 6.0f * s);
                nvgFill(vg);
            }
            else if (hov)
            {
                rr(vg, rx, ry + 1.0f * s, L.listColW, L.listRowH - 2.0f * s, 4.0f * s);
                nvgFillColor(vg, rgbai(255, 255, 255, 14));
                nvgFill(vg);
            }
            text(vg, rx + 11.0f * s, ry + L.listRowH * 0.5f, tables::kPresetNames[en.model],
                 12.5f * s, sel ? kBrassBright : rgbi(216, 211, 198),
                 NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, sel, 0.2f * s);
        }
    }

    // ---- controls panel ----
    paintPanel(vg, L.ctlPanel, s, nullptr);
    {
        const float px = L.ctlPanel.x + 12.0f * s;
        const float py = L.ctlPanel.y;
        text(vg, px, py + 14.0f * s, "PICKUP", 12.0f * s, kBrass,
             NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 2.4f * s);
        text(vg, px, py + 176.0f * s, "HOLLOW BODY", 12.0f * s, kBrass,
             NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 2.4f * s);
        text(vg, px, py + 370.0f * s, "OUTPUT", 12.0f * s, kBrass,
             NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true, 2.4f * s);
    }
    for (int k = 0; k < 7; ++k)
    {
        const RectF& c = L.knobCell[k];
        const float cx = c.x + c.w * 0.5f;
        const float cy = c.y + c.h * 0.5f;
        const ParamSpec& sp = kSpecs[kKnobs[k].param];
        const float v01 = (p.v[kKnobs[k].param] - sp.min) / (sp.max - sp.min);
        const bool hot = (grab.type == Hit::KNOB && grab.index == kKnobs[k].param)
                      || (hover.type == Hit::KNOB && hover.index == kKnobs[k].param);
        drawKnob(vg, cx, cy, 30.0f * s, v01, hot);
        text(vg, cx, c.y + c.h + 7.0f * s, kKnobs[k].label, 11.0f * s, kTextMid,
             NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false, 0.3f * s);
        char buf[48];
        valueText(kKnobs[k].param, p.v[kKnobs[k].param], buf, sizeof(buf));
        text(vg, cx, c.y + c.h + 22.0f * s, buf, 12.5f * s, rgbi(207, 201, 187),
             NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false);
    }
    {
        const RectF& c = L.phaseCell;
        const float cx = c.x + c.w * 0.5f;
        const float cy = c.y + c.h * 0.5f;
        const bool on = p.v[kBodyPhase] > 0.5f;
        const bool hot = hover.type == Hit::TOGGLE && hover.index == kBodyPhase;
        drawRoundToggle(vg, cx, cy, 30.0f * s, on, hot);
        text(vg, cx, c.y + c.h + 7.0f * s, "Phase", 11.0f * s, kTextMid,
             NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false, 0.3f * s);
        char buf[48];
        valueText(kBodyPhase, p.v[kBodyPhase], buf, sizeof(buf));
        text(vg, cx, c.y + c.h + 22.0f * s, buf, 12.5f * s, rgbi(207, 201, 187),
             NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false);
    }

    // ---- custom X/Y panel ----
    paintPanel(vg, L.customPanel, s, "CUSTOM X / Y");
    {
        const RectF& pad = L.pad;
        const bool active = model >= 27;
        const bool hot = (hover.type == Hit::PAD) || (grab.type == Hit::PAD);

        rr(vg, pad.x, pad.y, pad.w, pad.h, 6.0f * s);
        nvgFillColor(vg, rgbi(27, 27, 32));
        nvgFill(vg);

        const float px = clamp01(p.v[kCustomX] * 0.01f);
        const float py = clamp01(p.v[kCustomY] * 0.01f);
        const float hx = pad.x + pad.w * px;
        const float hy = pad.y + pad.h * (1.0f - py);

        // grid
        nvgStrokeWidth(vg, 1.0f);
        for (int i = 1; i < 8; ++i)
        {
            const float gx = pad.x + pad.w * i / 8.0f;
            const float gy = pad.y + pad.h * i / 8.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, gx, pad.y);
            nvgLineTo(vg, gx, pad.y + pad.h);
            nvgMoveTo(vg, pad.x, gy);
            nvgLineTo(vg, pad.x + pad.w, gy);
            nvgStrokeColor(vg, i == 4 ? rgbi(58, 58, 66) : rgbi(44, 44, 52));
            nvgStroke(vg);
        }

        if (active)
        {
            nvgBeginPath(vg);
            nvgMoveTo(vg, pad.x, hy);
            nvgLineTo(vg, pad.x + pad.w, hy);
            nvgMoveTo(vg, hx, pad.y);
            nvgLineTo(vg, hx, pad.y + pad.h);
            nvgStrokeColor(vg, rgbai(201, 168, 106, 110));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgRect(vg, pad.x, pad.y, pad.w, pad.h);
            nvgFillPaint(vg, nvgRadialGradient(vg, hx, hy, 2.0f * s, 26.0f * s,
                                               rgbai(232, 205, 138, 70), rgbai(232, 205, 138, 0)));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgCircle(vg, hx, hy, 6.0f * s);
            nvgFillColor(vg, hot ? kBrassBright : kBrass);
            nvgFill(vg);
            nvgStrokeColor(vg, rgbi(242, 234, 214));
            nvgStrokeWidth(vg, 1.4f);
            nvgStroke(vg);
        }

        rr(vg, pad.x, pad.y, pad.w, pad.h, 6.0f * s);
        nvgStrokeColor(vg, active ? kCardBorder : rgbi(48, 48, 55));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        if (!active)
        {
            rr(vg, pad.x, pad.y, pad.w, pad.h, 6.0f * s);
            nvgFillColor(vg, rgbai(18, 18, 22, 200));
            nvgFill(vg);
            const char* const dimText[3] = { "custom", "models", "only" };
            for (int i = 0; i < 3; ++i)
                text(vg, pad.x + pad.w * 0.5f, pad.y + pad.h * 0.5f + (i - 1) * 14.0f * s,
                     dimText[i], 10.5f * s, kTextDim, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, false);
        }

        char buf[48];
        std::snprintf(buf, sizeof(buf), "X %d   Y %d",
                      (int)(p.v[kCustomX] + 0.5f), (int)(p.v[kCustomY] + 0.5f));
        text(vg, pad.x + pad.w * 0.5f, L.padReadout.y, buf, 12.0f * s,
             active ? rgbi(207, 201, 187) : kTextDim,
             NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false);

        const char* const hint[3] = { "Drag to shape", "the E-Custom and", "AC-Custom models." };
        for (int i = 0; i < 3; ++i)
            text(vg, L.customPanel.x + L.customPanel.w * 0.5f, L.padReadout.y + (26.0f + 14.0f * i) * s,
                 hint[i], 10.0f * s, kTextDim, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, false);
    }

    // ---- hover tooltip (last, so it sits on top) ----
    if (mouseX >= 0.0f && mouseY >= 0.0f && grab.type == Hit::NONE)
    {
        const char* tip = tooltipText(hover);
        if (tip != nullptr)
            drawTooltip(vg, w, h, s, mouseX, mouseY, tip);
    }
}

} // namespace ui
} // namespace pw
