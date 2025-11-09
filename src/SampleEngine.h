#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "DrumKitLoader.h"
#include <atomic>

class SampleEngine
{
public:
    SampleEngine();
    ~SampleEngine();

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();
    
    void loadKit(std::unique_ptr<DrumKit> kit, bool async = true);
    bool loadMidiMap(const juce::File& midiMapFile);
    bool isLoaded() const { return kitLoaded.load(); }
    
    const DrumSample* getSampleForNote(int midiNote, float velocity, float roundRobinAmount = 0.5f);
    juce::AudioBuffer<float>* getAudioBuffer(const DrumSample* sample, 
                                             const juce::String& channelName);
    
    double getOriginalSampleRate(const DrumSample* sample, const juce::String& channelName);
    
    // Callback para notificar cuando termina la carga
    std::function<void(bool)> loadingCallback;

private:
    void loadSamplesAsync();
    void loadSamplesSync();
    bool loadSampleFileOnce(const juce::String& filePath, const std::vector<AudioSample>& channels);
    void resampleBuffer(juce::AudioBuffer<float>& buffer, double sourceSampleRate, double targetSampleRate);
    
    std::unique_ptr<DrumKit> currentKit;
    std::atomic<bool> kitLoaded{false};
    
    // Cache de buffers de audio cargados
    std::map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>> audioBufferCache;
    std::map<juce::String, double> originalSampleRates; // Guardar sample rate original
    juce::CriticalSection cacheLock;
    
    // Round robin tracking
    std::map<int, int> lastSampleIndex; // midiNote -> último índice usado
    
    double sampleRate = 44100.0;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleEngine)
};
