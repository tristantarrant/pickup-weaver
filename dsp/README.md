# PickupWeaverDSP — Pickup Weaver signal processing core (framework-neutral; used by the DPF plugin in ../plugin)

| file | purpose |
|---|---|
| `PickupWeaverDSP.h` | the DSP core: header-only C++17, no framework dependency, SIMD engine + backend selection |
| `PickupWeaverSIMD.h` | SIMD layer: scalar / SSE2 / AVX / NEON vector types + vectorized sincos and 10^x |
| `PickupWeaverDSP_AVX.cpp` | optional: runtime-dispatched AVX engine for builds whose baseline is SSE2 |
| `CMakeLists.txt` | `pickup_weaver_dsp` library target (sets up the AVX dispatch file) + tools/tests |
| `PickupWeaverTables.h` | generated tables (27 pickup models, E/AC-Custom navigator); regenerate with `python3 gen_tables.py` |
| `pw_cli.cpp` | offline harness: renders a float32 file with parameter changes at block boundaries |
| `test_static.py` | compares against all 135 static impulse captures of the original |
| `test_steps.py` | replays the original's parameter-step recordings through the core |
| `tests/test_simd.py` | bit-identity across backends, builds, platforms, channel counts (+ deviation from the pre-SIMD core) |
| `tests/test_vmath.cpp` | accuracy (ulp) of the vector math, bit-hash per backend |
| `tests/bench.cpp` | CPU benchmark: pre-SIMD core vs every backend |
| `tests/setup_aarch64_toolchain.sh` | rootless ARM64 cross toolchain (clang + Ubuntu sysroot + lld) for NEON tests under qemu |

Build/test: `g++ -std=c++17 -O2 -o pw_cli pw_cli.cpp && g++ -std=c++17 -O2 -o tests/pw_cli_ref tests/pw_cli_ref.cpp && python3 tests/test_simd.py`.
`test_static.py` / `test_steps.py` compare against reference impulse-response captures (`ir_*.f32`); point
`PW_CAPTURE_DIR` at the folder that holds them. `PickupWeaverTables.h` is generated from `../data` by `gen_tables.py`.

## API
```cpp
pw::PickupWeaverDSP dsp;
dsp.prepare(sampleRate);                 // optional: smoothingSeconds (11.61 ms), faithfulQuirk (true), pw::Backend (Auto)
dsp.backendName();                       // "neon", "avx", "sse2" or "scalar"
dsp.setParameters(params);               // every block; plain units (see pw::Parameters)
dsp.setParameters(params, true);         // instant: no smoothing (first block after a host restored a state)
dsp.process(channelPtrs, numChannels, numSamples);   // in place, up to 8 channels
```

## Smoothing (Option B)
- Continuous knobs: linear ramp over a **fixed 11.61 ms** (the original's ramp at buffer size 512),
  in the original's domains (dB for gains, Hz for frequencies, knob position for Tone/Volume,
  fractional filter orders), coefficients redesigned every sample while ramping, Direct Form I filters.
- Pickup To (model switch): the inner EQ glides over **20 ms** (the original's model glide is a fixed
  ~20 ms, measured at buffer sizes 128 and 512).
- Switches: squared crossfades, 20 ms (Pickup/Body enable, Tone at 10, From selection), 10 ms (Bypass).
- Body Thickness: one-pole delay glide `delay += (1 - 10^(-20/SR))·(target - delay)` after the ramp.

## SIMD
Where the parallelism is (each channel's filter chain is strictly sequential in time):
- **Channels in vector lanes.** All channels share the coefficients, so a register holds one sample of
  several channels: SSE2 / NEON = 2 doubles (stereo = one register), AVX = 4. Double precision is kept
  (as the original).
- **Batched transcendentals.** While parameters ramp, coefficients are redesigned every sample; their
  sin/cos/10^x are queued and evaluated W at a time with vectorized polynomials (max 1.6 ulp), and only
  for the stages whose parameters moved.
- **Shorter dependency chains.** The biquad's state part is summed independently of the incoming sample
  and the denormal flush is applied to the stored state, so the cascade's critical path is
  one multiply + one add per stage. For stereo this latency, not arithmetic, is the limit.

Backends and selection (`pw::Backend::Auto`):
| platform | backend | notes |
|---|---|---|
| ARM64 (Apple Silicon, Windows/Linux ARM64) | NEON | always available |
| x86-64, AVX present | AVX | compiled-in when the TU has AVX enabled, or via `PickupWeaverDSP_AVX.cpp` (CPU + OS support checked at `prepare()`) |
| x86-64 baseline | SSE2 | always available |
| other (incl. 32-bit ARM: NEON has no doubles there) | scalar | |

Runtime dispatch (plugin built for baseline x86-64, AVX used when present): compile `PickupWeaverDSP_AVX.cpp`
with `-mavx` (MSVC `/arch:AVX`) and define `PW_ENABLE_AVX_DISPATCH` for the code including
`PickupWeaverDSP.h`. `CMakeLists.txt` does both (`target_link_libraries(MyPlugin PRIVATE pickup_weaver_dsp)`; the DPF plugin uses this).
Everything inline in the AVX file lives in `pw::arch_avx`, so its AVX-encoded inline functions can't
replace the baseline copies at link time. Don't build that file with `-march=native`/`-mavx2`.

Fast-math robustness: the headers switch fast-math off locally (GCC `optimize("no-fast-math,...")`,
clang/MSVC `float_control(precise)`) and pin the range reductions with value barriers (empty asm), and
rounding uses rounding instructions rather than the `+1.5*2^52` trick. Built inside a `-ffast-math`
project (e.g. DPF's Makefiles) the math stays accurate (≤ 5.4 ulp measured); bit-identity across
compilers is then no longer guaranteed (within one compiler, backends still agree).

Determinism: every backend, compiler and platform produces **bit-identical** output. The audio path uses
no libm (own sincos/10^x) and the header disables FMA contraction locally (`clang fp contract(off)`,
`GCC optimize("fp-contract=off")`, MSVC `fp_contract(off)`); an FMA-capable build therefore gives the
same bits as a plain one.

Measured CPU (% of one core, 44.1 kHz, block 512, Ryzen 9 9900X):
| build | engine | 2 ch static | 2 ch automation | 8 ch static | 8 ch automation |
|---|---|---|---|---|---|
| g++ -O2, x86-64 baseline | pre-SIMD scalar | 0.30 | 0.97 | 1.07 | 1.71 |
| g++ -O2, x86-64 baseline | sse2 | 0.12 | 0.63 | 0.38 | 0.91 |
| g++ baseline + AVX dispatch | avx | 0.12 | 0.56 | 0.20 | 0.66 |
| clang++ -O2, x86-64 baseline | sse2 | 0.11 | 0.54 | 0.34 | 0.77 |
| clang++ -O2, x86-64-v3 | avx | 0.12 | 0.47 | 0.21 | 0.55 |
"Automation" = several knobs moving every block plus periodic model switches (redesign every sample).
Stereo gains come from the shorter chains and the batched math; AVX adds most for > 2 channels.
NEON is verified for correctness under qemu only (no ARM hardware here, so no ARM timings).

## Verification against the original plugin
- Static: 135 captures (all models, Custom X/Y grid, random full-chain settings): median 4e-7,
  worst 1.8e-4 max complex relative error.
- Knob steps at buffer size 512 (where Option B's ramp equals the original's): Gain/Volume/
  Brightness/Body Amount/Body Thickness/Body Phase exact to float precision, Amount 4e-5, Tone 2e-3.
- Pickup/Body enable and Bypass fades: same envelope and HF content as the original (residual ~1e-2).
- Model switches (Pickup To, From): smooth with the same duration and no click; the sample-level
  waveform differs because the original's internal switching mechanism (coefficients jump, filter
  state handled by an undetermined fixed-time process) isn't replicated.
- At other buffer sizes the original's knob ramp equals the buffer length; Option B deliberately doesn't.
- The SIMD engine is bit-identical to the pre-SIMD scalar core for static settings and within 3e-8 of
  peak (below float32 resolution) under automation; all plugin-comparison numbers above are unchanged.

## Notes
- `faithfulQuirk = true` reproduces the original's stale 1st-order Butterworth coefficients for
  fractional even-ceiling orders (seeded from the Single Hot model like a fresh instance).
- The navigator triangle search is compiled with strict IEEE ordering (`fp-contract=off`): on exact
  vertices the original's result depends on rounding (and falls back to its default row at
  AC-Custom X = 100, Y = 0 / 100). Verified identical to the reference on a 401×401 grid with
  g++ -O2, clang++ -O2 and g++ -O3 -march=native.
- With exact digital silence the original truncates some tails at a block boundary after a model
  switch (silence/tail logic). Not replicated; irrelevant with real input.
