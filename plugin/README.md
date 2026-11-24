# Pickup Weaver — DPF plugin (LV2, VST2, VST3, CLAP), by Dataforte

Thin [DPF](https://github.com/DISTRHO/DPF) wrapper around the framework-neutral DSP core in
`../dsp` (SIMD: SSE2 / AVX runtime dispatch / NEON). Stereo, 15 parameters, with a vector GUI on
the desktop formats and a MOD GUI for MOD Devices / MOD Desktop / Zynthian. Identifiers are the
Dataforte convention: `http://dataforte.net/plugins/pickupweaver`,
`net.dataforte.pickupweaver`, brand `Dataforte`; licence GPL-2.0-or-later
(`src/DistrhoPluginInfo.h`, `getLicense()` in `src/PluginPickupWeaver.cpp`).

## Layout
| path | |
|---|---|
| `src/DistrhoPluginInfo.h` | plugin metadata (stereo in/out, categories, IDs, UI/modgui switches) |
| `src/PluginParams.hpp` | parameter table shared by plugin, UI and painter |
| `src/PluginPickupWeaver.cpp` | the plugin: 15 parameters (original ranges/defaults), thread-safe values, DSP calls |
| `src/PluginPickupWeaverUI.cpp` | desktop UI: event handling, parameter sync (drawing lives in `UIPainter`) |
| `src/UIPainter.{hpp,cpp}` | all GUI drawing + layout + hit-testing, pure NanoVG (also used by the render tool) |
| `src/UIAssets.{hpp,cpp}` | generated: subsetted fonts (DejaVu text faces + the wordmark face) embedded in the binaries |
| `assets/` | the dataforte wordmark font (Bitcount Grid Double, OFL — instanced, subsetted, renamed) and its license |
| `modgui/` | MOD GUI: `modgui.ttl`, `icon.html` (generated), `stylesheet.css`, sprites, screenshot/thumbnail |
| `tools/` | asset and screenshot tooling (see below) |
| `CMakeLists.txt` | builds DPF + `pickup_weaver_dsp` + the plugin via `dpf_add_plugin(... TARGETS lv2 vst2 vst3 clap)` |
| `dpf-patches/` | local DPF fix for VST3 (see below) |
| `tests/clap_host.cpp`, `tests/lv2_host.cpp` | minimal test hosts (CLAP; LV2 via lilv) |
| `tests/test_plugin.py` | builds everything, renders scenarios through CLAP and LV2, requires bit-identity with `pw_cli` |

## Build
```sh
make            # git submodule update --init --recursive + cmake configure/build
# -> plugin/build/bin/pickup_weaver.lv2/  pickup_weaver.vst3/  pickup_weaver.clap  pickup_weaver-vst2.so
```

The equivalent by hand (DPF is the `dpf/` submodule, pinned at 4238e1c):

```sh
git submodule update --init --recursive
cmake -S plugin -B plugin/build -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/build -j
```

CMake applies `dpf-patches/0001-vst3-parameter-flush-and-controller-echo.patch` to the
submodule at configure time when it is not already present, so `dpf/` stays a plain
checkout apart from that one local edit.
Release builds contain no absolute source paths (`-ffile-prefix-map`) and are stripped (`-s`).
Install the LV2 bundle (DSP + UI + modgui) with `cp -r plugin/build/bin/pickup_weaver.lv2 ~/.lv2/`.
For a release that must run on every x86-64 CPU, build with the baseline target (`-march=x86-64`;
some distributions' compilers default to x86-64-v3). AVX is still used at runtime when present.
Use DPF's CMake route: DPF's Makefiles compile with `-ffast-math`. The core protects its own math
against that (strict-FP pragmas + value barriers; stays accurate), but bit-identity across
compilers/backends is only guaranteed without fast-math.

Requires X11 + OpenGL development headers (DPF's DGL/NanoVG UI). The LV2 DSP binary itself does not
link X11, so it still loads on MOD hardware and other headless systems; the modgui is pure web files.

## Desktop GUI
- NanoVG vector UI, no raster assets: scales crisply at any size. Follows the host/desktop HiDPI
  factor automatically and supports window resizing with a locked 1180×640 aspect ratio.
- Layout: `dataforte` wordmark + bypass; **Source** tiles for the four input pickup types;
  **Target** model display with pickup artwork, a grouped list of all 29 models and ◀ ▶ stepping;
  knobs (Mix, Gain, Brightness, Body Mix, Thickness, Volume, Tone) and Body Enable/Phase toggles;
  a Custom X/Y pad for the two custom models.
- Interaction: drag knobs vertically (Shift = fine), wheel steps, double-click resets a knob;
  the wheel over the model list steps through models. Click-type changes bracket
  `editParameter`, so hosts record them as automation.

## MOD GUI (modgui)
Pure HTML/CSS inside the LV2 bundle (`modgui/`), wired via DPF's `DISTRHO_PLUGIN_USES_MODGUI`
(`rdfs:seeAlso <modgui.ttl>` in the manifest). The pedal face has dataforte branding, the pickup
artwork, native `<select>` dropdowns for source and target model, seven knobs and a bypass
footswitch with a status light; the remaining parameters (custom X/Y, enable/phase switches) stay
in MOD's built-in settings panel, which is generated from the plugin description.
Knob sprites are rendered by the same painter as the desktop GUI.

## Tools
| tool | |
|---|---|
| `tools/render.py` | offscreen screenshots + sprites: `render.py ui out.png [W H] [sym=value ...]`, `render.py knobs out.png [frames] [size]`, `render.py art <family> <out.png>` (runs `pw_render_ui` under Xvfb) |
| `tools/embed_assets.py` | regenerates `src/UIAssets.{hpp,cpp}` (fonts subsetted with fontTools) |
| `tools/build_wordmark_font.py` | builds `assets/dataforte-wordmark.ttf` from the Bitcount Grid Double variable font: weight 400 instance, subsetted to the wordmark letters, composites flattened (NanoVG's stb_truetype cannot draw them) and the family renamed |
| `tools/gen_modgui_html.py` | regenerates `modgui/icon.html` from `../data/pickup_models.csv` |
| `tools/build_modgui_assets.py` | regenerates `modgui/knobs.png`, `background.png`, `screenshot.png`, `thumbnail.png` |

After changing GUI code, `python3 tools/render.py ui /tmp/shot.png` gives a headless screenshot;
after changing models or the pedal layout, rerun the three generators.

## Plugin behavior
- Parameters: the original's 15 controls in plain units. Bypass uses DPF's bypass designation
  (LV2 `lv2:enabled`, VST3 `kIsBypass`, CLAP `CLAP_PARAM_IS_BYPASS`); the DSP crossfades it (10 ms).
- Thread safety: `setParameterValue` may be called off the audio thread (VST2, VST3 controller):
  values are atomics; `run()` pushes them to the DSP only when something changed.
- Restored state: LV2 and VST3 hosts deliver saved values with the first block after activation.
  That block is applied without smoothing (`setParameters(p, instant=true)`), so a session doesn't
  start with a glide from the default settings.
- Realtime: allocation only in the constructor, `activate()` and `sampleRateChanged()`.
- Automation is block-rate (DPF applies parameter events at block starts; same as the original).

## DPF patch (VST3)
`dpf-patches/0001-vst3-parameter-flush-and-controller-echo.patch`, applied to the `dpf/`
submodule by CMake and enabled per plugin by
`#define DPF_VST3_PATCH_FLUSH_AND_CONTROLLER_ECHO 1` (in `DistrhoPluginInfo.h`), fixes three things in
DPF's VST3 wrapper (also present on DPF `develop`):
1. parameter changes sent with a zero-length `process()` ("flush", e.g. Cubase while stopped) were dropped;
2. the edit controller didn't reflect `setParamNormalized` until the processor had seen the value
   (fixed by applying it to the plugin, which is why the plugin's `setParameterValue` must be thread-safe);
3. a legal null output-parameter list logged an assertion on every block.
Without the patch the plugin still builds and works, but Steinberg's validator fails its
"Parameter Bypass persistence" test. Worth offering upstream.

## Verification (this machine, x86-64 Linux)
- CLAP and LV2: `tests/test_plugin.py` renders 5 scenarios (defaults, complex static setting, bypass,
  dense automation of every parameter, model/switch changes) at block sizes 512 and 64:
  **bit-identical** to the standalone core (`pw_cli`) in all 20 renders.
- VST3: Steinberg validator (VST3 SDK, built as a test tool only), extensive mode: **537/537 passed**
  (with the GUI compiled in).
- GUI: rendered headlessly through the same painter (`tools/render.py`) at 1180×640, 2360×1280 and
  590×320; the modgui pedal is screenshotted in headless Chrome.
- Carla (`carla-discovery-native`) loads LV2, VST2 and VST3: stereo, 15 parameters.
- Exported symbols are the format entry points only.
Not tested: interactive use of the real UI window (headless box — verified by rendering the identical
painter offscreen and by the validator), ARM64 plugin builds (the core's NEON path is verified under
qemu), macOS/Windows, AU, real DAWs. This Carla build has no CLAP support, so CLAP was tested with
the custom host only.

## Licensing notes
Pickup Weaver is GPL-2.0-or-later (see `../../LICENSE`).
DPF and the four chosen formats are permissive: DPF ISC, LV2 ISC, VST2 BSD-3 (Xaymar's clean-room
headers bundled with DPF), VST3 ISC (DPF's own "travesty" headers, no Steinberg SDK), CLAP MIT.
All require attribution (see `../dpf/LICENSING.md`). LADSPA/DSSI (LGPL headers) are not built.
The embedded GUI text fonts are subsetted DejaVu Sans (Bitstream Vera license, permissive).
The dataforte wordmark uses Bitcount Grid Double (SIL Open Font License 1.1, see
`assets/OFL-Bitcount.txt`); the modified (instanced/subsetted) copy is renamed per the license,
and the copyright/trademark notices are kept in the font.
