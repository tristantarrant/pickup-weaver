// ============================================================================
// PickupWeaverDSP.h -- Pickup Weaver signal processing (SIMD).
//
// Header-only C++17, no framework dependency. In a plugin: prepare() when activated / on sample-rate
// change, setParameters() at the start of every audio block, then process().
//
// Smoothing: fixed-time linear ramps (see README.md).
//
// SIMD (PickupWeaverSIMD.h):
//  * channels are processed in vector lanes (all channels share the coefficients):
//    SSE2 / NEON = 2 channels per register (stereo = one register), AVX = 4;
//  * coefficient redesign during ramps evaluates its transcendentals (sin/cos/10^x) in
//    vector batches, and only for the parameters that actually moved;
//  * every backend produces bit-identical output (no FMA, no libm in the audio path).
// Backend choice: PickupWeaverDSP::prepare(..., Backend::Auto) picks NEON on ARM64, AVX when
// available (compiled-in, or the optional runtime-dispatched PickupWeaverDSP_AVX.cpp), else SSE2.
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "PickupWeaverTables.h"
#include "PickupWeaverSIMD.h"
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

// PW_FP_STRICT_BEGIN / PW_FP_STRICT_END (strict IEEE region) are defined in PickupWeaverSIMD.h.

namespace pw {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDefaultSmoothingSeconds = 512.0 / 44100.0;   // 11.61 ms
inline constexpr double kSwitchFadeSeconds = 0.020;                    // enable/disable fades, model glide
inline constexpr double kBypassFadeSeconds = 0.010;
inline constexpr int kMaxChannels = 8;

// Parameters in PLAIN units (what the original displays). Plain aggregate, shared by all backends.
struct Parameters
{
    bool   bypass        = false;
    bool   pickupEnabled = true;
    int    pickupFrom    = 1;       // 0 Humbucker, 1 Single Coil, 2 Single Coil (Bright), 3 Piezo
    int    pickupTo      = 0;       // 0..26 built-in models, 27 E-Custom, 28 AC-Custom
    double amount        = 100.0;   // %
    double customX       = 50.0;    // %
    double customY       = 50.0;    // %
    bool   bodyEnabled   = true;
    double bodyThickness = 10.0;    // mm (10..100)
    double bodyAmount    = 0.0;     // %
    bool   bodyPhase     = false;
    double brightness    = 0.0;     // % (-100..100)
    double gainDb        = 0.0;     // dB (-20..20)
    double volume        = 10.0;    // 0..10
    double tone          = 10.0;    // 0..10 (10 = filter bypassed)
};

enum class Backend { Auto, Scalar, SSE2, AVX, NEON };

namespace detail {
struct EngineBase
{
    virtual ~EngineBase() = default;
    virtual void prepare(double sampleRate, double smoothingSeconds, bool faithfulQuirk, const Parameters& p) = 0;
    virtual void setParameters(const Parameters& p, bool instant) = 0;
    virtual void process(float* const* ch, int numChannels, int numSamples) = 0;
    virtual void reset() = 0;
    virtual const char* name() const = 0;
};
} // namespace detail

PW_FP_STRICT_BEGIN
namespace PW_ARCH_NS {

using namespace ::pw::PW_ARCH_NS::simd;

// ---------------------------------------------------------------------------
// Coefficients  y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2   (designs take precomputed sin/cos/10^x)
// ---------------------------------------------------------------------------
struct Coeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline Coeffs norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// A = 10^(gainDb/40), (s, c) = sincos(2*pi*f/sr)
inline Coeffs rbjPeak(double q, double A, double s, double c)
{
    const double al = s / (2 * q);
    return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

inline Coeffs rbjLowShelf(double A, double s, double c)      // S = 1 (FUN_180ad97d0)
{
    const double sa = 2 * std::sqrt(A) * s / std::sqrt(2.0);
    return norm(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}

inline Coeffs rbjHighShelf(double A, double s, double c)     // S = 1 (FUN_180ad8a10)
{
    const double sa = 2 * std::sqrt(A) * s / std::sqrt(2.0);
    return norm(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

inline Coeffs rbjLowPass(double q, double s, double c)
{
    const double al = s / (2 * q);
    return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

// k = tan(pi*f/sr)
inline Coeffs bilinearLowPass1(double k) { return norm(k, k, 0, 1 + k, k - 1, 0); }
inline Coeffs bilinearHighPass1(double k) { return norm(1, -1, 0, 1 + k, k - 1, 0); }

// First-order high shelf (FUN_180aca900): g = 10^(gainDb/20), s = sin(2*pi*f/sr)
inline Coeffs monoQuadHighShelf(double g, double s)
{
    const double k = s * (g + 1) * 0.25;
    return norm(g + k, -(g - k), 0, 1 + k, -(1 - k), 0);
}

// ---------------------------------------------------------------------------
// Batched transcendentals for one redesign step (evaluated W lanes at a time)
// ---------------------------------------------------------------------------
template <class V>
struct TransBatch
{
    static constexpr int kMax = 48;                    // + padding to a whole vector
    alignas(32) double pIn[kMax + 4], pOut[kMax + 4], sIn[kMax + 4], sOut[kMax + 4], cOut[kMax + 4];
    int np = 0, ns = 0;
    void clear() { np = ns = 0; }
    int pow10(double x) { pIn[np] = x; return np++; }
    int sincos(double x) { sIn[ns] = x; return ns++; }
    void runPow10() { batchPow10<V>(pIn, pOut, np); }
    void runSinCos() { batchSinCos<V>(sIn, sOut, cOut, ns); }
};

// ---------------------------------------------------------------------------
// Direct Form I state, vectorized over channel lanes
// ---------------------------------------------------------------------------
template <class V>
struct CoeffsV
{
    typename V::T b0, b1, b2, a1, a2;
    void set(const Coeffs& c) { b0 = V::set1(c.b0); b1 = V::set1(c.b1); b2 = V::set1(c.b2); a1 = V::set1(c.a1); a2 = V::set1(c.a2); }
};

template <class V>
struct DF1V
{
    using T = typename V::T;
    T x1 = V::zero(), x2 = V::zero(), y1 = V::zero(), y2 = V::zero();
    // Latency-optimized evaluation: only b0*x depends on the previous stage, so the state part is
    // summed independently (shorter critical path through a cascade). The denormal flush (as in the
    // original kernels) is applied to the stored feedback state, keeping it off the critical path.
    inline T process(const CoeffsV<V>& c, T x)
    {
        const T st = V::sub(V::add(V::mul(c.b1, x1), V::mul(c.b2, x2)), V::add(V::mul(c.a1, y1), V::mul(c.a2, y2)));
        const T y = V::add(V::mul(c.b0, x), st);
        x2 = x1; x1 = x; y2 = y1; y1 = V::flushSmall(y, V::set1(1e-300));
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = V::zero(); }
};

// y = w*S(x) + (1-w)*x  helpers
template <class V>
inline typename V::T blendIn(const typename V::T& processed, const typename V::T& x, double w)
{
    return V::add(processed, V::mul(V::set1(1.0 - w), x));
}

template <class V>
inline typename V::T xfade(const typename V::T& a, const typename V::T& b, double g)   // (1-g)*a + g*b
{
    return V::add(V::mul(V::set1(1.0 - g), a), V::mul(V::set1(g), b));
}

// ---------------------------------------------------------------------------
// Smoothing primitives (scalar; shared by all channels)
// ---------------------------------------------------------------------------
class LinearSmoother
{
public:
    void reset(double v) { cur = tgt = v; left = 0; step = 0; }
    void setTarget(double t, int len)
    {
        if (len <= 0) { cur = tgt = t; left = 0; return; }   // jump (also cancels a running ramp)
        if (t == tgt) return;
        tgt = t;
        step = (tgt - cur) / len;
        left = len;
    }
    bool active() const { return left > 0; }
    double value() const { return cur; }
    // returns true when the value changed
    bool advance()
    {
        if (left <= 0) return false;
        if (--left == 0) cur = tgt; else cur += step;
        return true;
    }

private:
    double cur = 0, tgt = 0, step = 0;
    int left = 0;
};

class SwitchFade   // position u moves linearly; processed weight = u^2
{
public:
    void reset(bool on) { u = on ? 1.0 : 0.0; target = u; }
    void set(bool on) { target = on ? 1.0 : 0.0; }
    void setLength(int samples) { inc = 1.0 / std::max(1, samples); }
    bool fullyOff() const { return u == 0.0 && target == 0.0; }
    double weight() const { return u * u; }
    void advance()
    {
        if (u < target) u = std::min(target, u + inc);
        else if (u > target) u = std::max(target, u - inc);
    }

private:
    double u = 1, target = 1, inc = 1.0 / 882;
};

// ---------------------------------------------------------------------------
// Fractional-order Butterworth (FUN_180ad6f20 / FUN_180ad7c20 / FUN_180ad5de0)
// ---------------------------------------------------------------------------
template <class V>
class FracButterworth
{
public:
    static constexpr int kMaxSections = 10;    // order <= 20
    using T = typename V::T;

    explicit FracButterworth(bool lowPass) : lp(lowPass) {}

    // Section sines sin((k+1/2)*pi/x) depend only on the order: recomputed only when it changes.
    // Returns the number of sincos requests queued (sections) so the caller can batch them.
    template <class B>
    void queue(double fc, double x, double sr, B& tb)
    {
        req = {};
        if (!(x > 0.0)) return;
        const int N = (x == std::floor(x)) ? (int) x : (int) std::ceil(x);
        req.wIdx = tb.sincos(2 * kPi * (lp ? std::min(fc / sr, 0.5) : fc / sr));
        if (N & 1) req.tIdx = tb.sincos(kPi * fc / sr);
        if (x != cachedX)
        {
            req.secBase = tb.ns;
            for (int k = 0; k < std::min(N / 2, kMaxSections); ++k) tb.sincos((k + 0.5) * kPi / x);
        }
    }

    // faithfulQuirk: for fractional orders with even ceil(order) the original applies the 1st-order
    // coefficients left over from its last odd-order design (stale). false = always fresh.
    template <class B>
    void design(double fc, double x, double sr, bool faithfulQuirk, const B& tb)
    {
        if (!(x > 0.0)) { nsec = 0; w58 = 0; w60 = 0; return; }   // curN intentionally kept (as the original)
        const int N = (x == std::floor(x)) ? (int) x : (int) std::ceil(x);
        const int prevN = curN;
        const int newSec = std::min(N / 2, kMaxSections);
        const bool odd = (N & 1) != 0;
        if (newSec > nsec)
            for (auto& g : st) for (int k = nsec; k < newSec; ++k) g[k].reset();
        nsec = newSec;
        w58 = odd ? 0.0 : 1.0 - (N - x);
        w60 = odd ? 1.0 - (N - x) : (N - x);
        if (req.secBase >= 0)
        {
            for (int k = 0; k < nsec; ++k) sk[k] = tb.sOut[req.secBase + k];
            cachedX = x;
        }
        const double sw = tb.sOut[req.wIdx], c = tb.cOut[req.wIdx];
        for (int k = 0; k < nsec; ++k)
        {
            const double d = 1.0 / (1.0 + sk[k] * sw);
            Coeffs cf;
            if (lp) cf = { d * (1 - c) * 0.5, d * (1 - c), d * (1 - c) * 0.5, -2 * c * d, (1 - sk[k] * sw) * d };
            else    cf = { d * (1 + c) * 0.5, -d * (1 + c), d * (1 + c) * 0.5, -2 * c * d, (1 - sk[k] * sw) * d };
            sec[k].set(cf);
        }
        Coeffs fresh {};
        if (odd || !faithfulQuirk)
        {
            double tk;
            if (req.tIdx >= 0) tk = tb.sOut[req.tIdx] / tb.cOut[req.tIdx];
            else
            {   // even N, quirk off: tan not queued (rare path) -> compute through the same vector math
                double s1, c1;
                vsincos<VScalar>(kPi * fc / sr, s1, c1);
                tk = s1 / c1;
            }
            fresh = lp ? bilinearLowPass1(tk) : bilinearHighPass1(tk);
        }
        if (odd)
        {
            staleFirst = fresh;
            first.set(fresh);
            if (prevN < N) for (auto& s : fst) s.reset();
        }
        else
            first.set(faithfulQuirk ? staleFirst : fresh);
        curN = N;
    }

    inline T process(int g, T x)
    {
        T y = x;
        const int full = nsec - (w58 != 0.0 ? 1 : 0);
        for (int k = 0; k < full; ++k) y = st[g][k].process(sec[k], y);
        if (w58 != 0.0 && nsec != 0)
            y = blendIn<V>(st[g][nsec - 1].process(sec[nsec - 1], V::mul(V::set1(w58), y)), y, w58);
        if (w60 != 0.0)
            y = blendIn<V>(fst[g].process(first, V::mul(V::set1(w60), y)), y, w60);
        return y;
    }

    void reset()
    {
        for (auto& g : st) for (auto& s : g) s.reset();
        for (auto& s : fst) s.reset();
    }

    void seed(const Coeffs& stale) { staleFirst = stale; }

private:
    struct Req { int wIdx = -1, tIdx = -1, secBase = -1; };
    static constexpr int kGroups = (kMaxChannels + V::W - 1) / V::W;
    bool lp;
    Req req;
    int nsec = 0, curN = 0;
    double w58 = 0, w60 = 0, cachedX = -1.0;
    std::array<double, kMaxSections> sk {};
    std::array<CoeffsV<V>, kMaxSections> sec {};
    CoeffsV<V> first {};
    Coeffs staleFirst {};
    std::array<std::array<DF1V<V>, kMaxSections>, kGroups> st {};
    std::array<DF1V<V>, kGroups> fst {};
};

// ---------------------------------------------------------------------------
// Pickup model -> 22 inner EQ values (FUN_1805e86b0). Not in the audio inner loop.
// The triangle test is evaluated in strict IEEE order: on exact vertices the original's result
// depends on rounding (it even falls back to its default row at AC-Custom X = 100, Y = 0 / 100).
// ---------------------------------------------------------------------------
inline double pow10s(double x)      // scalar 10^x through the shared vector math (libm-independent)
{
    return vpow10<VScalar>(x);
}

inline double navCurve(int k, double n)
{
    n = std::min(std::max(n, 0.0), 1.0);
    switch (k)
    {
        case 1: case 3: case 5: case 8: case 11: case 14: case 16: return 20.0 * pow10s(n * 3.04139268515822504075);  // 20*1100^n
        case 4: case 7: case 10: case 13: case 15: case 18: return -40.0 + 80.0 * n;
        case 6: case 9: case 12: return pow10s(3.0 * n - 2.0);
        case 2: case 17: return std::floor(20.0 * n + 0.5);
        default: return n;
    }
}

inline int customModelRowImpl(int model, double xPct, double yPct, double out[22])
{
    using namespace ::pw::tables;
    const bool e = (model == 27);
    const double (*rows)[21] = e ? kNavERows : kNavACRows;
    const int (*tris)[3] = e ? kNavETris : kNavACTris;
    const int ntris = e ? kNavENumTris : kNavACNumTris;
    const double* M = e ? kNavEMatrix : kNavACMatrix;
    const double* def = e ? kNavEDefaultRow : kNavACDefaultRow;
    const double u = yPct * 0.01 * (e ? kNavEUScale : kNavACUScale) + (e ? kNavEU0 : kNavACU0);
    const double v = xPct * 0.01 * (e ? kNavEVScale : kNavACVScale) + (e ? kNavEV0 : kNavACV0);

    double R[21];
    int foundTri = -1;
    for (int t = 0; t < ntris && foundTri < 0; ++t)
    {
        const double* ra = rows[tris[t][0]];
        const double* rb = rows[tris[t][1]];
        const double* rc = rows[tris[t][2]];
        const double xc = rc[19], yc = rc[20];
        const double d8 = u - xc, d35 = v - yc;
        const double den = 1.0 / ((ra[19] - xc) * (yc - rb[20]) + (ra[20] - yc) * (rb[19] - xc));
        const double wa = (d8 * (yc - rb[20]) + d35 * (rb[19] - xc)) * den;
        if (!(wa >= 0.0 && wa <= 1.0)) continue;
        const double wb = ((xc - ra[19]) * d35 + d8 * (ra[20] - yc)) * den;
        if (!(wb >= 0.0 && wb <= 1.0)) continue;
        double wc = (1.0 - wa) - wb;
        if (std::fabs(wc) < 1e-6) wc = 0.0;
        if (!(wc >= 0.0 && wc <= 1.0)) continue;
        for (int j = 0; j < 19; ++j) R[j] = ra[j] * wa + rb[j] * wb + rc[j] * wc;
        foundTri = t;
    }
    if (foundTri < 0)
        for (int j = 0; j < 19; ++j) R[j] = def[j];
    R[19] = u;
    R[20] = v;
    out[0] = 0.0;
    for (int k = 1; k <= 21; ++k)
    {
        double n = 0.0;
        for (int i = 0; i < 21; ++i) n += M[(k - 1) * 21 + i] * R[i];
        out[k] = navCurve(k, n);
    }
    return foundTri;
}

inline void customModelRow(int model, double xPct, double yPct, double out[22]) { customModelRowImpl(model, xPct, yPct, out); }

// Target inner EQ values for (model, X, Y, Amount); Amount scales indices {2,4,7,10,13,15,17,18}.
inline void innerEqTargets(int model, double xPct, double yPct, double amountPct, double out[22])
{
    model = std::min(std::max(model, 0), 28);
    if (model < ::pw::tables::kNumPresets)
        for (int k = 0; k < 22; ++k) out[k] = ::pw::tables::kPresets[model][k];
    else
        customModelRow(model, xPct, yPct, out);
    const double a = amountPct * 0.01;
    for (int k : { 2, 4, 7, 10, 13, 15, 17, 18 }) out[k] *= a;
}

// ---------------------------------------------------------------------------
// The engine, templated on the vector backend
// ---------------------------------------------------------------------------
template <class V>
class Engine final : public detail::EngineBase
{
public:
    using T = typename V::T;

    const char* name() const override { return V::name; }

    void prepare(double sampleRate, double smoothingSeconds, bool faithfulQuirk, const Parameters& p) override
    {
        sr = sampleRate;
        quirk = faithfulQuirk;
        rampLen = std::max(1, (int) std::lround(smoothingSeconds * sr));
        modelRampLen = std::max(1, (int) std::lround(kSwitchFadeSeconds * sr));
        for (auto* f : { &fadePickup, &fadeBody, &fadeTone }) f->setLength((int) std::lround(kSwitchFadeSeconds * sr));
        fadeBypass.setLength((int) std::lround(kBypassFadeSeconds * sr));
        for (auto& f : fadeFrom) f.setLength((int) std::lround(kSwitchFadeSeconds * sr));
        bodyGlide = 1.0 - pow10s(-20.0 / sr);
        bodyLine.assign((size_t) kGroups * kBodyLineSize * V::W, 0.0f);

        // constant-frequency trig (From stages, brightness, volume shelves)
        {
            const double in[6] = { 2 * kPi * 3105.0 / sr, 2 * kPi * 3000.0 / sr, 2 * kPi * 5800.0 / sr,
                                   2 * kPi * 26.0 / sr, 2 * kPi * 1800.0 / sr, 2 * kPi * 2250.0 / sr };
            for (int k = 0; k < 6; ++k) vsincos<VScalar>(in[k], constS[k], constC[k]);
        }

        // A fresh original instance starts on Single Hot: seeds the stale 1st-order Butterworth
        // coefficients (HP 43.0897 Hz order 1, LP 10492.5 Hz order 1).
        {
            const double in[2] = { kPi * ::pw::tables::kPresets[0][1] / sr, kPi * ::pw::tables::kPresets[0][16] / sr };
            double s[2], c[2];
            for (int k = 0; k < 2; ++k) vsincos<VScalar>(in[k], s[k], c[k]);
            hp.seed(bilinearHighPass1(s[0] / c[0]));
            lp.seed(bilinearLowPass1(s[1] / c[1]));
        }

        initialised = false;
        lastPickupTo = -1;
        setParameters(p, true);
        reset();
    }

    void reset() override
    {
        hp.reset(); lp.reset();
        for (auto& g : eqState) for (auto& s : g) s.reset();
        for (auto& g : fromState) for (auto& s : g) s.reset();
        for (auto* a : { &briState, &volState, &toneState }) for (auto& s : *a) s.reset();
        std::fill(bodyLine.begin(), bodyLine.end(), 0.0f);
        bodyPos = 0;
    }

    // instant: jump to p without ramps/fades (first configuration, or the first block after the
    // host restored a state). Realtime-safe.
    void setParameters(const Parameters& p, bool instant) override
    {
        const bool snap = instant || !initialised;
        const int len = snap ? 0 : rampLen;
        double inner[22];
        innerEqTargets(p.pickupTo, p.customX, p.customY, p.amount, inner);
        const bool modelChange = !snap && p.pickupTo != lastPickupTo;
        lastPickupTo = p.pickupTo;
        const int eqLen = snap ? 0 : (modelChange ? modelRampLen : rampLen);
        for (int k = 0; k < 22; ++k) eqVals[k].setTarget(inner[k], eqLen);
        amountS.setTarget(p.amount * 0.01, len);
        bodyAmountS.setTarget(p.bodyAmount * 0.01, len);
        bodySignS.setTarget(p.bodyPhase ? -1.0 : 1.0, len);
        bodyTargetS.setTarget(2.0 * (p.bodyThickness * 0.001) / 340.0 * sr, len);
        briS.setTarget(p.brightness * 0.12, len);
        gainS.setTarget(p.gainDb, len);
        volS.setTarget(p.volume * 0.1, len);
        toneS.setTarget(std::min(p.tone * 0.1, 1.0), len);

        const int from = std::min(std::max(p.pickupFrom, 0), 3);
        for (int i = 0; i < 4; ++i) fadeFrom[i].set(i == from);
        fadePickup.set(p.pickupEnabled);
        fadeBody.set(p.bodyEnabled);
        fadeTone.set(p.tone < 10.0);
        fadeBypass.set(!p.bypass);

        if (snap)
        {
            for (int i = 0; i < 4; ++i) fadeFrom[i].reset(i == from);
            fadePickup.reset(p.pickupEnabled);
            fadeBody.reset(p.bodyEnabled);
            fadeTone.reset(p.tone < 10.0);
            fadeBypass.reset(!p.bypass);
            bodyDelay = bodyTargetS.value();
            initialised = true;
        }
        redesign(kAll);
    }

    void process(float* const* ch, int numChannels, int numSamples) override
    {
        numChannels = std::min(numChannels, kMaxChannels);
        const int groups = (numChannels + V::W - 1) / V::W;
        alignas(32) double lanes[kGroups][kSub][V::W];
        for (int s0 = 0; s0 < numSamples; s0 += kSub)
        {
            const int n = std::min(kSub, numSamples - s0);
            // gather channels into lane-interleaved doubles (unused lanes = 0)
            for (int g = 0; g < groups; ++g)
                for (int k = 0; k < V::W; ++k)
                {
                    const int c = g * V::W + k;
                    if (c < numChannels) for (int i = 0; i < n; ++i) lanes[g][i][k] = ch[c][s0 + i];
                    else for (int i = 0; i < n; ++i) lanes[g][i][k] = 0.0;
                }
            for (int i = 0; i < n; ++i)
            {
                for (int g = 0; g < groups; ++g)
                    V::store(lanes[g][i], processSample(g, V::load(lanes[g][i])));
                bodyPos = (bodyPos + 1) % kBodyLineSize;
                advance();
            }
            for (int g = 0; g < groups; ++g)
                for (int k = 0; k < V::W; ++k)
                {
                    const int c = g * V::W + k;
                    if (c < numChannels) for (int i = 0; i < n; ++i) ch[c][s0 + i] = (float) lanes[g][i][k];
                }
        }
    }

private:
    static constexpr int kGroups = (kMaxChannels + V::W - 1) / V::W;
    static constexpr int kBodyLineSize = 512;   // >= max body delay (2*0.1/340*192k = 113) + margin
    static constexpr int kSub = 64;             // sub-block for channel gather/scatter
    enum : unsigned { kEq = 1, kFrom = 2, kBri = 4, kGain = 8, kVol = 16, kTone = 32, kAll = 63 };

    inline T processSample(int g, T x)
    {
        const T dry = x;
        T y = x;

        // ---- pickup: From compensation stages (each with its own on/off fade) + To EQ ----
        if (!fadePickup.fullyOff())
        {
            T p = y;
            for (int i = 0; i < 4; ++i)
            {
                if (fadeFrom[i].fullyOff()) continue;
                T s = V::mul(p, V::set1(fromGain[i]));
                for (int j = 0; j < fromNumSec[i]; ++j) s = fromState[g][i * 2 + j].process(fromCoef[i][j], s);
                p = xfade<V>(p, s, fadeFrom[i].weight());
            }
            p = hp.process(g, p);
            for (int j = 0; j < 5; ++j) p = eqState[g][j].process(eqCoef[j], p);
            p = lp.process(g, p);
            p = V::mul(p, V::set1(eqOutGain));
            y = xfade<V>(y, p, fadePickup.weight());
        }

        // ---- body comb (float delay line, lane-interleaved) ----
        float* line = &bodyLine[(size_t) g * kBodyLineSize * V::W];
        V::storeFloat(line + bodyPos * V::W, y);
        if (!fadeBody.fullyOff())
        {
            const double d = bodyDelay;
            const int k = (int) std::floor(d);
            const double fr = d - k;
            const T xk = V::loadFloat(line + ((bodyPos - k + kBodyLineSize) % kBodyLineSize) * V::W);
            const T xk1 = V::loadFloat(line + ((bodyPos - k - 1 + kBodyLineSize) % kBodyLineSize) * V::W);
            const double b = bodyAmountS.value();
            const T taps = V::add(V::mul(V::set1(1.0 - fr), xk), V::mul(V::set1(fr), xk1));
            const T s = V::add(V::mul(V::set1(1.0 - b * 0.5), y), V::mul(V::set1(bodySignS.value() * (b * 0.5)), taps));
            y = xfade<V>(y, s, fadeBody.weight());
        }

        // ---- brightness, gain, volume, tone ----
        y = briState[g].process(briCoef, y);
        y = V::mul(y, V::set1(gainLin));
        y = volState[g].process(volCoef, V::mul(y, V::set1(volGain)));
        if (!fadeTone.fullyOff())
            y = xfade<V>(y, toneState[g].process(toneCoef, y), fadeTone.weight());

        // ---- bypass ----
        return xfade<V>(dry, y, fadeBypass.weight());
    }

    void advance()
    {
        unsigned dirty = 0;
        for (auto& s : eqVals) if (s.advance()) dirty |= kEq;
        if (amountS.advance()) dirty |= kFrom;
        bodyAmountS.advance(); bodySignS.advance(); bodyTargetS.advance();
        if (briS.advance()) dirty |= kBri;
        if (gainS.advance()) dirty |= kGain;
        if (volS.advance()) dirty |= kVol;
        if (toneS.advance()) dirty |= kTone;
        bodyDelay += bodyGlide * (bodyTargetS.value() - bodyDelay);
        for (auto& f : fadeFrom) f.advance();
        fadePickup.advance(); fadeBody.advance(); fadeTone.advance(); fadeBypass.advance();
        if (dirty) redesign(dirty);
    }

    // Two-phase redesign: queue every transcendental needed by the dirty stages, evaluate them in
    // vector batches (10^x first: the tone cutoff feeds a sincos), then finish the coefficients.
    void redesign(unsigned dirty)
    {
        TransBatch<V>& tb = batch;
        tb.clear();
        double v[22];
        int iEq[6] = {}, iFrom[8] = {}, iBri = 0, iGain = 0, iVol = 0, iToneQ = 0, iToneF = -1;
        if (dirty & kEq)
        {
            for (int k = 0; k < 22; ++k) v[k] = eqVals[k].value();
            const int gi[6] = { 4, 7, 10, 13, 15, 18 };
            for (int j = 0; j < 5; ++j) iEq[j] = tb.pow10(v[gi[j]] / 40.0);
            iEq[5] = tb.pow10(v[18] / 20.0);
        }
        if (dirty & kFrom)
        {
            const double a = amountS.value();
            const double ex[8] = { 2.5 * a / 20.0, 7.0 * a / 20.0, 8.0 * a / 20.0, 9.0 * a / 20.0,
                                   5.5 * a / 20.0, -5.5 * a / 20.0, -6.0 * a / 40.0, -3.0 * a / 40.0 };
            for (int j = 0; j < 8; ++j) iFrom[j] = tb.pow10(ex[j]);
        }
        if (dirty & kBri) iBri = tb.pow10(briS.value() / 20.0);
        if (dirty & kGain) iGain = tb.pow10(gainS.value() / 20.0);
        const double vv = volS.value();
        if (dirty & kVol) iVol = tb.pow10(-5.0 * (1.0 - vv) / 20.0);
        const double d = std::min(toneS.value(), 1.0);
        if (dirty & kTone)
        {
            iToneQ = tb.pow10((1 - d) * 0.3 - 0.15);
            if (d > 0.5) iToneF = tb.pow10(2 * (d - 0.5) * 1.04139268515822504075);   // 11^t = 10^(t*log10(11))
        }
        tb.runPow10();

        int iW[5] = {}, iToneW = 0;
        if (dirty & kEq)
        {
            hp.queue(v[1], v[2], sr, tb);
            lp.queue(v[16], v[17], sr, tb);
            const int fi[5] = { 3, 5, 8, 11, 14 };
            for (int j = 0; j < 5; ++j) iW[j] = tb.sincos(2 * kPi * v[fi[j]] / sr);
        }
        if (dirty & kTone)
        {
            const double f = d <= 0.5 ? 400 + 3200 * d : 2000 * tb.pOut[iToneF];
            iToneW = tb.sincos(2 * kPi * std::min(f / sr, 0.5));
        }
        tb.runSinCos();

        if (dirty & kEq)
        {
            hp.design(v[1], v[2], sr, quirk, tb);
            lp.design(v[16], v[17], sr, quirk, tb);
            auto S = [&](int j) { return tb.sOut[iW[j]]; };
            auto C = [&](int j) { return tb.cOut[iW[j]]; };
            eqCoef[0].set(rbjLowShelf(tb.pOut[iEq[0]], S(0), C(0)));
            eqCoef[1].set(rbjPeak(1.0 / v[6], tb.pOut[iEq[1]], S(1), C(1)));
            eqCoef[2].set(rbjPeak(1.0 / v[9], tb.pOut[iEq[2]], S(2), C(2)));
            eqCoef[3].set(rbjPeak(1.0 / v[12], tb.pOut[iEq[3]], S(3), C(3)));
            eqCoef[4].set(rbjHighShelf(tb.pOut[iEq[4]], S(4), C(4)));
            eqOutGain = tb.pOut[iEq[5]];
        }
        if (dirty & kFrom)
        {
            for (int i = 0; i < 4; ++i) fromGain[i] = tb.pOut[iFrom[i]];
            fromNumSec = { 1, 0, 1, 2 };
            fromCoef[0][0].set(monoQuadHighShelf(tb.pOut[iFrom[4]], constS[0]));
            fromCoef[2][0].set(monoQuadHighShelf(tb.pOut[iFrom[5]], constS[1]));
            fromCoef[3][0].set(rbjHighShelf(tb.pOut[iFrom[6]], constS[2], constC[2]));
            fromCoef[3][1].set(rbjPeak(0.5, tb.pOut[iFrom[7]], constS[3], constC[3]));
        }
        if (dirty & kBri) briCoef.set(monoQuadHighShelf(tb.pOut[iBri], constS[4]));
        if (dirty & kGain) gainLin = tb.pOut[iGain];
        if (dirty & kVol)
        {
            volGain = vv * vv;
            volCoef.set(monoQuadHighShelf(tb.pOut[iVol], constS[5]));
        }
        if (dirty & kTone) toneCoef.set(rbjLowPass(tb.pOut[iToneQ], tb.sOut[iToneW], tb.cOut[iToneW]));
    }

    double sr = 44100.0;
    bool quirk = true, initialised = false;
    int rampLen = 512, modelRampLen = 882, lastPickupTo = -1;
    TransBatch<V> batch;
    double constS[6] {}, constC[6] {};

    std::array<LinearSmoother, 22> eqVals {};
    LinearSmoother amountS, bodyAmountS, bodySignS, bodyTargetS, briS, gainS, volS, toneS;
    SwitchFade fadePickup, fadeBody, fadeTone, fadeBypass;
    std::array<SwitchFade, 4> fadeFrom {};

    FracButterworth<V> hp { false }, lp { true };
    std::array<CoeffsV<V>, 5> eqCoef {};
    double eqOutGain = 1.0;
    std::array<std::array<DF1V<V>, 5>, kGroups> eqState {};

    std::array<double, 4> fromGain {};
    std::array<int, 4> fromNumSec { 1, 0, 1, 2 };
    std::array<std::array<CoeffsV<V>, 2>, 4> fromCoef {};
    std::array<std::array<DF1V<V>, 8>, kGroups> fromState {};

    std::vector<float> bodyLine;
    int bodyPos = 0;
    double bodyDelay = 0.0, bodyGlide = 0.0;

    CoeffsV<V> briCoef {}, volCoef {}, toneCoef {};
    double gainLin = 1.0, volGain = 1.0;
    std::array<DF1V<V>, kGroups> briState {}, volState {}, toneState {};
};

} // namespace PW_ARCH_NS
PW_FP_STRICT_END

// ---------------------------------------------------------------------------
// Front end with backend selection (compiled only in the baseline translation unit)
// ---------------------------------------------------------------------------
#if !defined(PW_BUILDING_AVX_TU)

#if defined(PW_ENABLE_AVX_DISPATCH) && !defined(PW_HAS_AVX)
// Defined in PickupWeaverDSP_AVX.cpp (compiled with AVX enabled).
detail::EngineBase* makeEngineAVX();
#endif

namespace detail {
inline bool cpuHasAVX()
{
#if defined(PW_HAS_AVX)
    return true;
#elif defined(PW_ENABLE_AVX_DISPATCH) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 1);
    const bool osx = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
    return osx && avx && ((_xgetbv(0) & 6) == 6);
#else
    unsigned a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!((c >> 27) & 1) || !((c >> 28) & 1)) return false;
    unsigned lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;     // OS saves XMM and YMM state
#endif
#else
    return false;
#endif
}
} // namespace detail

class PickupWeaverDSP
{
public:
    // smoothingSeconds: fixed linear ramp time for continuous parameters (Option B).
    // faithfulQuirk: reproduce the original's stale 1st-order Butterworth coefficients.
    // backend: Auto = best available; requesting an unavailable backend falls back to Auto.
    void prepare(double sampleRate, double smoothingSeconds = kDefaultSmoothingSeconds, bool faithfulQuirk = true,
                 Backend backend = Backend::Auto)
    {
        engine.reset(create(backend));
        engine->prepare(sampleRate, smoothingSeconds, faithfulQuirk, params);
    }
    // Call at the start of every block. instant = true jumps to p without smoothing (use it for the
    // first block after activation, when the host delivers a restored state). Realtime-safe.
    void setParameters(const Parameters& p, bool instant = false)
    {
        params = p;
        if (engine) engine->setParameters(p, instant);
    }
    void process(float* const* ch, int numChannels, int numSamples)
    {
        if (engine) engine->process(ch, numChannels, numSamples);
    }
    void reset() { if (engine) engine->reset(); }
    const char* backendName() const { return engine ? engine->name() : "none"; }

    static bool isAvailable(Backend b)
    {
        switch (b)
        {
            case Backend::Auto: case Backend::Scalar: return true;
#if defined(PW_HAS_SSE2)
            case Backend::SSE2: return true;
#endif
            case Backend::AVX:
#if defined(PW_HAS_AVX) || defined(PW_ENABLE_AVX_DISPATCH)
                return detail::cpuHasAVX();
#else
                return false;
#endif
#if defined(PW_HAS_NEON)
            case Backend::NEON: return true;
#endif
            default: return false;
        }
    }

private:
    static detail::EngineBase* create(Backend b)
    {
        using namespace PW_ARCH_NS::simd;
        if (b != Backend::Auto && !isAvailable(b)) b = Backend::Auto;
        if (b == Backend::Auto)
        {
            if (isAvailable(Backend::NEON)) b = Backend::NEON;
            else if (isAvailable(Backend::AVX)) b = Backend::AVX;
            else if (isAvailable(Backend::SSE2)) b = Backend::SSE2;
            else b = Backend::Scalar;
        }
        switch (b)
        {
#if defined(PW_HAS_AVX)
            case Backend::AVX: return new PW_ARCH_NS::Engine<VAVX>();
#elif defined(PW_ENABLE_AVX_DISPATCH)
            case Backend::AVX: return makeEngineAVX();
#endif
#if defined(PW_HAS_SSE2)
            case Backend::SSE2: return new PW_ARCH_NS::Engine<VSSE2>();
#endif
#if defined(PW_HAS_NEON)
            case Backend::NEON: return new PW_ARCH_NS::Engine<VNEON>();
#endif
            default: return new PW_ARCH_NS::Engine<VScalar>();
        }
    }

    Parameters params;
    std::unique_ptr<detail::EngineBase> engine;
};

#endif // !PW_BUILDING_AVX_TU

} // namespace pw
