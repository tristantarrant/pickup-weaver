// Accuracy of the vector math vs long double libm, and a checksum of the exact output bits
// (the checksum must be identical for every backend and platform).
#include "../PickupWeaverSIMD.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pw::PW_ARCH_NS::simd;

static double ulpErr(double got, long double ref)
{
    const double r = (double) ref;
    const double ulp = std::nextafter(std::fabs(r), INFINITY) - std::fabs(r);
    return (double) (std::fabs((long double) got - ref) / ulp);
}

template <class V>
static void run(std::uint64_t& hash)
{
    const int N = 200000;
    std::vector<double> x(N + 4), s(N + 4), c(N + 4), p(N + 4);
    double es = 0, ec = 0, ep = 0, xs = 0, xp = 0;
    // sincos over [0, 3.3] (DSP range: 2*pi*f/sr <= pi) plus a wider sweep
    for (int i = 0; i < N; ++i) x[i] = (i < N / 2) ? 3.3 * i / (N / 2) : -50.0 + 100.0 * (i - N / 2) / (N / 2);
    batchSinCos<V>(x.data(), s.data(), c.data(), N);
    for (int i = 0; i < N; ++i)
    {
        const double a = ulpErr(s[i], sinl((long double) x[i])), b = ulpErr(c[i], cosl((long double) x[i]));
        if (std::fabs(sinl((long double) x[i])) > 1e-300 && a > es) { es = a; xs = x[i]; }
        if (std::fabs(cosl((long double) x[i])) > 1e-300 && b > ec) ec = b;
    }
    // pow10 over [-4, 4] (gains/40, gains/20, tone exponents)
    for (int i = 0; i < N; ++i) x[i] = -4.0 + 8.0 * i / (N - 1);
    batchPow10<V>(x.data(), p.data(), N);
    for (int i = 0; i < N; ++i)
    {
        const double a = ulpErr(p[i], powl(10.0L, (long double) x[i]));
        if (a > ep) { ep = a; xp = x[i]; }
    }
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) { std::uint64_t b; std::memcpy(&b, &v, 8); h = (h ^ b) * 1099511628211ull; };
    for (int i = 0; i < N; ++i) { mix(s[i]); mix(c[i]); mix(p[i]); }
    hash = h;
    std::printf("%-6s max ulp: sin %.2f (x=%.4g) cos %.2f pow10 %.2f (x=%.4g)  bits-hash %016llx\n", V::name, es, xs, ec, ep, xp,
                (unsigned long long) h);
}

int main()
{
    std::uint64_t h0 = 0, h = 0;
    run<VScalar>(h0);
#if defined(PW_HAS_SSE2)
    run<VSSE2>(h); if (h != h0) std::printf("  MISMATCH vs scalar\n");
#endif
#if defined(PW_HAS_AVX)
    run<VAVX>(h); if (h != h0) std::printf("  MISMATCH vs scalar\n");
#endif
#if defined(PW_HAS_NEON)
    run<VNEON>(h); if (h != h0) std::printf("  MISMATCH vs scalar\n");
#endif
    return 0;
}
