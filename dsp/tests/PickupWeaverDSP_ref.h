// ============================================================================
// PickupWeaverDSP_ref.h -- pre-SIMD scalar version of the DSP core (regression reference).
//
// Kept unchanged (apart from names) so tests can check the SIMD engine against it.
//
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "../PickupWeaverTables.h"

namespace pwref {
namespace tables = ::pw::tables;

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDefaultSmoothingSeconds = 512.0 / 44100.0;   // 11.61 ms
inline constexpr double kSwitchFadeSeconds = 0.020;                    // enable/disable fades
inline constexpr double kBypassFadeSeconds = 0.010;
inline constexpr int kMaxChannels = 8;

// ---------------------------------------------------------------------------
// Parameters in PLAIN units (what the original displays)
// ---------------------------------------------------------------------------
struct Parameters
{
    bool   bypass        = false;
    bool   pickupEnabled = true;
    int    pickupFrom    = 1;       // 0 Humbucker, 1 Single Coil, 2 Single Coil (Bright), 3 Piezo
    int    pickupTo      = 0;       // 0..26 built-in models, 27 E-Custom, 28 AC-Custom
    double amount        = 100.0;   // %   pickup simulation amount
    double customX       = 50.0;    // %   (E-/AC-Custom navigator)
    double customY       = 50.0;    // %
    bool   bodyEnabled   = true;
    double bodyThickness = 10.0;    // mm  (10..100)
    double bodyAmount    = 0.0;     // %
    bool   bodyPhase     = false;
    double brightness    = 0.0;     // %   (-100..100)
    double gainDb        = 0.0;     // dB  (-20..20)
    double volume        = 10.0;    // 0..10
    double tone          = 10.0;    // 0..10 (10 = filter bypassed)
};

// ---------------------------------------------------------------------------
// Biquad coefficients  y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
// ---------------------------------------------------------------------------
struct Coeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline Coeffs norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

inline Coeffs rbjPeak(double f, double q, double gainDb, double sr)
{
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * f / sr;
    const double al = std::sin(w) / (2 * q), c = std::cos(w);
    return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

inline Coeffs rbjLowShelf(double f, double gainDb, double sr)      // S = 1 (FUN_180ad97d0)
{
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * f / sr, c = std::cos(w);
    const double sa = 2 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
    return norm(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}

inline Coeffs rbjHighShelf(double f, double gainDb, double sr)     // S = 1 (FUN_180ad8a10)
{
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * f / sr, c = std::cos(w);
    const double sa = 2 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
    return norm(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

inline Coeffs rbjLowPass(double f, double q, double sr)
{
    const double w = 2 * kPi * std::min(f / sr, 0.5), al = std::sin(w) / (2 * q), c = std::cos(w);
    return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

inline Coeffs rbjHighPass(double f, double q, double sr)
{
    const double w = 2 * kPi * f / sr, al = std::sin(w) / (2 * q), c = std::cos(w);
    return norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

inline Coeffs bilinearLowPass1(double f, double sr)
{
    const double k = std::tan(kPi * f / sr);
    return norm(k, k, 0, 1 + k, k - 1, 0);
}

inline Coeffs bilinearHighPass1(double f, double sr)
{
    const double k = std::tan(kPi * f / sr);
    return norm(1, -1, 0, 1 + k, k - 1, 0);
}

// First-order high shelf (FUN_180aca900): DC gain 1, HF gain g
inline Coeffs monoQuadHighShelf(double f, double gainDb, double sr)
{
    const double g = std::pow(10.0, gainDb / 20.0);
    const double k = std::sin(2 * kPi * f / sr) * (g + 1) * 0.25;
    return norm(g + k, -(g - k), 0, 1 + k, -(1 - k), 0);
}

// Tone low-pass (FUN_1805e7f30 / FUN_1805e7ca0); d = Tone/10 (d = 1 handled by a fade)
inline Coeffs toneLowPass(double d, double sr)
{
    d = std::min(d, 1.0);
    const double q = std::pow(10.0, (1 - d) * 0.3 - 0.15);
    const double f = d <= 0.5 ? 400 + 3200 * d : 2000 * std::pow(11.0, 2 * (d - 0.5));
    return rbjLowPass(f, q, sr);
}

// ---------------------------------------------------------------------------
// Direct Form I state (one per channel per section), as in the original kernels
// ---------------------------------------------------------------------------
struct DF1
{
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    inline double process(const Coeffs& c, double x)
    {
        double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
        if (std::fabs(y) < 1e-300) y = 0.0;      // the original flushes denormals in its kernels
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0; }
};

// ---------------------------------------------------------------------------
// Smoothing primitives
// ---------------------------------------------------------------------------
// Linear ramp to the target over a fixed number of samples (linear-smoothing semantics,
// but the CURRENT value is used for the current sample and the step is taken afterwards,
// like the original).
class LinearSmoother
{
public:
    void reset(double v) { cur = tgt = v; left = 0; step = 0; }
    void setTarget(double t, int len)
    {
        if (t == tgt) return;
        tgt = t;
        if (len <= 0) { cur = t; left = 0; return; }
        step = (tgt - cur) / len;
        left = len;
    }
    bool active() const { return left > 0; }
    double value() const { return cur; }
    double target() const { return tgt; }
    void advance()
    {
        if (left > 0) { if (--left == 0) cur = tgt; else cur += step; }
    }

private:
    double cur = 0, tgt = 0, step = 0;
    int left = 0;
};

// Switch fade: position u moves linearly 0 <-> 1 over L samples; processed weight = u^2.
// (Measured: enabling weight (t/L)^2, disabling (1 - t/L)^2, i.e. u^2 in both directions.)
class SwitchFade
{
public:
    void reset(bool on) { u = on ? 1.0 : 0.0; target = u; }
    void set(bool on) { target = on ? 1.0 : 0.0; }
    void setLength(int samples) { inc = 1.0 / std::max(1, samples); }
    bool fading() const { return u != target; }
    bool fullyOff() const { return u == 0.0 && target == 0.0; }
    bool fullyOn() const { return u == 1.0 && target == 1.0; }
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
// Fractional-order Butterworth low-/high-pass
// FUN_180ad6f20 (layout), FUN_180ad7c20 (coefficients), FUN_180ad5de0 (kernel).
// ---------------------------------------------------------------------------
class FracButterworth
{
public:
    static constexpr int kMaxSections = 10;    // order <= 20

    explicit FracButterworth(bool lowPass) : lp(lowPass) {}

    // faithfulQuirk: for fractional orders with even ceil(order) the original applies the 1st-order
    // coefficients left over from its last odd-order design (stale). false = always fresh.
    void design(double fc, double x, double sr, bool faithfulQuirk)
    {
        if (!(x > 0.0)) { nsec = 0; w58 = 0; w60 = 0; return; }
        const int N = (x == std::floor(x)) ? (int) x : (int) std::ceil(x);
        const int prevN = curN;
        const int newSec = std::min(N / 2, kMaxSections);
        const bool odd = (N & 1) != 0;
        if (newSec > nsec)
            for (int ch = 0; ch < kMaxChannels; ++ch)
                for (int k = nsec; k < newSec; ++k) st[ch][k].reset();
        nsec = newSec;
        w58 = odd ? 0.0 : 1.0 - (N - x);
        w60 = odd ? 1.0 - (N - x) : (N - x);
        const double w = 2 * kPi * (lp ? std::min(fc / sr, 0.5) : fc / sr);
        const double sw = std::sin(w), c = std::cos(w);
        for (int k = 0; k < nsec; ++k)
        {
            const double sk = std::sin((k + 0.5) * kPi / x);
            const double d = 1.0 / (1.0 + sk * sw);
            if (lp) sec[k] = { d * (1 - c) * 0.5, d * (1 - c), d * (1 - c) * 0.5, -2 * c * d, (1 - sk * sw) * d };
            else    sec[k] = { d * (1 + c) * 0.5, -d * (1 + c), d * (1 + c) * 0.5, -2 * c * d, (1 - sk * sw) * d };
        }
        if (odd)
        {
            first = lp ? bilinearLowPass1(fc, sr) : bilinearHighPass1(fc, sr);
            staleFirst = first;
            if (prevN < N)
                for (int ch = 0; ch < kMaxChannels; ++ch) fst[ch].reset();
        }
        else
        {
            first = faithfulQuirk ? staleFirst : (lp ? bilinearLowPass1(fc, sr) : bilinearHighPass1(fc, sr));
        }
        curN = N;
    }

    inline double process(int ch, double x)
    {
        double y = x;
        const int full = nsec - (w58 != 0.0 ? 1 : 0);
        for (int k = 0; k < full; ++k) y = st[ch][k].process(sec[k], y);
        if (w58 != 0.0 && nsec != 0)
        {
            const double v = y;
            y = st[ch][nsec - 1].process(sec[nsec - 1], w58 * v) + (1.0 - w58) * v;
        }
        if (w60 != 0.0)
        {
            const double v = y;
            y = fst[ch].process(first, w60 * v) + (1.0 - w60) * v;
        }
        return y;
    }

    void reset()
    {
        for (auto& c : st) for (auto& s : c) s.reset();
        for (auto& s : fst) s.reset();
    }

private:
    bool lp;
    int nsec = 0, curN = 0;
    double w58 = 0, w60 = 0;
    std::array<Coeffs, kMaxSections> sec {};
    Coeffs first {}, staleFirst {};
    std::array<std::array<DF1, kMaxSections>, kMaxChannels> st {};
    std::array<DF1, kMaxChannels> fst {};
};

// ---------------------------------------------------------------------------
// Pickup model -> 22 inner EQ values (FUN_1805e86b0)
// ---------------------------------------------------------------------------
inline double navCurve(int k, double n)
{
    n = std::min(std::max(n, 0.0), 1.0);
    switch (k)
    {
        case 1: case 3: case 5: case 8: case 11: case 14: case 16: return 20.0 * std::pow(1100.0, n);
        case 4: case 7: case 10: case 13: case 15: case 18: return -40.0 + 80.0 * n;
        case 6: case 9: case 12: return std::pow(10.0, 3.0 * n - 2.0);
        case 2: case 17: return std::floor(20.0 * n + 0.5);
        default: return n;
    }
}

// The navigator's triangle test is evaluated in strict IEEE order: at points exactly on a vertex
// (e.g. the corners of the X/Y square) the original's result depends on exact rounding, and the
// original even falls back to its default row at AC-Custom X = 100, Y = 0 / 100. Fused
// multiply-add contraction (GCC -O2, some ARM builds) would change which branch is taken.
#if defined(__GNUC__) && !defined(__clang__)
#define PW_STRICT_FP __attribute__((optimize("fp-contract=off")))
#else
#define PW_STRICT_FP
#endif
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

PW_STRICT_FP inline int customModelRowImpl(int model, double xPct, double yPct, double out[22])
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    using namespace tables;
    const bool e = (model == 27);
    const double (*rows)[21] = e ? kNavERows : kNavACRows;
    const int (*tris)[3] = e ? kNavETris : kNavACTris;
    const int ntris = e ? kNavENumTris : kNavACNumTris;
    const double* M = e ? kNavEMatrix : kNavACMatrix;
    const double* def = e ? kNavEDefaultRow : kNavACDefaultRow;
    const double u = yPct * 0.01 * (e ? kNavEUScale : kNavACUScale) + (e ? kNavEU0 : kNavACU0);
    const double v = xPct * 0.01 * (e ? kNavEVScale : kNavACVScale) + (e ? kNavEV0 : kNavACV0);

    double R[21];
    bool found = false;
    int foundTri = -1;
    for (int t = 0; t < ntris && !found; ++t)
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
        found = true;
        foundTri = t;
    }
    if (!found)
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
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

inline void customModelRow(int model, double xPct, double yPct, double out[22])
{
    customModelRowImpl(model, xPct, yPct, out);
}

// Target inner EQ values for (model, X, Y, Amount), with the original's Amount scaling
// (indices {2,4,7,10,13,15,17,18} -> orders and gains are multiplied by Amount/100).
inline void innerEqTargets(int model, double xPct, double yPct, double amountPct, double out[22])
{
    model = std::min(std::max(model, 0), 28);
    if (model < tables::kNumPresets)
        for (int k = 0; k < 22; ++k) out[k] = tables::kPresets[model][k];
    else
        customModelRow(model, xPct, yPct, out);
    const double a = amountPct * 0.01;
    for (int k : { 2, 4, 7, 10, 13, 15, 17, 18 }) out[k] *= a;
}

// ---------------------------------------------------------------------------
// The processor
// ---------------------------------------------------------------------------
class PickupWeaverDSP
{
public:
    // smoothingSeconds: fixed linear ramp time for continuous parameters (Option B).
    // faithfulQuirk: reproduce the original's stale 1st-order Butterworth coefficients.
    void prepare(double sampleRate, double smoothingSeconds = kDefaultSmoothingSeconds, bool faithfulQuirk = true)
    {
        sr = sampleRate;
        quirk = faithfulQuirk;
        rampLen = std::max(1, (int) std::lround(smoothingSeconds * sr));
        modelRampLen = std::max(1, (int) std::lround(kSwitchFadeSeconds * sr));
        fadePickup.setLength((int) std::lround(kSwitchFadeSeconds * sr));
        fadeBody.setLength((int) std::lround(kSwitchFadeSeconds * sr));
        fadeTone.setLength((int) std::lround(kSwitchFadeSeconds * sr));
        fadeBypass.setLength((int) std::lround(kBypassFadeSeconds * sr));
        for (auto& f : fadeFrom) f.setLength((int) std::lround(kSwitchFadeSeconds * sr));
        bodyGlide = 1.0 - std::pow(10.0, -20.0 / sr);
        bodyLine.assign((size_t) kMaxChannels * kBodyLineSize, 0.0f);

        // The original instance starts on the default model (Single Hot) -> seeds the stale
        // 1st-order Butterworth coefficients exactly as a fresh plugin instance does.
        hp.design(tables::kPresets[0][1], tables::kPresets[0][2], sr, quirk);
        lp.design(tables::kPresets[0][16], tables::kPresets[0][17], sr, quirk);

        initialised = false;
        setParameters(params);
        reset();
    }

    void reset()
    {
        hp.reset(); lp.reset();
        for (auto& c : eqState) for (auto& s : c) s.reset();
        for (auto& c : fromState) for (auto& s : c) s.reset();
        for (auto& s : briState) s.reset();
        for (auto& s : volState) s.reset();
        for (auto& s : toneState) s.reset();
        std::fill(bodyLine.begin(), bodyLine.end(), 0.0f);
        bodyPos = 0;
    }

    // Call at the start of every block (audio thread). Ramps start from the current values.
    void setParameters(const Parameters& p)
    {
        params = p;
        const int len = initialised ? rampLen : 0;
        double inner[22];
        innerEqTargets(p.pickupTo, p.customX, p.customY, p.amount, inner);
        // A discrete model change (Pickup To) glides over the original's fixed 20 ms switch time;
        // continuous knobs (Amount, Custom X/Y) use the normal smoothing ramp.
        const bool modelChange = initialised && p.pickupTo != lastPickupTo;
        lastPickupTo = p.pickupTo;
        const int eqLen = initialised ? (modelChange ? modelRampLen : rampLen) : 0;
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

        if (!initialised)
        {
            for (int i = 0; i < 4; ++i) fadeFrom[i].reset(i == from);
            fadePickup.reset(p.pickupEnabled);
            fadeBody.reset(p.bodyEnabled);
            fadeTone.reset(p.tone < 10.0);
            fadeBypass.reset(!p.bypass);
            bodyDelay = bodyTargetS.value();
            initialised = true;
        }
        redesignAll();
    }

    // In-place processing of numChannels (<= kMaxChannels) float buffers.
    void process(float* const* ch, int numChannels, int numSamples)
    {
        numChannels = std::min(numChannels, kMaxChannels);
        for (int n = 0; n < numSamples; ++n)
        {
            for (int c = 0; c < numChannels; ++c)
                ch[c][n] = (float) processSample(c, ch[c][n]);
            bodyPos = (bodyPos + 1) % kBodyLineSize;
            advance();
        }
    }

private:
    static constexpr int kBodyLineSize = 512;   // >= max body delay (2*0.1/340*192k = 113) + margin

    double processSample(int c, double x)
    {
        const double dry = x;
        double y = x;

        // ---- pickup: From compensation stages (each with its own on/off fade) + To EQ ----
        if (!fadePickup.fullyOff())
        {
            double p = y;
            for (int i = 0; i < 4; ++i)
            {
                if (fadeFrom[i].fullyOff()) continue;
                double s = p * fromGain[i];
                for (int j = 0; j < fromNumSec[i]; ++j) s = fromState[c][i * 2 + j].process(fromCoef[i][j], s);
                const double g = fadeFrom[i].weight();
                p = (1.0 - g) * p + g * s;
            }
            p = hp.process(c, p);
            for (int j = 0; j < 5; ++j) p = eqState[c][j].process(eqCoef[j], p);
            p = lp.process(c, p);
            p *= eqOutGain;
            const double g = fadePickup.weight();
            y = (1.0 - g) * y + g * p;
        }

        // ---- body comb ----
        float* line = &bodyLine[(size_t) c * kBodyLineSize];
        line[bodyPos] = (float) y;   // stored at the stage input (float, like the original's streams)
        if (!fadeBody.fullyOff())
        {
            const double d = bodyDelay;
            const int k = (int) std::floor(d);
            const double fr = d - k;
            const double xk = line[(bodyPos - k + kBodyLineSize) % kBodyLineSize];
            const double xk1 = line[(bodyPos - k - 1 + kBodyLineSize) % kBodyLineSize];
            const double b = bodyAmountS.value();
            const double s = (1.0 - b * 0.5) * y + bodySignS.value() * (b * 0.5) * ((1.0 - fr) * xk + fr * xk1);
            const double g = fadeBody.weight();
            y = (1.0 - g) * y + g * s;
        }

        // ---- brightness, gain, volume, tone ----
        y = briState[c].process(briCoef, y);
        y *= gainLin;
        y = volState[c].process(volCoef, y * volGain);
        if (!fadeTone.fullyOff())
        {
            const double t = toneState[c].process(toneCoef, y);
            const double g = fadeTone.weight();
            y = (1.0 - g) * y + g * t;
        }

        // ---- bypass ----
        const double g = fadeBypass.weight();
        return (1.0 - g) * dry + g * y;
    }

    void advance()
    {
        bool eq = false, from = false, other = false;
        for (auto& s : eqVals) if (s.active()) { s.advance(); eq = true; }
        if (amountS.active()) { amountS.advance(); from = true; }
        for (auto* s : { &bodyAmountS, &bodySignS, &bodyTargetS, &briS, &gainS, &volS, &toneS })
            if (s->active()) { s->advance(); other = true; }
        bodyDelay += bodyGlide * (bodyTargetS.value() - bodyDelay);
        for (auto& f : fadeFrom) f.advance();
        fadePickup.advance(); fadeBody.advance(); fadeTone.advance(); fadeBypass.advance();
        if (eq) designEq();
        if (from) designFrom();
        if (other) designOther();
    }

    void redesignAll() { designEq(); designFrom(); designOther(); }

    void designEq()
    {
        double v[22];
        for (int k = 0; k < 22; ++k) v[k] = eqVals[k].value();
        hp.design(v[1], v[2], sr, quirk);
        eqCoef[0] = rbjLowShelf(v[3], v[4], sr);
        eqCoef[1] = rbjPeak(v[5], 1.0 / v[6], v[7], sr);
        eqCoef[2] = rbjPeak(v[8], 1.0 / v[9], v[10], sr);
        eqCoef[3] = rbjPeak(v[11], 1.0 / v[12], v[13], sr);
        eqCoef[4] = rbjHighShelf(v[14], v[15], sr);
        lp.design(v[16], v[17], sr, quirk);
        eqOutGain = std::pow(10.0, v[18] / 20.0);
    }

    void designFrom()
    {
        const double a = amountS.value();
        fromGain[0] = std::pow(10.0, 2.5 * a / 20.0); fromNumSec[0] = 1;
        fromCoef[0][0] = monoQuadHighShelf(3105.0, 5.5 * a, sr);
        fromGain[1] = std::pow(10.0, 7.0 * a / 20.0); fromNumSec[1] = 0;
        fromGain[2] = std::pow(10.0, 8.0 * a / 20.0); fromNumSec[2] = 1;
        fromCoef[2][0] = monoQuadHighShelf(3000.0, -5.5 * a, sr);
        fromGain[3] = std::pow(10.0, 9.0 * a / 20.0); fromNumSec[3] = 2;
        fromCoef[3][0] = rbjHighShelf(5800.0, -6.0 * a, sr);
        fromCoef[3][1] = rbjPeak(26.0, 0.5, -3.0 * a, sr);
    }

    void designOther()
    {
        briCoef = monoQuadHighShelf(1800.0, briS.value(), sr);
        gainLin = std::pow(10.0, gainS.value() / 20.0);
        const double v = volS.value();
        volGain = v * v;
        volCoef = monoQuadHighShelf(2250.0, -5.0 * (1.0 - v), sr);
        toneCoef = toneLowPass(toneS.value(), sr);
    }

    double sr = 44100.0;
    bool quirk = true, initialised = false;
    int rampLen = 512, modelRampLen = 882, lastPickupTo = -1;
    Parameters params;

    std::array<LinearSmoother, 22> eqVals {};
    LinearSmoother amountS, bodyAmountS, bodySignS, bodyTargetS, briS, gainS, volS, toneS;
    SwitchFade fadePickup, fadeBody, fadeTone, fadeBypass;
    std::array<SwitchFade, 4> fadeFrom {};

    FracButterworth hp { false }, lp { true };
    std::array<Coeffs, 5> eqCoef {};
    double eqOutGain = 1.0;
    std::array<std::array<DF1, 5>, kMaxChannels> eqState {};

    std::array<double, 4> fromGain {};
    std::array<int, 4> fromNumSec {};
    std::array<std::array<Coeffs, 2>, 4> fromCoef {};
    std::array<std::array<DF1, 8>, kMaxChannels> fromState {};

    std::vector<float> bodyLine;
    int bodyPos = 0;
    double bodyDelay = 0.0, bodyGlide = 0.0;

    Coeffs briCoef, volCoef, toneCoef;
    double gainLin = 1.0, volGain = 1.0;
    std::array<DF1, kMaxChannels> briState {}, volState {}, toneState {};
};

} // namespace pwref
