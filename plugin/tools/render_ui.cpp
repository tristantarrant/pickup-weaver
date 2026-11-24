// render_ui.cpp — offscreen renderer for the Pickup Weaver GUI.
// Renders the shared painter into a GLX pbuffer (no visible window) and writes PPM,
// so GUI screenshots and modgui assets can be generated headlessly (xvfb-run).
//
// Usage:
//   render_ui ui    <out.ppm> <width> <height> [sym=value ...]
//   render_ui knobs <out.ppm> <frames> <size>     (sprite strip on magenta)
//   render_ui art   <out.ppm> <family> <size> [bright]   (on magenta)
//   render_ui word  <out.ppm> <width> <height>           (logo wordmark test)
//
// Magenta (255,0,255) backgrounds are keyed out by tools/render.py.

#include "UIPainter.hpp"
#include "PluginParams.hpp"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xlib.h>
#include <GL/glx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define NANOVG_GL2_IMPLEMENTATION
#include "nanovg/nanovg.h"
extern "C" {
#include "nanovg/nanovg_gl.h"
#include "nanovg/nanovg.c"
}

using namespace pw::plugin;

namespace {

bool writePPM(const char* path, const unsigned char* rgb, int w, int h)
{
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    // GL origin is bottom-left; flip rows while writing
    std::vector<unsigned char> row((size_t)w * 3);
    for (int y = h - 1; y >= 0; --y)
    {
        std::memcpy(row.data(), rgb + (size_t)y * w * 3, row.size());
        if (std::fwrite(row.data(), 1, row.size(), f) != row.size())
        {
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

bool readPixels(std::vector<unsigned char>& buf, int w, int h)
{
    buf.resize((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);   // rows are tightly packed in the PPM
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf.data());
    return glGetError() == GL_NO_ERROR;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s ui <out.ppm> <w> <h> [sym=value ...] | knobs <out.ppm> <frames> <size> | art <out.ppm> <family> <size> [bright]\n", argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    const char* out = argv[2];

    // pbuffer dimensions and mode parameters
    int pw = 0, ph = 0;
    int frames = 0, size = 0, family = 0;
    bool bright = false;

    if (mode == "ui")
    {
        if (argc < 5) { std::fprintf(stderr, "ui mode needs <w> <h>\n"); return 1; }
        pw = std::atoi(argv[3]);
        ph = std::atoi(argv[4]);
    }
    else if (mode == "knobs")
    {
        if (argc < 5) { std::fprintf(stderr, "knobs mode needs <frames> <size>\n"); return 1; }
        frames = std::atoi(argv[3]);
        size = std::atoi(argv[4]);
        pw = ph = size;
    }
    else if (mode == "word")
    {
        if (argc < 5) { std::fprintf(stderr, "word mode needs <w> <h>\n"); return 1; }
        pw = std::atoi(argv[3]);
        ph = std::atoi(argv[4]);
    }
    else if (mode == "art")
    {
        if (argc < 5) { std::fprintf(stderr, "art mode needs <family> <size> [bright]\n"); return 1; }
        family = std::atoi(argv[3]);
        size = std::atoi(argv[4]);
        bright = argc >= 6 && std::strcmp(argv[5], "bright") == 0;
        pw = ph = size;
    }
    else
    {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 1;
    }
    const int w = pw, h = ph;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
    {
        std::fprintf(stderr, "bad size %dx%d\n", w, h);
        return 1;
    }
    if (mode == "knobs" && (frames < 2 || frames > 256))
    {
        std::fprintf(stderr, "bad frame count %d\n", frames);
        return 1;
    }

    Display* const dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr)
    {
        std::fprintf(stderr, "cannot open X display (run under xvfb-run)\n");
        return 1;
    }

    const int fbAttribs[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_STENCIL_SIZE, 8,
        GLX_DOUBLEBUFFER, False,
        None
    };
    int nc = 0;
    GLXFBConfig* const cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), fbAttribs, &nc);
    if (cfgs == nullptr || nc < 1)
    {
        std::fprintf(stderr, "no GLX fbconfig with pbuffer support\n");
        return 1;
    }
    // pick a config whose color depth matches the display (XPutImage BadMatch otherwise)
    const int wantDepth = DefaultDepth(dpy, DefaultScreen(dpy));
    GLXFBConfig cfg = cfgs[0];
    for (int i = 0; i < nc; ++i)
    {
        int depth = 0;
        if (glXGetFBConfigAttrib(dpy, cfgs[i], GLX_BUFFER_SIZE, &depth) == Success
            && depth == wantDepth)
        {
            cfg = cfgs[i];
            break;
        }
    }

    const int pbAttribs[] = { GLX_PBUFFER_WIDTH, w, GLX_PBUFFER_HEIGHT, h, None };
    GLXPbuffer pbuf = glXCreatePbuffer(dpy, cfg, pbAttribs);
    GLXContext ctx = glXCreateNewContext(dpy, cfg, GLX_RGBA_TYPE, nullptr, True);
    if (pbuf == None || ctx == nullptr || !glXMakeContextCurrent(dpy, pbuf, pbuf, ctx))
    {
        std::fprintf(stderr, "cannot make GLX pbuffer context current\n");
        return 1;
    }

    NVGcontext* const vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (vg == nullptr)
    {
        std::fprintf(stderr, "cannot create NanoVG context\n");
        return 1;
    }

    pw::ui::Painter painter;
    if (!painter.init(vg))
    {
        std::fprintf(stderr, "cannot load embedded fonts\n");
        return 1;
    }

    bool ok = false;

    if (mode == "ui")
    {
        pw::ui::Params p;
        p.setDefaults();
        for (int i = 5; i < argc; ++i)
        {
            char* const eq = std::strchr(argv[i], '=');
            if (eq == nullptr) continue;
            *eq = 0;
            const char* const sym = argv[i];
            const float value = (float)std::atof(eq + 1);
            for (uint32_t k = 0; k < kParamCount; ++k)
            {
                if (std::strcmp(kSpecs[k].symbol, sym) == 0)
                {
                    p.v[k] = value;
                    break;
                }
            }
        }
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, (float)w, (float)h, 1.0f);
        painter.paint(vg, (float)w, (float)h, p, pw::ui::Hit(), pw::ui::Hit());
        nvgEndFrame(vg);
        glFinish();

        std::vector<unsigned char> buf;
        if (readPixels(buf, w, h))
            ok = writePPM(out, buf.data(), w, h);
    }
    else if (mode == "knobs")
    {
        std::vector<unsigned char> strip((size_t)frames * size * size * 3);
        std::vector<unsigned char> buf;
        for (int i = 0; i < frames; ++i)
        {
            glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            nvgBeginFrame(vg, (float)size, (float)size, 1.0f);
            painter.paintKnobSprite(vg, (float)size, (float)i / (float)(frames - 1));
            nvgEndFrame(vg);
            glFinish();
            if (!readPixels(buf, size, size)) { ok = false; break; }
            for (int y = 0; y < size; ++y)
                std::memcpy(&strip[(size_t)y * frames * size * 3 + (size_t)i * size * 3],
                            &buf[(size_t)y * size * 3], (size_t)size * 3);
            ok = true;
        }
        if (ok)
            ok = writePPM(out, strip.data(), frames * size, size);
    }
    else if (mode == "word")
    {
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, (float)w, (float)h, 1.0f);
        nvgFontFace(vg, "logo");
        nvgFontSize(vg, h * 0.7f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGB(232, 205, 138));
        nvgText(vg, w * 0.5f, h * 0.5f, "dataforte", nullptr);
        nvgEndFrame(vg);
        glFinish();
        std::vector<unsigned char> buf;
        if (readPixels(buf, w, h))
            ok = writePPM(out, buf.data(), w, h);
    }
    else // art
    {
        const float m = size * 0.06f;
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, (float)size, (float)size, 1.0f);
        painter.paintPickupArt(vg, m, m, (float)size - 2.0f * m, (float)size - 2.0f * m, family, bright);
        nvgEndFrame(vg);
        glFinish();

        std::vector<unsigned char> buf;
        if (readPixels(buf, size, size))
            ok = writePPM(out, buf.data(), size, size);
    }

    nvgDeleteGL2(vg);
    glXMakeContextCurrent(dpy, None, None, nullptr);
    glXDestroyPbuffer(dpy, pbuf);
    glXDestroyContext(dpy, ctx);
    XFree(cfgs);
    XCloseDisplay(dpy);

    if (!ok)
    {
        std::fprintf(stderr, "render failed\n");
        return 1;
    }
    std::printf("wrote %s\n", out);
    return 0;
}
