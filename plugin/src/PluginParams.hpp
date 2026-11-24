// PluginParams.hpp — parameter definitions shared by the plugin, the UI and the GUI painter.
// Deliberately free of DPF types so the painter (and the offscreen renderer) can use it too.
#pragma once

#include <cstdint>

namespace pw {
namespace plugin {

enum ParamId : uint32_t
{
    kBypass = 0,
    kPickupEnabled,
    kPickupFrom,
    kPickupTo,
    kAmount,
    kCustomX,
    kCustomY,
    kBodyEnabled,
    kBodyThickness,
    kBodyAmount,
    kBrightness,
    kGain,
    kVolume,
    kTone,
    kBodyPhase,
    kParamCount
};

struct ParamSpec
{
    const char* name;
    const char* symbol;
    const char* unit;
    float min, max, def;
    bool isBool;
    bool isInt;
};

inline const ParamSpec kSpecs[kParamCount] = {
    { "Bypass",          "dpf_bypass",      "",   0.f,    1.f,    0.f,   true,  true  },
    { "Pickup Enabled",  "pickup_enabled",  "",   0.f,    1.f,    1.f,   true,  true  },
    { "Pickup From",     "pickup_from",     "",   0.f,    3.f,    1.f,   false, true  },
    { "Pickup To",       "pickup_to",       "",   0.f,    28.f,   0.f,   false, true  },
    { "Amount",          "amount",          "%",  0.f,    100.f,  100.f, false, false },
    { "Custom X",        "custom_x",        "%",  0.f,    100.f,  50.f,  false, false },
    { "Custom Y",        "custom_y",        "%",  0.f,    100.f,  50.f,  false, false },
    { "Body Enabled",    "body_enabled",    "",   0.f,    1.f,    1.f,   true,  true  },
    { "Body Thickness",  "body_thickness",  "mm", 10.f,   100.f,  10.f,  false, false },
    { "Body Amount",     "body_amount",     "%",  0.f,    100.f,  0.f,   false, false },
    { "Brightness",      "brightness",      "%",  -100.f, 100.f,  0.f,   false, false },
    { "Gain",            "gain",            "dB", -20.f,  20.f,   0.f,   false, false },
    { "Volume",          "volume",          "",   0.f,    10.f,   10.f,  false, false },
    { "Tone",            "tone",            "",   0.f,    10.f,   10.f,  false, false },
    { "Body Phase",      "body_phase",      "",   0.f,    1.f,    0.f,   true,  true  },
};

inline const char* const kFromNames[4] = { "Humbucker", "Single Coil", "Single Coil (Bright)", "Piezo" };

} // namespace plugin
} // namespace pw
