// UIPainter.hpp — all drawing and interaction geometry for the Pickup Weaver GUI.
// Pure NanoVG, no DPF types, so the same code is shared by the plugin UI
// (desktop formats) and tools/render_ui.cpp (offscreen rendering, modgui assets).
#pragma once

#include <cstddef>
#include <cstdint>

struct NVGcontext;

namespace pw {
namespace ui {

// Base design size; the window keeps this aspect ratio and everything scales
// relative to the current width.
static const uint32_t kBaseWidth = 1180;
static const uint32_t kBaseHeight = 640;

struct Params
{
    float v[15];   // indexed by pw::plugin::ParamId

    void setDefaults();
};

struct RectF
{
    float x = 0, y = 0, w = 0, h = 0;

    bool contains(float px, float py) const noexcept
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// What is under the mouse / being dragged.
struct Hit
{
    enum Type : uint8_t
    {
        NONE = 0,
        KNOB,           // index = param id
        TILE,           // index = source pickup type 0..3
        ROW,            // index = target model 0..28
        TOGGLE,         // index = param id
        PAD,            // custom X/Y pad
        PREV,           // previous model button
        NEXT,           // next model button
        BYPASS,         // bypass button
        PICKUP_ENABLE   // pickup simulation on/off pill
    };

    uint8_t type = NONE;
    int index = -1;

    bool operator==(const Hit& o) const noexcept { return type == o.type && index == o.index; }
    bool operator!=(const Hit& o) const noexcept { return !(*this == o); }
};

struct Layout
{
    float s = 1;        // scale relative to the base design
    RectF header, bypassBtn;
    RectF srcPanel, tiles[4], pickupEnablePill, pickupEnableLabel;
    RectF tgtPanel, art, groupLbl, nameLbl, prevBtn, nextBtn, listArea;
    float listRowH = 20, listColW = 198;
    RectF ctlPanel, knobCell[7];
    RectF togglePill[2], toggleLabel[2];
    RectF customPanel, pad, padReadout;
};

Layout makeLayout(float w, float h);
Hit hitTest(const Layout& l, float x, float y);

// Formats a parameter value for display (knob readouts etc.).
void valueText(int param, float value, char* buf, size_t cap);

// Pickup art family for a target model index (also used for the source tiles).
// 0 single coil, 1 P90, 2 humbucker, 3 piezo, 4 acoustic body, 5 custom.
int familyOfModel(int model);

class Painter
{
public:
    // Loads fonts into the given NanoVG context. Must be called once per context.
    bool init(NVGcontext* vg);
    void destroy(NVGcontext* vg);

    void paint(NVGcontext* vg, float w, float h, const Params& p,
               const Hit& hover, const Hit& grab) const;

    // Standalone pieces used by the offscreen renderer for modgui assets.
    void paintKnobSprite(NVGcontext* vg, float size, float value01) const;
    void paintPickupArt(NVGcontext* vg, float x, float y, float w, float h,
                        int family, bool bright) const;

private:
    int fFontRegular = -1;
    int fFontBold = -1;
    int fFontLogo = -1;
};

} // namespace ui
} // namespace pw
