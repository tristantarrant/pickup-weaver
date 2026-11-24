// Minimal CLAP host for testing: renders a mono float32 file (duplicated to stereo) through a .clap
// plugin. Same parameter syntax as pw_cli:  key=value  (set before activation, like a restored session)
// and @SAMPLE:key=value,...  (sent as CLAP param events in the block starting at SAMPLE).
//   clap_host plugin.clap in.f32 out.f32 n bs [key=value ...] [@N:key=value,...]
// Output: channel 0 float32 (exit code 3 if L != R).
#include "clap/entry.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/host.h"
#include "clap/plugin-factory.h"
#include "clap/plugin.h"
#include "clap/process.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static const char* const kKeys[15] = { "bypass", "pickupEnabled", "pickupFrom", "pickupTo", "amount", "customX", "customY",
                                       "bodyEnabled", "bodyThickness", "bodyAmount", "brightness", "gainDb", "volume",
                                       "tone", "bodyPhase" };

static int keyIndex(const std::string& k)
{
    for (int i = 0; i < 15; ++i) if (k == kKeys[i]) return i;
    std::fprintf(stderr, "unknown key %s\n", k.c_str());
    std::exit(2);
}

static const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
static void hostNoop(const clap_host_t*) {}
static const clap_host_t kHost = { CLAP_VERSION, nullptr, "pw-test-host", "test", "", "1.0",
                                   hostGetExtension, hostNoop, hostNoop, hostNoop };

// event lists
struct EventList
{
    std::vector<clap_event_param_value_t> ev;
    static uint32_t size(const clap_input_events_t* l) { return (uint32_t) static_cast<EventList*>(l->ctx)->ev.size(); }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) { return &static_cast<EventList*>(l->ctx)->ev[i].header; }
    void add(uint32_t id, double v)
    {
        clap_event_param_value_t e {};
        e.header = { sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id; e.cookie = nullptr; e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1; e.value = v;
        ev.push_back(e);
    }
};
static bool outPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

int main(int argc, char** argv)
{
    if (argc < 6) { std::fprintf(stderr, "usage: clap_host plugin.clap in.f32 out.f32 n bs [params]\n"); return 2; }
    const int n = std::atoi(argv[4]), bs = std::atoi(argv[5]);
    std::map<int, double> statics;
    std::multimap<int, std::pair<int, double>> events;
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
                events.insert({ at, { keyIndex(kv.substr(0, eq)), std::atof(kv.substr(eq + 1).c_str()) } });
                pos = comma + 1;
            }
        }
        else { const auto eq = a.find('='); statics[keyIndex(a.substr(0, eq))] = std::atof(a.substr(eq + 1).c_str()); }
    }

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto* entry = (const clap_plugin_entry_t*) dlsym(lib, "clap_entry");
    if (!entry || !entry->init(argv[1])) { std::fprintf(stderr, "clap_entry init failed\n"); return 1; }
    auto* factory = (const clap_plugin_factory_t*) entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    std::fprintf(stderr, "plugin: %s (%s), %u plugin(s)\n", desc->name, desc->id, factory->get_plugin_count(factory));
    const clap_plugin_t* plug = factory->create_plugin(factory, &kHost, desc->id);
    if (!plug || !plug->init(plug)) { std::fprintf(stderr, "create/init failed\n"); return 1; }
    auto* params = (const clap_plugin_params_t*) plug->get_extension(plug, CLAP_EXT_PARAMS);
    std::fprintf(stderr, "params: %u\n", params->count(plug));

    // static values: params.flush before activation (like a host restoring a session)
    EventList in; clap_input_events_t inEv { &in, EventList::size, EventList::get };
    clap_output_events_t outEv { nullptr, outPush };
    for (auto& kv : statics) in.add((uint32_t) kv.first, kv.second);
    params->flush(plug, &inEv, &outEv);
    in.ev.clear();

    if (!plug->activate(plug, 44100.0, 1, (uint32_t) bs) || !plug->start_processing(plug)) { std::fprintf(stderr, "activate failed\n"); return 1; }

    std::vector<float> x((size_t) n);
    FILE* f = std::fopen(argv[2], "rb"); if (!f) return 1;
    const size_t got = std::fread(x.data(), 4, (size_t) n, f); std::fclose(f);
    if ((int) got < n) { std::fprintf(stderr, "short input\n"); return 1; }
    std::vector<float> inL(x), inR(x), outL((size_t) n), outR((size_t) n);

    for (int s = 0; s < n; s += bs)
    {
        const int len = std::min(bs, n - s);
        for (auto it = events.begin(); it != events.end() && it->first <= s;)
        {
            in.add((uint32_t) it->second.first, it->second.second);
            it = events.erase(it);
        }
        float* ip[2] = { inL.data() + s, inR.data() + s };
        float* op[2] = { outL.data() + s, outR.data() + s };
        clap_audio_buffer_t ib {}, ob {};
        ib.data32 = ip; ib.channel_count = 2;
        ob.data32 = op; ob.channel_count = 2;
        clap_process_t pr {};
        pr.steady_time = s; pr.frames_count = (uint32_t) len;
        pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        pr.in_events = &inEv; pr.out_events = &outEv;
        if (plug->process(plug, &pr) == CLAP_PROCESS_ERROR) { std::fprintf(stderr, "process error\n"); return 1; }
        in.ev.clear();
    }
    plug->stop_processing(plug);
    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();

    f = std::fopen(argv[3], "wb");
    std::fwrite(outL.data(), 4, (size_t) n, f);
    std::fclose(f);
    return outL == outR ? 0 : 3;
}
