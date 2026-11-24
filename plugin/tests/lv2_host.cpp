// Minimal LV2 host (lilv) for testing; provides the features DPF requires (urid:map, options with
// block sizes + sample rate, boundedBlockLength). Same parameter syntax as pw_cli / clap_host:
//   lv2_host URI in.f32 out.f32 n bs [key=value ...] [@N:key=value,...]
// key=value: control port values set before activation; @N: control values changed before the
// block starting at N. "bypass" maps to the lv2:enabled port (inverted).
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::vector<std::string> gUris;
static LV2_URID mapUri(LV2_URID_Map_Handle, const char* uri)
{
    for (size_t i = 0; i < gUris.size(); ++i) if (gUris[i] == uri) return (LV2_URID) (i + 1);
    gUris.push_back(uri);
    return (LV2_URID) gUris.size();
}

static std::string symbolFor(const std::string& key)
{
    static const std::map<std::string, std::string> m = {
        { "bypass", "lv2_enabled" }, { "pickupEnabled", "pickup_enabled" }, { "pickupFrom", "pickup_from" },
        { "pickupTo", "pickup_to" }, { "amount", "amount" }, { "customX", "custom_x" }, { "customY", "custom_y" },
        { "bodyEnabled", "body_enabled" }, { "bodyThickness", "body_thickness" }, { "bodyAmount", "body_amount" },
        { "brightness", "brightness" }, { "gainDb", "gain" }, { "volume", "volume" }, { "tone", "tone" },
        { "bodyPhase", "body_phase" } };
    auto it = m.find(key);
    if (it == m.end()) { std::fprintf(stderr, "unknown key %s\n", key.c_str()); std::exit(2); }
    return it->second;
}

int main(int argc, char** argv)
{
    if (argc < 6) { std::fprintf(stderr, "usage: lv2_host URI in.f32 out.f32 n bs [params]\n"); return 2; }
    const int n = std::atoi(argv[4]);
    int32_t bs = std::atoi(argv[5]);

    LilvWorld* world = lilv_world_new();
    lilv_world_load_all(world);
    LilvNode* uri = lilv_new_uri(world, argv[1]);
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri);
    if (!plugin) { std::fprintf(stderr, "plugin not found\n"); return 1; }

    LV2_URID_Map map { nullptr, mapUri };
    int32_t minB = 1, maxB = bs, nomB = bs;
    float srate = 44100.0f;
    const LV2_URID tInt = mapUri(nullptr, LV2_ATOM__Int), tFloat = mapUri(nullptr, LV2_ATOM__Float);
    LV2_Options_Option opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), tInt, &minB },
        { LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), tInt, &maxB },
        { LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), tInt, &nomB },
        { LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_PARAMETERS__sampleRate), sizeof(float), tFloat, &srate },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr } };
    LV2_Feature fMap { LV2_URID__map, &map }, fOpts { LV2_OPTIONS__options, opts }, fBound { LV2_BUF_SIZE__boundedBlockLength, nullptr };
    const LV2_Feature* features[] = { &fMap, &fOpts, &fBound, nullptr };
    LilvInstance* inst = lilv_plugin_instantiate(plugin, 44100.0, features);
    if (!inst) { std::fprintf(stderr, "instantiate failed\n"); return 1; }

    const uint32_t np = lilv_plugin_get_num_ports(plugin);
    std::vector<float> ctl(np, 0.0f), mins(np), maxs(np), defs(np);
    lilv_plugin_get_port_ranges_float(plugin, mins.data(), maxs.data(), defs.data());
    LilvNode* audioC = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode* inputC = lilv_new_uri(world, LV2_CORE__InputPort);
    std::map<std::string, uint32_t> sym;
    std::vector<uint32_t> ain, aout;
    for (uint32_t i = 0; i < np; ++i)
    {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        sym[lilv_node_as_string(lilv_port_get_symbol(plugin, port))] = i;
        if (lilv_port_is_a(plugin, port, audioC)) (lilv_port_is_a(plugin, port, inputC) ? ain : aout).push_back(i);
        else { ctl[i] = defs[i]; lilv_instance_connect_port(inst, i, &ctl[i]); }
    }
    auto setKey = [&](const std::string& k, double v) {
        const uint32_t idx = sym.at(symbolFor(k));
        ctl[idx] = (float) (k == "bypass" ? 1.0 - v : v);
    };
    std::multimap<int, std::pair<std::string, double>> events;
    for (int i = 6; i < argc; ++i)
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
                size_t comma = rest.find(',', pos); if (comma == std::string::npos) comma = rest.size();
                const std::string kv = rest.substr(pos, comma - pos); const auto eq = kv.find('=');
                events.insert({ at, { kv.substr(0, eq), std::atof(kv.substr(eq + 1).c_str()) } });
                pos = comma + 1;
            }
        }
        else { const auto eq = a.find('='); setKey(a.substr(0, eq), std::atof(a.substr(eq + 1).c_str())); }
    }

    std::vector<float> x((size_t) n);
    FILE* f = std::fopen(argv[2], "rb"); if (!f) return 1;
    if (std::fread(x.data(), 4, (size_t) n, f) < (size_t) n) { std::fprintf(stderr, "short input\n"); return 1; }
    std::fclose(f);
    std::vector<float> inL(x), inR(x), outL((size_t) n), outR((size_t) n);

    lilv_instance_activate(inst);
    for (int s = 0; s < n; s += bs)
    {
        const int len = std::min((int) bs, n - s);
        for (auto it = events.begin(); it != events.end() && it->first <= s;)
        {
            setKey(it->second.first, it->second.second);
            it = events.erase(it);
        }
        lilv_instance_connect_port(inst, ain[0], inL.data() + s);
        lilv_instance_connect_port(inst, ain[1], inR.data() + s);
        lilv_instance_connect_port(inst, aout[0], outL.data() + s);
        lilv_instance_connect_port(inst, aout[1], outR.data() + s);
        lilv_instance_run(inst, (uint32_t) len);
    }
    lilv_instance_deactivate(inst);
    lilv_instance_free(inst);
    lilv_node_free(audioC); lilv_node_free(inputC); lilv_node_free(uri);
    lilv_world_free(world);

    f = std::fopen(argv[3], "wb");
    std::fwrite(outL.data(), 4, (size_t) n, f);
    std::fclose(f);
    return outL == outR ? 0 : 3;
}
