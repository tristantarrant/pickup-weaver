// Plugin metadata for DPF. The identifiers follow the Dataforte convention and are
// permanent: hosts use them to find the plugin in saved sessions.
#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "Dataforte"
#define DISTRHO_PLUGIN_NAME    "Pickup Weaver"
#define DISTRHO_PLUGIN_URI     "http://dataforte.net/plugins/pickupweaver"
#define DISTRHO_PLUGIN_CLAP_ID "net.dataforte.pickupweaver"

#define DISTRHO_PLUGIN_BRAND_ID  Dfte
#define DISTRHO_PLUGIN_UNIQUE_ID pkWv

#define DISTRHO_PLUGIN_HAS_UI        1
#define DISTRHO_PLUGIN_IS_RT_SAFE    1
#define DISTRHO_PLUGIN_NUM_INPUTS    2
#define DISTRHO_PLUGIN_NUM_OUTPUTS   2

// GUI (NanoVG vector UI, scales with the desktop/host factor and user resizing)
// keep in sync with pw::ui::kBaseWidth/kBaseHeight in src/UIPainter.hpp
#define DISTRHO_UI_USE_NANOVG 1
#define DISTRHO_UI_DEFAULT_WIDTH  1180
#define DISTRHO_UI_DEFAULT_HEIGHT 640
#define DISTRHO_UI_USER_RESIZABLE 1

// LV2 modgui (MOD Devices / MOD Desktop / Zynthian): files in ../modgui,
// shipped into the bundle by plugin/CMakeLists.txt
#define DISTRHO_PLUGIN_USES_MODGUI 1
#define DISTRHO_PLUGIN_USES_CUSTOM_MODGUI 1

#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:SimulatorPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Fx|EQ|Stereo"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "audio-effect", "equalizer", "stereo"

// Local DPF fix (dpf-patches/0001-vst3-parameter-flush-and-controller-echo.patch): apply VST3 parameter
// changes delivered without audio, and make the controller reflect values set on it. Requires the
// thread-safe setParameterValue of PluginPickupWeaver.cpp. Harmless if the patch isn't applied.
#define DPF_VST3_PATCH_FLUSH_AND_CONTROLLER_ECHO 1

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
