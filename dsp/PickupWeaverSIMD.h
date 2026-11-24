// ============================================================================
// PickupWeaverSIMD.h -- minimal double-precision SIMD layer for PickupWeaverDSP.
//
// Backends (all expose the same static interface, W = lanes):
//   VScalar  W=1  portable fallback
//   VSSE2    W=2  x86-64 baseline
//   VAVX     W=4  x86 AVX (only when the translation unit is compiled with AVX enabled)
//   VNEON    W=2  ARM64 (AArch64 NEON has double-precision vectors; 32-bit ARM uses VScalar)
//
// Vector math (vsincos, vpow10) is written ONCE against that interface, using only
// IEEE add/sub/mul/div and exact bit manipulation -- no FMA, no libm -- so every backend
// produces bit-identical results (given that the compiler doesn't contract a*b+c; see
// PW_FP_STRICT_BEGIN in PickupWeaverDSP.h).
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// Strict IEEE semantics for all DSP code, regardless of the project's flags (DPF's Makefiles use
// -ffast-math): no reassociation (the Cody-Waite range reduction and the bit-identity across backends
// depend on the written operation order) and no FMA contraction. Restored after the headers.
#if defined(__clang__)
#define PW_FP_STRICT_BEGIN _Pragma("float_control(push)") _Pragma("float_control(precise, on)") _Pragma("clang fp contract(off)")
#define PW_FP_STRICT_END _Pragma("float_control(pop)")
#elif defined(__GNUC__)
#define PW_FP_STRICT_BEGIN _Pragma("GCC push_options") _Pragma("GCC optimize(\"no-fast-math,no-unsafe-math-optimizations,no-associative-math,no-reciprocal-math,fp-contract=off\")")
#define PW_FP_STRICT_END _Pragma("GCC pop_options")
#elif defined(_MSC_VER)
#define PW_FP_STRICT_BEGIN __pragma(float_control(precise, on, push)) __pragma(fp_contract(off))
#define PW_FP_STRICT_END __pragma(float_control(pop))
#else
#define PW_FP_STRICT_BEGIN
#define PW_FP_STRICT_END
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define PW_HAS_SSE2 1
#include <emmintrin.h>
#endif
#if defined(__AVX__)
#define PW_HAS_AVX 1
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define PW_HAS_NEON 1
#include <arm_neon.h>
#endif

// Every inline function of the DSP lives in an architecture-specific namespace, so a translation
// unit compiled with -mavx (PickupWeaverDSP_AVX.cpp) can never donate AVX-encoded copies of shared
// inline functions to the baseline build at link time (classic ODR pitfall of runtime dispatch).
#ifndef PW_ARCH_NS
#if defined(PW_HAS_AVX)
#define PW_ARCH_NS arch_avx
#else
#define PW_ARCH_NS arch_base
#endif
#endif

PW_FP_STRICT_BEGIN
namespace pw {
namespace PW_ARCH_NS {
namespace simd {

// Value barrier: an empty asm that pins a value in a register, so the compiler can't reassociate
// across it even under -ffast-math (the intrinsics are inline functions compiled with the global
// flags, so pragmas alone can't protect vector code). Emits no instruction.
template <class T>
inline T barrier(T a)
{
#if defined(__GNUC__) && (defined(__x86_64__) || (defined(__i386__) && defined(__SSE2__)))
    __asm__("" : "+x"(a));
#elif defined(__GNUC__) && defined(__aarch64__)
    __asm__("" : "+w"(a));
#endif
    return a;
}

// ---------------------------------------------------------------------------
struct VScalar
{
    using T = double;
    static constexpr int W = 1;
    static constexpr const char* name = "scalar";
    static T set1(double a) { return a; }
    static T zero() { return 0.0; }
    static T load(const double* p) { return *p; }
    static void store(double* p, T a) { *p = a; }
    static T add(T a, T b) { return a + b; }
    static T sub(T a, T b) { return a - b; }
    static T mul(T a, T b) { return a * b; }
    static T div(T a, T b) { return a / b; }
    static T neg(T a) { return -a; }
    // |a| < t ? 0 : a   (denormal/underflow flush, same predicate as the original kernels)
    static T flushSmall(T a, T t) { return (a < t && a > -t) ? 0.0 : a; }
    // select(m != 0 ? b : a) where m is a 0/1-valued mask from eq()
    static T eq(T a, T b) { return a == b ? 1.0 : 0.0; }
    static T select(T m, T a, T b) { return m != 0.0 ? b : a; }
    static T roundNearest(T a) { return std::nearbyint(a); }   // current mode = to nearest (even)
    static T loadFloat(const float* p) { return (double) *p; }
    static void storeFloat(float* p, T a) { *p = (float) a; }
    // 2^n for integer-valued n in [-1022, 1023]
    static T pow2i(T n)
    {
        const std::uint64_t bits = (std::uint64_t) ((std::int64_t) n + 1023) << 52;
        double r;
        std::memcpy(&r, &bits, 8);
        return r;
    }
};

#if defined(PW_HAS_SSE2)
struct VSSE2
{
    using T = __m128d;
    static constexpr int W = 2;
    static constexpr const char* name = "sse2";
    static T set1(double a) { return _mm_set1_pd(a); }
    static T zero() { return _mm_setzero_pd(); }
    static T load(const double* p) { return _mm_loadu_pd(p); }
    static void store(double* p, T a) { _mm_storeu_pd(p, a); }
    static T add(T a, T b) { return _mm_add_pd(a, b); }
    static T sub(T a, T b) { return _mm_sub_pd(a, b); }
    static T mul(T a, T b) { return _mm_mul_pd(a, b); }
    static T div(T a, T b) { return _mm_div_pd(a, b); }
    static T neg(T a) { return _mm_xor_pd(a, _mm_set1_pd(-0.0)); }
    static T flushSmall(T a, T t)
    {
        const T absA = _mm_andnot_pd(_mm_set1_pd(-0.0), a);
        return _mm_andnot_pd(_mm_cmplt_pd(absA, t), a);
    }
    static T eq(T a, T b) { return _mm_cmpeq_pd(a, b); }
    static T select(T m, T a, T b) { return _mm_or_pd(_mm_and_pd(m, b), _mm_andnot_pd(m, a)); }
    static T roundNearest(T a) { return _mm_cvtepi32_pd(_mm_cvtpd_epi32(a)); }   // MXCSR nearest-even, |a| < 2^31
    static T loadFloat(const float* p)
    {
        return _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))));
    }
    static void storeFloat(float* p, T a)
    {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_castps_si128(_mm_cvtpd_ps(a)));
    }
    static T pow2i(T n)
    {
        // (n + 1023) + 2^52 puts the biased exponent in the low mantissa bits; shift it into place.
        const T e = _mm_add_pd(_mm_add_pd(n, _mm_set1_pd(1023.0)), _mm_set1_pd(4503599627370496.0));
        return _mm_castsi128_pd(_mm_slli_epi64(_mm_castpd_si128(e), 52));
    }
};
#endif

#if defined(PW_HAS_AVX)
struct VAVX
{
    using T = __m256d;
    static constexpr int W = 4;
    static constexpr const char* name = "avx";
    static T set1(double a) { return _mm256_set1_pd(a); }
    static T zero() { return _mm256_setzero_pd(); }
    static T load(const double* p) { return _mm256_loadu_pd(p); }
    static void store(double* p, T a) { _mm256_storeu_pd(p, a); }
    static T add(T a, T b) { return _mm256_add_pd(a, b); }
    static T sub(T a, T b) { return _mm256_sub_pd(a, b); }
    static T mul(T a, T b) { return _mm256_mul_pd(a, b); }
    static T div(T a, T b) { return _mm256_div_pd(a, b); }
    static T neg(T a) { return _mm256_xor_pd(a, _mm256_set1_pd(-0.0)); }
    static T flushSmall(T a, T t)
    {
        const T absA = _mm256_andnot_pd(_mm256_set1_pd(-0.0), a);
        return _mm256_andnot_pd(_mm256_cmp_pd(absA, t, _CMP_LT_OQ), a);
    }
    static T eq(T a, T b) { return _mm256_cmp_pd(a, b, _CMP_EQ_OQ); }
    static T select(T m, T a, T b) { return _mm256_blendv_pd(a, b, m); }
    static T roundNearest(T a) { return _mm256_round_pd(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    static T loadFloat(const float* p) { return _mm256_cvtps_pd(_mm_loadu_ps(p)); }
    static void storeFloat(float* p, T a) { _mm_storeu_ps(p, _mm256_cvtpd_ps(a)); }
    static T pow2i(T n)
    {
        // AVX1 has no 256-bit integer shifts: do the exponent shift on the two 128-bit halves.
        const T e = _mm256_add_pd(_mm256_add_pd(n, _mm256_set1_pd(1023.0)), _mm256_set1_pd(4503599627370496.0));
        const __m128i lo = _mm_slli_epi64(_mm_castpd_si128(_mm256_castpd256_pd128(e)), 52);
        const __m128i hi = _mm_slli_epi64(_mm_castpd_si128(_mm256_extractf128_pd(e, 1)), 52);
        return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_castsi128_pd(lo)), _mm_castsi128_pd(hi), 1);
    }
};
#endif

#if defined(PW_HAS_NEON)
struct VNEON
{
    using T = float64x2_t;
    static constexpr int W = 2;
    static constexpr const char* name = "neon";
    static T set1(double a) { return vdupq_n_f64(a); }
    static T zero() { return vdupq_n_f64(0.0); }
    static T load(const double* p) { return vld1q_f64(p); }
    static void store(double* p, T a) { vst1q_f64(p, a); }
    static T add(T a, T b) { return vaddq_f64(a, b); }
    static T sub(T a, T b) { return vsubq_f64(a, b); }
    static T mul(T a, T b) { return vmulq_f64(a, b); }
    static T div(T a, T b) { return vdivq_f64(a, b); }
    static T neg(T a) { return vnegq_f64(a); }
    static T flushSmall(T a, T t)
    {
        const uint64x2_t small = vcltq_f64(vabsq_f64(a), t);
        return vreinterpretq_f64_u64(vbicq_u64(vreinterpretq_u64_f64(a), small));
    }
    static T eq(T a, T b) { return vreinterpretq_f64_u64(vceqq_f64(a, b)); }
    static T select(T m, T a, T b) { return vbslq_f64(vreinterpretq_u64_f64(m), b, a); }
    static T roundNearest(T a) { return vrndnq_f64(a); }
    static T loadFloat(const float* p) { return vcvt_f64_f32(vld1_f32(p)); }
    static void storeFloat(float* p, T a) { vst1_f32(p, vcvt_f32_f64(a)); }
    static T pow2i(T n)
    {
        const T e = vaddq_f64(vaddq_f64(n, vdupq_n_f64(1023.0)), vdupq_n_f64(4503599627370496.0));
        return vreinterpretq_f64_u64(vshlq_n_u64(vreinterpretq_u64_f64(e), 52));
    }
};
#endif

// ---------------------------------------------------------------------------
// Vector math (Cephes-derived, ~1 ulp). Arguments are within the ranges the DSP uses:
//   vsincos: |x| < 2^20 ; vpow10: |x| < 300.
// ---------------------------------------------------------------------------
// Round half to even with a rounding INSTRUCTION (not the (x + 1.5*2^52) - 1.5*2^52 trick, which
// -ffast-math may fold away). |x| < 2^31 here.
template <class V>
inline typename V::T vroundNearest(typename V::T x) { return V::roundNearest(x); }

template <class V>
inline typename V::T vpoly(typename V::T z, const double* c, int n)   // c[0]*z^n + ... + c[n]
{
    auto r = V::set1(c[0]);
    for (int i = 1; i <= n; ++i) r = V::add(V::mul(r, z), V::set1(c[i]));
    return r;
}

template <class V>
inline void vsincos(typename V::T x, typename V::T& s, typename V::T& c)
{
    using T = typename V::T;
    static const double sincof[6] = { 1.58962301576546568060E-10, -2.50507477628578072866E-8, 2.75573136213857245213E-6,
                                      -1.98412698295895385996E-4, 8.33333333332211858878E-3, -1.66666666666666307295E-1 };
    static const double coscof[6] = { -1.13585365213876817300E-11, 2.08757008419747316778E-9, -2.75573141792967388112E-7,
                                      2.48015872888517045348E-5, -1.38888888888730564116E-3, 4.16666666666665929218E-2 };
    // quadrant n = round(x * 2/pi); r = x - n*pi/2 (3-part Cody-Waite, pieces exact doubles)
    const T n = barrier(vroundNearest<V>(V::mul(x, V::set1(0.63661977236758134308))));
    T r = barrier(V::sub(x, V::mul(n, V::set1(1.57079625129699707031))));
    r = barrier(V::sub(r, V::mul(n, V::set1(7.54978941586159635335E-8))));
    r = barrier(V::sub(r, V::mul(n, V::set1(5.39030285815811905290E-15))));
    const T z = V::mul(r, r);
    const T sr = V::add(r, V::mul(V::mul(r, z), vpoly<V>(z, sincof, 5)));
    const T cr = V::add(V::sub(V::set1(1.0), V::mul(V::set1(0.5), z)), V::mul(V::mul(z, z), vpoly<V>(z, coscof, 5)));
    // q = n mod 4 (exact: n is a small integer)
    const T q = V::sub(n, V::mul(V::set1(4.0), vroundNearest<V>(V::mul(V::sub(n, V::set1(1.5)), V::set1(0.25)))));
    const T q1 = V::eq(q, V::set1(1.0)), q2 = V::eq(q, V::set1(2.0)), q3 = V::eq(q, V::set1(3.0));
    // q0: (sr, cr)  q1: (cr, -sr)  q2: (-sr, -cr)  q3: (-cr, sr)
    T ss = V::select(q1, sr, cr);
    ss = V::select(q2, ss, V::neg(sr));
    ss = V::select(q3, ss, V::neg(cr));
    T cc = V::select(q1, cr, V::neg(sr));
    cc = V::select(q2, cc, V::neg(cr));
    cc = V::select(q3, cc, sr);
    s = ss;
    c = cc;
}

template <class V>
inline typename V::T vpow10(typename V::T x)
{
    using T = typename V::T;
    static const double P[4] = { 4.09962519798587023075E-2, 1.17452732554344059015E1, 4.06717289936872725516E2,
                                 2.39423741207388267439E3 };
    static const double Q[4] = { 1.0, 8.50936160849306532625E1, 1.27209271178345121210E3, 2.07960819286001865907E3 };
    // 10^x = 2^n * 10^r, n = round(x * log2(10)), r = x - n*log10(2) (2-part)
    const T n = barrier(vroundNearest<V>(V::mul(x, V::set1(3.32192809488736234787))));
    T r = barrier(V::sub(x, V::mul(n, V::set1(3.01025390625000000000E-1))));
    r = barrier(V::sub(r, V::mul(n, V::set1(4.60503898119521373889E-6))));
    const T rr = V::mul(r, r);
    const T px = V::mul(r, vpoly<V>(rr, P, 3));
    T y = V::div(px, V::sub(vpoly<V>(rr, Q, 3), px));
    y = V::add(V::set1(1.0), V::add(y, y));
    return V::mul(y, V::pow2i(n));
}

// Batched evaluation of n scalar arguments, W at a time. The buffers must have room for n rounded
// up to a multiple of W; the padding lanes are filled with a safe argument and their results
// discarded. Loading whole vectors straight from the argument buffer avoids rebuilding each vector
// from scalar stores (a store-forwarding stall on x86 when the load follows the stores closely).
template <class V>
inline void batchSinCos(double* in, double* s, double* c, int n)
{
    for (int k = n; k % V::W != 0; ++k) in[k] = 0.0;
    for (int i = 0; i < n; i += V::W)
    {
        typename V::T vs, vc;
        vsincos<V>(V::load(in + i), vs, vc);
        V::store(s + i, vs);
        V::store(c + i, vc);
    }
}

template <class V>
inline void batchPow10(double* in, double* out, int n)
{
    for (int k = n; k % V::W != 0; ++k) in[k] = 0.0;
    for (int i = 0; i < n; i += V::W) V::store(out + i, vpow10<V>(V::load(in + i)));
}

} // namespace simd
} // namespace PW_ARCH_NS
} // namespace pw
PW_FP_STRICT_END
