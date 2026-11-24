// In-process benchmark: old scalar core (tests/PickupWeaverDSP_ref.h) vs the SIMD engine backends.
#include "../PickupWeaverDSP.h"
#include "PickupWeaverDSP_ref.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

template <class P>
static void params(P& p, int b, bool automate)
{
    p.pickupTo = 27; p.bodyAmount = 40; p.tone = 6; p.customX = 30; p.customY = 60;
    if (!automate) return;
    p.gainDb = (b % 40) - 20; p.tone = 2 + (b % 8); p.brightness = (b % 50) * 4 - 100;
    p.customX = b % 100; p.customY = (b * 7) % 100; p.volume = 4 + (b % 6);
    if (b % 64 == 0) p.pickupTo = (b / 64) % 29;
}

template <class DSP, class P>
static double run(DSP& dsp, int nch, bool automate, double seconds)
{
    const double sr = 44100; const int bs = 512;
    const int nblk = (int) (seconds * sr / bs);
    std::vector<std::vector<float>> buf((size_t) nch, std::vector<float>(bs));
    std::mt19937 rng(1); std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> noise((size_t) bs * 64);
    for (auto& v : noise) v = u(rng);
    P p; params(p, 0, false);
    dsp.setParameters(p);
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep)
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < nblk; ++b)
        {
            for (int c = 0; c < nch; ++c) std::copy_n(noise.begin() + (b % 64) * bs, bs, buf[(size_t) c].begin());
            if (automate) { params(p, b, true); dsp.setParameters(p); }
            float* ch[8]; for (int c = 0; c < nch; ++c) ch[c] = buf[(size_t) c].data();
            dsp.process(ch, nch, bs);
        }
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return best / seconds * 100.0;   // % of one core for real-time
}

int main()
{
    const double secs = 20.0;
    std::printf("%-12s %16s %16s %16s %16s\n", "engine", "2ch static", "2ch automation", "8ch static", "8ch automation");
    {
        pwref::PickupWeaverDSP d; d.prepare(44100);
        double r[4]; int i = 0;
        for (int nch : { 2, 8 }) for (bool a : { false, true }) r[i++] = run<pwref::PickupWeaverDSP, pwref::Parameters>(d, nch, a, secs);
        std::printf("%-12s %15.3f%% %15.3f%% %15.3f%% %15.3f%%\n", "old scalar", r[0], r[1], r[2], r[3]);
    }
    for (pw::Backend b : { pw::Backend::Scalar, pw::Backend::SSE2, pw::Backend::AVX, pw::Backend::NEON })
    {
        if (!pw::PickupWeaverDSP::isAvailable(b)) continue;
        pw::PickupWeaverDSP d; d.prepare(44100, pw::kDefaultSmoothingSeconds, true, b);
        double r[4]; int i = 0;
        for (int nch : { 2, 8 }) for (bool a : { false, true }) r[i++] = run<pw::PickupWeaverDSP, pw::Parameters>(d, nch, a, secs);
        std::printf("%-12s %15.3f%% %15.3f%% %15.3f%% %15.3f%%\n", d.backendName(), r[0], r[1], r[2], r[3]);
    }
    std::printf("(%% of one core needed for real time at 44.1 kHz, block 512; best of 3)\n");
    return 0;
}
