// DPF wrapper around the PickupWeaverDSP core (../dsp).
// Parameters are the original's 15 controls, in plain units, with the original's ranges/defaults.
#include "DistrhoPlugin.hpp"
#include "PickupWeaverDSP.h"
#include "PluginParams.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

using namespace pw::plugin;

START_NAMESPACE_DISTRHO

class PickupWeaverPlugin : public Plugin
{
public:
    PickupWeaverPlugin()
        : Plugin(kParamCount, 0, 0)
    {
        for (uint32_t i = 0; i < kParamCount; ++i)
            values[i].store(kSpecs[i].def, std::memory_order_relaxed);
        prepareDsp(getSampleRate());
    }

protected:
    // ---- information ----
    const char* getLabel() const override { return "PickupWeaver"; }
    const char* getDescription() const override
    {
        return "Guitar pickup, hollow body and tone-pot simulation.";
    }
    const char* getMaker() const override { return DISTRHO_PLUGIN_BRAND; }
    const char* getLicense() const override { return "GPL-2.0-or-later"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }
    int64_t getUniqueId() const override { return d_cconst('p', 'k', 'W', 'v'); }

    // ---- init ----
    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(uint32_t index, Parameter& parameter) override
    {
        if (index == kBypass)
        {
            parameter.initDesignation(kParameterDesignationBypass);
            return;
        }
        const ParamSpec& s = kSpecs[index];
        parameter.hints = kParameterIsAutomatable;
        if (s.isBool)
            parameter.hints |= kParameterIsBoolean | kParameterIsInteger;
        else if (s.isInt)
            parameter.hints |= kParameterIsInteger;
        parameter.name = s.name;
        parameter.symbol = s.symbol;
        parameter.unit = s.unit;
        parameter.ranges.min = s.min;
        parameter.ranges.max = s.max;
        parameter.ranges.def = s.def;

        if (index == kPickupFrom || index == kPickupTo)
        {
            const uint8_t n = index == kPickupFrom ? 4 : 29;
            ParameterEnumerationValue* const ev = new ParameterEnumerationValue[n];   // freed by DPF
            for (uint8_t i = 0; i < n; ++i)
            {
                ev[i].value = i;
                ev[i].label = index == kPickupFrom ? kFromNames[i] : pw::tables::kPresetNames[i];
            }
            parameter.enumValues.count = n;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = ev;
        }
    }

    // ---- parameters (may be called outside the audio thread: atomics) ----
    float getParameterValue(uint32_t index) const override
    {
        return index < kParamCount ? values[index].load(std::memory_order_relaxed) : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index < kParamCount)
            values[index].store(value, std::memory_order_relaxed);
    }

    // ---- processing ----
    void activate() override { prepareDsp(getSampleRate()); }
    void sampleRateChanged(double newSampleRate) override { prepareDsp(newSampleRate); }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        for (int c = 0; c < 2; ++c)
            if (outputs[c] != inputs[c])
                std::memcpy(outputs[c], inputs[c], sizeof(float) * frames);
        // LV2/VST3 hosts deliver a restored state with the first block: apply it without smoothing.
        applyParameters(false, snapNextChange);
        snapNextChange = false;
        dsp.process(outputs, 2, static_cast<int>(frames));
    }

private:
    // Non-realtime: allocates the engine (picks NEON / AVX / SSE2 / scalar).
    void prepareDsp(double sampleRate)
    {
        applyParameters(true, true);
        dsp.prepare(sampleRate);
        snapNextChange = true;
    }

    // Pushes the current values to the DSP, only when something changed (a change starts the ramps,
    // unless instant).
    void applyParameters(bool force, bool instant)
    {
        bool changed = force;
        float v[kParamCount];
        for (uint32_t i = 0; i < kParamCount; ++i)
        {
            v[i] = values[i].load(std::memory_order_relaxed);
            if (v[i] != applied[i]) changed = true;
        }
        if (!changed)
            return;
        std::memcpy(applied, v, sizeof(applied));

        pw::Parameters p;
        p.bypass = v[kBypass] > 0.5f;
        p.pickupEnabled = v[kPickupEnabled] > 0.5f;
        p.pickupFrom = static_cast<int>(std::lround(v[kPickupFrom]));
        p.pickupTo = static_cast<int>(std::lround(v[kPickupTo]));
        p.amount = v[kAmount];
        p.customX = v[kCustomX];
        p.customY = v[kCustomY];
        p.bodyEnabled = v[kBodyEnabled] > 0.5f;
        p.bodyThickness = v[kBodyThickness];
        p.bodyAmount = v[kBodyAmount];
        p.brightness = v[kBrightness];
        p.gainDb = v[kGain];
        p.volume = v[kVolume];
        p.tone = v[kTone];
        p.bodyPhase = v[kBodyPhase] > 0.5f;
        dsp.setParameters(p, instant);
    }

    pw::PickupWeaverDSP dsp;
    std::atomic<float> values[kParamCount];
    float applied[kParamCount] = {};
    bool snapNextChange = true;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PickupWeaverPlugin)
};

Plugin* createPlugin()
{
    return new PickupWeaverPlugin();
}

END_NAMESPACE_DISTRHO
