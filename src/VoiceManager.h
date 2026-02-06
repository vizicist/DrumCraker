#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SampleEngine.h"
#include <vector>

// Forward declaration
namespace juce { class AudioProcessor; }

class Voice
{
public:
    Voice() = default;
    
    void start(const DrumSample* sample, float velocity, SampleEngine* engine, 
               int offset = 0, double sampleRate = 44100.0, const juce::String& instrumentName = "", 
               int busIdx = 0);
    void stop();
    bool isActive() const { return active; }
    
    // Ultra-optimized rendering with pre-cached routing
    void renderToBuffer(juce::AudioBuffer<float>& outputBuffer,
                       int startSample, int numSamples);
    
    // Advance playback position (call once per block)
    void advancePosition(int numSamples);
    
    int getStartOffset() const { return startOffset; }
    int getBusIndex() const { return busIndex; }
    const DrumSample* getCurrentSample() const { return currentSample; }
    juce::String getInstrumentName() const { return instrumentName; }
    
    // Helper for stealing logic
    int getPlaybackPosition() const { return currentPosition; }
    bool isPending() const { return active && startOffset > 0; }

private:
    bool active = false;
    const DrumSample* currentSample = nullptr;
    SampleEngine* sampleEngine = nullptr;
    int currentPosition = 0;
    int startOffset = 0;
    int busIndex = 0;
    float currentVelocity = 1.0f;
    float finalGain = 1.0f;
    double playbackSampleRate = 44100.0;
    juce::String instrumentName;
    
    // Pre-cached channel routing (0=left, 1=right, 2=both)
    std::vector<int> cachedChannelRouting;
    
    // OPTIMIZED: Direct pointers to buffers (no lookups in render loop)
    std::vector<const juce::AudioBuffer<float>*> cachedAudioBuffers;
};

class VoiceManager
{
public:
    VoiceManager();
    
    void prepare(double sampleRate, int samplesPerBlock);
    void reset();
    void noteOn(int midiNote, float velocity, SampleEngine* engine, 
                int sampleOffset = 0, float roundRobinAmount = 0.5f);
    void noteOff(int midiNote);
    
    double getSampleRate() const { return sampleRate; }
    
    // Set instrument to bus mapping (called once when kit loads)
    void setInstrumentToBusMap(const std::map<juce::String, int>& map) { instrumentToBusMap = map; }
    
    // Render all voices to buffer (complete mix)
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                        int startSample, int numSamples);
    
    // OPTIMIZED: Render all voices once, routing to appropriate buses
    void renderNextBlockMultiBus(juce::AudioBuffer<float>& mainBuffer,
                                 int startSample, int numSamples,
                                 float gainLinear,
                                 juce::AudioProcessor* processor);
    
    // Advance all voice positions (call after rendering)
    void advanceAllVoices(int numSamples);

private:
    static constexpr int maxVoices = 128; // Increased for high BPM/Fast rolls
    std::vector<Voice> voices;
    double sampleRate = 44100.0;
    std::map<juce::String, int> instrumentToBusMap;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceManager)
};
