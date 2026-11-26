// PluginPickupWeaverUI.cpp — DPF UI for Pickup Weaver.
// Rendering and geometry live in UIPainter (shared with the offscreen renderer);
// this file only wires mouse/scroll events and parameter values to the host.

#include "DistrhoUI.hpp"
#include "UIPainter.hpp"
#include "PluginParams.hpp"

#include <cmath>
#include <cstring>

using namespace pw::plugin;
using pw::ui::Hit;
using pw::ui::Layout;
using pw::ui::RectF;

START_NAMESPACE_DISTRHO

class PickupWeaverUI : public UI
{
public:
    PickupWeaverUI()
        : UI(pw::ui::kBaseWidth, pw::ui::kBaseHeight, true),
          fHover(),
          fGrab(),
          fGrabY(0.0f),
          fGrabValue(0.0f),
          fLastClickTime(0),
          fLastClick(),
          fMouseX(-1.0f),
          fMouseY(-1.0f)
    {
        fParams.setDefaults();
        fPainter.init(getContext());
    }

    ~PickupWeaverUI() override
    {
        fPainter.destroy(getContext());
    }

protected:
    void onNanoDisplay() override
    {
        fPainter.paint(getContext(), (float)getWidth(), (float)getHeight(), fParams, fHover, fGrab,
                       fMouseX, fMouseY);
    }

    void parameterChanged(uint32_t index, float value) override
    {
        if (index < kParamCount)
        {
            fParams.v[index] = value;
            repaint();
        }
    }

    bool onMouse(const MouseEvent& ev) override
    {
        if (ev.button != kMouseButtonLeft)
            return false;

        float x = (float)ev.pos.getX();
        float y = (float)ev.pos.getY();
        toUiCoords(x, y);
        fMouseX = x;
        fMouseY = y;
        const Layout L = pw::ui::makeLayout((float)getWidth(), (float)getHeight());

        if (ev.press)
        {
            const Hit h = pw::ui::hitTest(L, x, y);
            fHover = h;

            // double-click on a knob resets it to the default value
            if (h.type == Hit::KNOB && fLastClick == h && ev.time - fLastClickTime < 400)
            {
                setParam((uint32_t)h.index, kSpecs[h.index].def, true);
                fLastClick = Hit();
                return true;
            }
            if (h.type != Hit::NONE)
            {
                fLastClick = h;
                fLastClickTime = ev.time;
            }

            switch (h.type)
            {
            case Hit::KNOB:
                fGrab = h;
                fGrabY = y;
                fGrabValue = fParams.v[h.index];
                editParameter((uint32_t)h.index, true);
                break;
            case Hit::PAD:
                fGrab = h;
                editParameter(kCustomX, true);
                editParameter(kCustomY, true);
                setPadFromPos(L, x, y);
                break;
            case Hit::TILE:
                setParam(kPickupFrom, (float)h.index, true);
                break;
            case Hit::ROW:
                setParam(kPickupTo, (float)h.index, true);
                break;
            case Hit::TOGGLE:
                setParam((uint32_t)h.index, fParams.v[h.index] > 0.5f ? 0.0f : 1.0f, true);
                break;
            case Hit::PREV:
            case Hit::NEXT:
            {
                int m = (int)(fParams.v[kPickupTo] + 0.5f) + (h.type == Hit::NEXT ? 1 : -1);
                setParam(kPickupTo, (float)(m < 0 ? 0 : (m > 28 ? 28 : m)), true);
                break;
            }
            case Hit::BYPASS:
                setParam(kBypass, fParams.v[kBypass] > 0.5f ? 0.0f : 1.0f, true);
                break;
            default:
                break;
            }
            repaint();
            return true;
        }

        // release
        if (fGrab.type != Hit::NONE)
        {
            if (fGrab.type == Hit::KNOB)
                editParameter((uint32_t)fGrab.index, false);
            else if (fGrab.type == Hit::PAD)
            {
                editParameter(kCustomX, false);
                editParameter(kCustomY, false);
            }
            fGrab = Hit();
            repaint();
        }
        return true;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        float x = (float)ev.pos.getX();
        float y = (float)ev.pos.getY();
        toUiCoords(x, y);
        fMouseX = x;
        fMouseY = y;
        const Layout L = pw::ui::makeLayout((float)getWidth(), (float)getHeight());

        if (fGrab.type == Hit::KNOB)
        {
            const ParamSpec& sp = kSpecs[fGrab.index];
            const float rate = (ev.mod & kModifierShift) ? 1500.0f : 150.0f;
            const float v = fGrabValue + (fGrabY - y) * (sp.max - sp.min) / (rate * L.s);
            setParam((uint32_t)fGrab.index, v);
            return true;
        }
        if (fGrab.type == Hit::PAD)
        {
            setPadFromPos(L, x, y);
            return true;
        }

        const Hit h = pw::ui::hitTest(L, x, y);
        if (h != fHover || pw::ui::tooltipText(h) != nullptr)
        {
            fHover = h;
            repaint();
        }
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        float x = (float)ev.pos.getX();
        float y = (float)ev.pos.getY();
        toUiCoords(x, y);
        fMouseX = x;
        fMouseY = y;
        const Layout L = pw::ui::makeLayout((float)getWidth(), (float)getHeight());
        const Hit h = pw::ui::hitTest(L, x, y);
        const float d = (float)ev.delta.getY();

        if (h.type == Hit::KNOB)
        {
            const ParamSpec& sp = kSpecs[h.index];
            const float step = (sp.max - sp.min) * ((ev.mod & kModifierShift) ? 0.004f : 0.03f);
            setParam((uint32_t)h.index, fParams.v[h.index] + (d > 0.0f ? step : (d < 0.0f ? -step : 0.0f)), true);
            return true;
        }
        if (h.type == Hit::ROW && d != 0.0f)
        {
            int m = (int)(fParams.v[kPickupTo] + 0.5f) + (d > 0.0f ? 1 : -1);
            setParam(kPickupTo, (float)(m < 0 ? 0 : (m > 28 ? 28 : m)), true);
            return true;
        }
        return false;
    }

private:
    // Mouse events arrive in window pixels while DGL's auto-scaling keeps the
    // painter in logical units, so convert before hit-testing (no-op at 1:1).
    void toUiCoords(float& x, float& y) const
    {
        const uint windowW = getWindow().getWidth();
        const uint windowH = getWindow().getHeight();
        if (windowW > 0 && windowH > 0)
        {
            x *= (float)getWidth() / (float)windowW;
            y *= (float)getHeight() / (float)windowH;
        }
    }

    void setParam(uint32_t index, float value, bool gesture = false)
    {
        const ParamSpec& sp = kSpecs[index];
        if (sp.isBool)
            value = value > 0.5f ? 1.0f : 0.0f;
        else
        {
            if (value < sp.min) value = sp.min;
            if (value > sp.max) value = sp.max;
            if (sp.isInt) value = (float)(int)(value + (value >= 0.0f ? 0.5f : -0.5f));
        }
        if (value == fParams.v[index])
            return;
        fParams.v[index] = value;
        if (gesture)
            editParameter(index, true);
        setParameterValue(index, value);   // push to the plugin
        if (gesture)
            editParameter(index, false);
        repaint();
    }

    void setPadFromPos(const Layout& L, float x, float y)
    {
        const RectF& pad = L.pad;
        float fx = (x - pad.x) / pad.w;
        float fy = 1.0f - (y - pad.y) / pad.h;
        if (fx < 0.0f) fx = 0.0f;
        if (fx > 1.0f) fx = 1.0f;
        if (fy < 0.0f) fy = 0.0f;
        if (fy > 1.0f) fy = 1.0f;
        setParam(kCustomX, fx * 100.0f);
        setParam(kCustomY, fy * 100.0f);
    }

    pw::ui::Painter fPainter;
    pw::ui::Params fParams;
    Hit fHover;
    Hit fGrab;
    float fGrabY;
    float fGrabValue;
    uint32_t fLastClickTime;
    Hit fLastClick;
    float fMouseX;
    float fMouseY;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PickupWeaverUI)
};

UI* createUI()
{
    return new PickupWeaverUI();
}

END_NAMESPACE_DISTRHO
