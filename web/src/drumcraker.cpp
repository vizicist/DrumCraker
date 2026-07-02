// DrumCraker WebAssembly engine
// ------------------------------
// A JUCE-free port of the DrumCraker audio core (src/SampleEngine.cpp,
// src/VoiceManager.cpp, src/PluginProcessor.cpp) built for the browser.
//
// The heavy lifting that native DrumCraker gets from JUCE -- audio file
// decoding, XML parsing, resampling -- is done in JavaScript before samples
// reach this module.  What lives here is the part that actually defines the
// DrumCraker "sound": velocity-layer selection, anti-machine-gun round robin,
// Gaussian velocity/timing humanization, polyphonic voice management with
// smart stealing, and multichannel L/R mixing with RMS gain compensation.
//
// The code is compiled to a standalone WASM reactor module (see build.sh) and
// driven from an AudioWorklet, so the whole render path runs on the audio
// thread with no allocations per block -- mirroring the lock-free design of
// the native plugin.

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "lockfree_random.h"

#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE __attribute__((used))
#endif

// Routing of a single audiofile channel into the stereo main mix.
enum Routing { ROUTE_LEFT = 0, ROUTE_RIGHT = 1, ROUTE_BOTH = 2 };

static constexpr int   kMaxVoices   = 128;   // matches VoiceManager::maxVoices
static constexpr int   kMaxBlock    = 2048;  // upper bound on render block size
static constexpr float kTwoPi       = 6.28318530717958647692f;

// ----------------------------------------------------------------------------
// Kit data model (mirrors DrumKitLoader.h, minus the disk/JUCE bits)
// ----------------------------------------------------------------------------

struct SampleBuffer
{
    std::vector<float> data;   // mono PCM at engine sample rate
};

struct AudioRef
{
    int bufferId = -1;
    int routing  = ROUTE_BOTH;
};

struct DrumSample
{
    float power = 1.0f;
    std::vector<AudioRef> refs;
};

struct Instrument
{
    std::vector<DrumSample> samples;
    // Round-robin memory: index of the last sample played for this instrument.
    int lastSampleIndex = -1;
};

// ----------------------------------------------------------------------------
// Voice
// ----------------------------------------------------------------------------

struct Voice
{
    bool  active   = false;
    int   position = 0;
    int   startOffset = 0;   // remaining samples until the note actually sounds
    float finalGain = 1.0f;

    // Cached per-channel render info (resolved once at note-on, VoiceManager style)
    static constexpr int kMaxRefs = 16;
    const float* chanData[kMaxRefs];
    int          chanLen [kMaxRefs];
    int          chanRoute[kMaxRefs];
    int          numChans = 0;
    int          voiceLen = 0;   // length of channel 0, used for lifetime
};

// ----------------------------------------------------------------------------
// Engine
// ----------------------------------------------------------------------------

class Engine
{
public:
    void init(float sr)
    {
        sampleRate = sr > 0.0f ? sr : 44100.0f;
        for (auto& v : voices) v = Voice{};
    }

    // --- kit building -------------------------------------------------------
    void clearKit()
    {
        instruments.clear();
        buffers.clear();
        for (int i = 0; i < 128; ++i) midiMap[i] = -1;
        for (auto& v : voices) v.active = false;
        kitReady = false;
    }

    int createBuffer(int numFrames)
    {
        buffers.emplace_back();
        buffers.back().data.assign((size_t) std::max(0, numFrames), 0.0f);
        return (int) buffers.size() - 1;
    }

    float* bufferPtr(int id)
    {
        if (id < 0 || id >= (int) buffers.size()) return nullptr;
        return buffers[(size_t) id].data.data();
    }

    int addInstrument()
    {
        instruments.emplace_back();
        return (int) instruments.size() - 1;
    }

    int addSample(int instr, float power)
    {
        if (instr < 0 || instr >= (int) instruments.size()) return -1;
        instruments[(size_t) instr].samples.push_back({ power, {} });
        return (int) instruments[(size_t) instr].samples.size() - 1;
    }

    void addAudioRef(int instr, int sample, int bufferId, int routing)
    {
        if (instr < 0 || instr >= (int) instruments.size()) return;
        auto& samples = instruments[(size_t) instr].samples;
        if (sample < 0 || sample >= (int) samples.size()) return;
        samples[(size_t) sample].refs.push_back({ bufferId, routing });
    }

    void setMidi(int note, int instr)
    {
        if (note >= 0 && note < 128) midiMap[note] = instr;
    }

    void finalizeKit()
    {
        for (auto& inst : instruments) inst.lastSampleIndex = -1;
        kitReady = !instruments.empty();
    }

    // --- parameters ---------------------------------------------------------
    void setParams(float masterDb, float velRnd, float timingMs, float roundRobin)
    {
        masterGain = std::pow(10.0f, masterDb / 20.0f); // decibelsToGain
        velHumanize = velRnd;
        timingHumanize = timingMs;
        rrAmount = roundRobin;
    }

    void seed(uint32_t s) { humanizeRng.seed(s); rrRng.seed(s ^ 0x1234567u); }

    // --- note on ------------------------------------------------------------
    // Faithful port of DrumSamplerProcessor::processBlock humanization + the
    // VoiceManager::noteOn voice allocation / stealing.
    void noteOn(int note, float velocity)
    {
        if (!kitReady) return;

        int sampleOffset = 0;

        if (velHumanize > 0.001f)
        {
            float u1 = humanizeRng.nextFloat() * 0.9999f + 0.0001f;
            float u2 = humanizeRng.nextFloat();
            float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::cos(kTwoPi * u2);
            velocity = clampf(velocity + gaussian * velHumanize, 0.05f, 1.0f);
        }

        if (timingHumanize > 0.001f)
        {
            float u1 = humanizeRng.nextFloat() * 0.9999f + 0.0001f;
            float u2 = humanizeRng.nextFloat();
            float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::sin(kTwoPi * u2);
            float velocityBias = (velocity - 0.5f) * 0.4f;
            int sampleDelay = (int) (((gaussian * 0.5f + velocityBias) * timingHumanize / 1000.0f) * sampleRate);
            sampleOffset += sampleDelay;
            if (sampleOffset < 0) sampleOffset = 0;
        }

        int instrIdx = 0;
        const DrumSample* sample = getSampleForNote(note, velocity, rrAmount, instrIdx);
        if (!sample) return;

        startVoice(sample, velocity, sampleOffset);
    }

    // --- render -------------------------------------------------------------
    // Renders `numFrames` of the summed stereo main mix into outL/outR
    // (VoiceManager::renderNextBlock semantics), then applies master gain.
    void render(int numFrames)
    {
        if (numFrames > kMaxBlock) numFrames = kMaxBlock;

        for (int i = 0; i < numFrames; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }

        for (auto& v : voices)
            if (v.active) renderVoice(v, numFrames);

        for (auto& v : voices)
            if (v.active) advanceVoice(v, numFrames);

        for (int i = 0; i < numFrames; ++i) { outL[i] *= masterGain; outR[i] *= masterGain; }
    }

    float* outLPtr() { return outL; }
    float* outRPtr() { return outR; }

    int activeVoices() const
    {
        int n = 0;
        for (const auto& v : voices) if (v.active) ++n;
        return n;
    }

private:
    static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

    // ---- sample selection (port of SampleEngine::getSampleForNote) ---------
    const DrumSample* getSampleForNote(int midiNote, float velocity, float roundRobinAmount, int& outInstr)
    {
        if (midiNote < 0 || midiNote >= 128) return nullptr;
        int instrIdx = midiMap[midiNote];
        if (instrIdx < 0 || instrIdx >= (int) instruments.size()) return nullptr;
        outInstr = instrIdx;

        Instrument& inst = instruments[(size_t) instrIdx];
        if (inst.samples.empty()) return nullptr;

        float minPower = inst.samples[0].power;
        float maxPower = inst.samples[0].power;
        for (const auto& s : inst.samples)
        {
            minPower = std::min(minPower, s.power);
            maxPower = std::max(maxPower, s.power);
        }
        minPower = clampf(minPower, 0.0f, 1.0f);
        maxPower = clampf(maxPower, 0.0f, 1.0f);

        const int numSamples = (int) inst.samples.size();

        // Equal-power branch: kits without velocity layers, pure round robin.
        if (std::fabs(maxPower - minPower) < 0.001f)
        {
            if (numSamples == 1) return &inst.samples[0];

            const int lastIndex = inst.lastSampleIndex;
            int selectedIndex = 0;

            if (roundRobinAmount < 0.01f)
            {
                selectedIndex = 0;
            }
            else if (roundRobinAmount > 0.99f)
            {
                selectedIndex = (lastIndex >= 0 && lastIndex < numSamples)
                    ? (lastIndex + 1) % numSamples : 0;
            }
            else
            {
                const int nextIndex = (lastIndex >= 0 && lastIndex < numSamples)
                    ? (lastIndex + 1) % numSamples : 0;

                float totalWeight = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                    totalWeight += equalPowerWeight(i, lastIndex, nextIndex, roundRobinAmount);

                float randomValue = rrRng.nextFloat() * totalWeight;
                float cumulative = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    cumulative += equalPowerWeight(i, lastIndex, nextIndex, roundRobinAmount);
                    if (randomValue <= cumulative) { selectedIndex = i; break; }
                }
            }

            inst.lastSampleIndex = selectedIndex;
            return &inst.samples[(size_t) selectedIndex];
        }

        // Velocity-layer branch.
        float normalizedVelocity = minPower + (velocity * (maxPower - minPower));
        float tolerance = (maxPower - minPower) * 0.25f;

        static constexpr int maxCandidates = 8;
        const DrumSample* candidatePool[maxCandidates];
        int numCandidates = 0;

        for (const auto& s : inst.samples)
        {
            float diff = std::fabs(normalizedVelocity - s.power);
            if (diff < tolerance && numCandidates < maxCandidates)
                candidatePool[numCandidates++] = &s;
        }

        if (numCandidates == 0)
        {
            // Fall back to the 4 closest samples by power.
            std::vector<std::pair<const DrumSample*, float>> withDiff;
            withDiff.reserve(inst.samples.size());
            for (const auto& s : inst.samples)
                withDiff.push_back({ &s, std::fabs(normalizedVelocity - s.power) });
            std::sort(withDiff.begin(), withDiff.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            numCandidates = std::min(4, (int) withDiff.size());
            for (int i = 0; i < numCandidates; ++i) candidatePool[i] = withDiff[(size_t) i].first;
        }

        if (numCandidates == 0) return &inst.samples[0];

        int lastIndex = inst.lastSampleIndex;
        int selectedIndex = 0;

        if (numCandidates == 1)
        {
            selectedIndex = 0;
        }
        else if (roundRobinAmount < 0.01f)
        {
            float minDiff = std::fabs(normalizedVelocity - candidatePool[0]->power);
            for (int i = 1; i < numCandidates; ++i)
            {
                float diff = std::fabs(normalizedVelocity - candidatePool[i]->power);
                if (diff < minDiff) { minDiff = diff; selectedIndex = i; }
            }
        }
        else if (roundRobinAmount > 0.99f)
        {
            std::pair<int, float> indexed[maxCandidates];
            for (int i = 0; i < numCandidates; ++i)
                indexed[i] = { i, std::fabs(normalizedVelocity - candidatePool[i]->power) };
            std::sort(indexed, indexed + numCandidates,
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            int poolSize = std::min(4, numCandidates);
            int nextIndex = (lastIndex + 1) % poolSize;
            selectedIndex = indexed[nextIndex].first;
        }
        else
        {
            std::pair<int, float> weighted[maxCandidates];
            int numWeighted = 0;
            float totalWeight = 0.0f;
            for (int i = 0; i < numCandidates; ++i)
            {
                float diff = std::fabs(normalizedVelocity - candidatePool[i]->power);
                float weight = 1.0f / (1.0f + diff * 5.0f);
                if (i == lastIndex)
                {
                    float penalty = 0.1f - (roundRobinAmount * 0.08f);
                    weight *= std::max(0.01f, penalty);
                }
                else if (i == (lastIndex + 1) % numCandidates)
                {
                    weight *= (1.0f + roundRobinAmount * 1.5f);
                }
                weighted[numWeighted++] = { i, weight };
                totalWeight += weight;
            }
            float randomValue = rrRng.nextFloat() * totalWeight;
            float cumulative = 0.0f;
            for (int i = 0; i < numWeighted; ++i)
            {
                cumulative += weighted[i].second;
                if (randomValue <= cumulative) { selectedIndex = weighted[i].first; break; }
            }
        }

        inst.lastSampleIndex = selectedIndex;
        return candidatePool[selectedIndex];
    }

    static float equalPowerWeight(int i, int lastIndex, int nextIndex, float rr)
    {
        float weight = 1.0f;
        if (i == lastIndex)
        {
            float penalty = 0.1f - (rr * 0.08f);
            weight *= std::max(0.01f, penalty);
        }
        else if (i == nextIndex)
        {
            weight *= (1.0f + rr * 1.5f);
        }
        return weight;
    }

    // ---- voice management (port of VoiceManager) ---------------------------
    void startVoice(const DrumSample* sample, float velocity, int offset)
    {
        // Find a free voice, else steal using the native smart-stealing score.
        Voice* target = nullptr;
        for (auto& v : voices) { if (!v.active) { target = &v; break; } }

        if (!target)
        {
            int bestScore = -0x7fffffff;
            for (auto& v : voices)
            {
                int score = v.position;
                if (v.position < 2000) score -= 1000000;
                if (v.active && v.startOffset > 0) score -= 10000000; // pending
                if (score > bestScore) { bestScore = score; target = &v; }
            }
        }
        if (!target) return;

        configureVoice(*target, sample, velocity, offset);
    }

    void configureVoice(Voice& v, const DrumSample* sample, float velocity, int offset)
    {
        v.active = true;
        v.position = 0;
        v.startOffset = offset;
        v.numChans = 0;
        v.voiceLen = 0;

        int numBoth = 0;
        const int n = std::min((int) sample->refs.size(), Voice::kMaxRefs);
        for (int i = 0; i < n; ++i)
        {
            const AudioRef& r = sample->refs[(size_t) i];
            if (r.bufferId < 0 || r.bufferId >= (int) buffers.size()) continue;
            const auto& buf = buffers[(size_t) r.bufferId].data;
            v.chanData[v.numChans]  = buf.data();
            v.chanLen[v.numChans]   = (int) buf.size();
            v.chanRoute[v.numChans] = r.routing;
            if (r.routing == ROUTE_BOTH) ++numBoth;
            if (v.numChans == 0) v.voiceLen = (int) buf.size();
            ++v.numChans;
        }

        float channelGain = 1.0f;
        if (numBoth > 1) channelGain = 1.0f / std::sqrt((float) numBoth);
        v.finalGain = velocity * channelGain;

        if (v.numChans == 0) v.active = false;
    }

    void renderVoice(Voice& v, int numSamples)
    {
        int actualStart = 0;
        int actualNum   = numSamples;
        if (v.startOffset > 0)
        {
            if (v.startOffset >= numSamples) return; // not sounding yet this block
            actualStart += v.startOffset;
            actualNum   -= v.startOffset;
        }

        for (int c = 0; c < v.numChans; ++c)
        {
            const float* src = v.chanData[c];
            if (!src) continue;
            int toRender = std::min(actualNum, v.chanLen[c] - v.position);
            if (toRender <= 0) continue;

            const float* s = src + v.position;
            const float g = v.finalGain;
            const int route = v.chanRoute[c];

            if (route == ROUTE_LEFT)
                for (int i = 0; i < toRender; ++i) outL[actualStart + i] += s[i] * g;
            else if (route == ROUTE_RIGHT)
                for (int i = 0; i < toRender; ++i) outR[actualStart + i] += s[i] * g;
            else
                for (int i = 0; i < toRender; ++i)
                {
                    float val = s[i] * g;
                    outL[actualStart + i] += val;
                    outR[actualStart + i] += val;
                }
        }
    }

    void advanceVoice(Voice& v, int numSamples)
    {
        if (v.startOffset > 0)
        {
            if (v.startOffset >= numSamples) { v.startOffset -= numSamples; return; }
            numSamples -= v.startOffset;
            v.startOffset = 0;
        }
        if (v.voiceLen <= 0) { v.active = false; return; }
        v.position += std::min(numSamples, v.voiceLen - v.position);
        if (v.position >= v.voiceLen) v.active = false;
    }

    // ---- state -------------------------------------------------------------
    float sampleRate = 44100.0f;
    float masterGain = 1.0f;
    float velHumanize = 0.08f;
    float timingHumanize = 5.0f;
    float rrAmount = 0.7f;

    std::vector<SampleBuffer> buffers;
    std::vector<Instrument>   instruments;
    int midiMap[128] = { 0 };
    bool kitReady = false;

    Voice voices[kMaxVoices];
    LockFreeRandom humanizeRng;
    LockFreeRandom rrRng;

    float outL[kMaxBlock] = { 0 };
    float outR[kMaxBlock] = { 0 };

public:
    Engine() { for (int i = 0; i < 128; ++i) midiMap[i] = -1; }
};

// ----------------------------------------------------------------------------
// C ABI exposed to JavaScript
// ----------------------------------------------------------------------------

static Engine g_engine;

extern "C" {

EMSCRIPTEN_KEEPALIVE void dc_init(float sampleRate)          { g_engine.init(sampleRate); }
EMSCRIPTEN_KEEPALIVE void dc_seed(uint32_t s)                { g_engine.seed(s); }
EMSCRIPTEN_KEEPALIVE void dc_clear_kit()                     { g_engine.clearKit(); }
EMSCRIPTEN_KEEPALIVE int  dc_create_buffer(int numFrames)    { return g_engine.createBuffer(numFrames); }
EMSCRIPTEN_KEEPALIVE float* dc_buffer_ptr(int id)            { return g_engine.bufferPtr(id); }
EMSCRIPTEN_KEEPALIVE int  dc_add_instrument()               { return g_engine.addInstrument(); }
EMSCRIPTEN_KEEPALIVE int  dc_add_sample(int inst, float pw)  { return g_engine.addSample(inst, pw); }
EMSCRIPTEN_KEEPALIVE void dc_add_audioref(int inst, int smp, int buf, int route) { g_engine.addAudioRef(inst, smp, buf, route); }
EMSCRIPTEN_KEEPALIVE void dc_set_midi(int note, int inst)    { g_engine.setMidi(note, inst); }
EMSCRIPTEN_KEEPALIVE void dc_finalize_kit()                  { g_engine.finalizeKit(); }
EMSCRIPTEN_KEEPALIVE void dc_set_params(float db, float vel, float tim, float rr) { g_engine.setParams(db, vel, tim, rr); }
EMSCRIPTEN_KEEPALIVE void dc_note_on(int note, float vel)    { g_engine.noteOn(note, vel); }
EMSCRIPTEN_KEEPALIVE void dc_render(int numFrames)           { g_engine.render(numFrames); }
EMSCRIPTEN_KEEPALIVE float* dc_out_l()                       { return g_engine.outLPtr(); }
EMSCRIPTEN_KEEPALIVE float* dc_out_r()                       { return g_engine.outRPtr(); }
EMSCRIPTEN_KEEPALIVE int  dc_active_voices()                 { return g_engine.activeVoices(); }

} // extern "C"
