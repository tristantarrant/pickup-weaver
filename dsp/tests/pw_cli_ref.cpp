// pw_cli_ref -- same harness on the pre-SIMD scalar core (generated from pw_cli.cpp)
//   pw_cli sr=44100 bs=512 n=65536 [in=input.f32] out=output.f32 [smooth=0.0116] [quirk=1]
//          [amp=0.25] key=value ... @SAMPLE:key=value[,key=value...] ...
// Input: mono float32 (duplicated to 2 channels); without in=, a unit-amp impulse (amp=) is used.
// Output: channel 0 as float32. Parameter changes @N apply at the first block starting at >= N
// (use N on block boundaries).
#include "../PickupWeaverTables.h"
#include "PickupWeaverDSP_ref.h"   // pre-SIMD scalar core (regression reference)

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static void applyKV(pwref::Parameters& p, const std::string& k, double v)
{
    if (k == "bypass") p.bypass = v != 0;
    else if (k == "pickupEnabled") p.pickupEnabled = v != 0;
    else if (k == "pickupFrom") p.pickupFrom = (int) v;
    else if (k == "pickupTo") p.pickupTo = (int) v;
    else if (k == "amount") p.amount = v;
    else if (k == "customX") p.customX = v;
    else if (k == "customY") p.customY = v;
    else if (k == "bodyEnabled") p.bodyEnabled = v != 0;
    else if (k == "bodyThickness") p.bodyThickness = v;
    else if (k == "bodyAmount") p.bodyAmount = v;
    else if (k == "bodyPhase") p.bodyPhase = v != 0;
    else if (k == "brightness") p.brightness = v;
    else if (k == "gainDb") p.gainDb = v;
    else if (k == "volume") p.volume = v;
    else if (k == "tone") p.tone = v;
    else { std::fprintf(stderr, "unknown parameter %s\n", k.c_str()); std::exit(2); }
}

int main(int argc, char** argv)
{
    double sr = 44100, smooth = pwref::kDefaultSmoothingSeconds, amp = 1.0;
    int bs = 512, n = 65536;
    bool quirk = true;
    std::string in, out = "out.f32";
    pwref::Parameters p;
    std::multimap<int, std::pair<std::string, double>> events;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a[0] == '@')
        {
            const auto colon = a.find(':');
            const int at = std::atoi(a.substr(1, colon - 1).c_str());
            std::string rest = a.substr(colon + 1);
            size_t pos = 0;
            while (pos < rest.size())
            {
                size_t comma = rest.find(',', pos);
                if (comma == std::string::npos) comma = rest.size();
                const std::string kv = rest.substr(pos, comma - pos);
                const auto eq = kv.find('=');
                events.insert({ at, { kv.substr(0, eq), std::atof(kv.substr(eq + 1).c_str()) } });
                pos = comma + 1;
            }
            continue;
        }
        const auto eq = a.find('=');
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "sr") sr = std::atof(v.c_str());
        else if (k == "bs") bs = std::atoi(v.c_str());
        else if (k == "n") n = std::atoi(v.c_str());
        else if (k == "in") in = v;
        else if (k == "out") out = v;
        else if (k == "smooth") smooth = std::atof(v.c_str());
        else if (k == "quirk") quirk = std::atoi(v.c_str()) != 0;
        else if (k == "amp") amp = std::atof(v.c_str());
        else if (k == "backend") {}
        else applyKV(p, k, std::atof(v.c_str()));
    }
    std::vector<float> x((size_t) n, 0.0f);
    if (!in.empty())
    {
        FILE* f = std::fopen(in.c_str(), "rb");
        if (!f) { std::perror(in.c_str()); return 1; }
        const size_t got = std::fread(x.data(), sizeof(float), (size_t) n, f);
        std::fclose(f);
        if ((int) got < n) n = (int) got, x.resize(got);
    }
    else
        x[0] = (float) amp;

    pwref::PickupWeaverDSP dsp;
    dsp.setParameters(p);              // initial state (no ramps before prepare)
    dsp.prepare(sr, smooth, quirk);
    if (std::getenv("PW_VERBOSE")) std::fprintf(stderr, "backend: ref\n");
    int nch = 2;
    if (const char* e = std::getenv("PW_CHANNELS")) nch = std::max(1, std::min(pwref::kMaxChannels, std::atoi(e)));
    std::vector<std::vector<float>> bufs((size_t) nch, x);   // all channels carry the same input
    for (int s = 0; s < n; s += bs)
    {
        bool changed = false;
        for (auto it = events.begin(); it != events.end() && it->first <= s;)
        {
            applyKV(p, it->second.first, it->second.second);
            it = events.erase(it);
            changed = true;
        }
        if (changed) dsp.setParameters(p);
        float* ch[pwref::kMaxChannels];
        for (int c = 0; c < nch; ++c) ch[c] = bufs[(size_t) c].data() + s;
        dsp.process(ch, nch, std::min(bs, n - s));
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    for (int c = 1; c < nch; ++c)   // all channels must be identical (same input, shared coefficients)
        if (bufs[(size_t) c] != bufs[0]) { std::fprintf(stderr, "channel %d differs from channel 0\n", c); return 3; }
    std::fwrite(bufs[0].data(), sizeof(float), bufs[0].size(), f);
    std::fclose(f);
    return 0;
}
