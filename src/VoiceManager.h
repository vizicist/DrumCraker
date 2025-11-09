#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SampleEngine.h"
#include <vector>

class Voice
{
public:
    Voice() = default;
    
    void start(const DrumSample* sample, float velocity, SampleEngine* engine, 
               int offset = 0, double sampleRate = 44100.0);
    void stop();
    bool isActive() const { return active; }
    
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer, 
                        int startSample, int numSamples);
    
    int getStartOffset() const { return startOffset; }

private:
    bool active = false;
    const DrumSample* currentSample = nullptr;
    SampleEngine* sampleEngine = nullptr;
    int currentPosition = 0;
    int startOffset = 0;
    float currentVelocity = 1.0f;
    double playbackSampleRate = 44100.0;
};

class VoiceManager
{
public:
    VoiceManager();
    
    void prepare(double sampleRate, int samplesPerBlock);
    void reset(); // Detener todas las voces activas
    void noteOn(int midiNote, float velocity, SampleEngine* engine, 
                int sampleOffset = 0, float roundRobinAmount = 0.5f);
    void noteOff(int midiNote);
    
    double getSampleRate() const { return sampleRate; }
    
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                        int startSample, int numSamples);

private:
    static constexpr int maxVoices = 64;
    std::vector<Voice> voices;
    double sampleRate = 44100.0;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceManager)
};
