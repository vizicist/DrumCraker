#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "DrumKitLoader.h"
#include "SampleEngine.h"
#include "VoiceManager.h"

class DrumSamplerProcessor : public juce::AudioProcessor
{
public:
    DrumSamplerProcessor();
    ~DrumSamplerProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DrumCraker"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool loadDrumKit(const juce::File& kitFile, bool async = true);
    bool loadMidiMap(const juce::File& midiMapFile);
    juce::String getCurrentKitName() const { return currentKitName; }
    juce::String getCurrentMidiMapName() const { return currentMidiMapName; }
    int getNumChannels() const { return numOutputChannels; }
    bool isKitFullyLoaded() const { return sampleEngine->isLoaded(); }
    bool getIsLoadingKit() const { return isLoadingKit.load(); }
    bool getIsLoadingMidiMap() const { return isLoadingMidiMap.load(); }

    // Parámetros
    juce::AudioParameterFloat* masterVolume;
    juce::AudioParameterFloat* velocityRandomness;
    juce::AudioParameterFloat* timingRandomness;
    juce::AudioParameterFloat* roundRobinVariation;

private:
    std::unique_ptr<DrumKitLoader> drumKitLoader;
    std::unique_ptr<SampleEngine> sampleEngine;
    std::unique_ptr<VoiceManager> voiceManager;
    
    juce::String currentKitName;
    juce::String currentMidiMapName;
    juce::String currentKitPath;      // Ruta completa del kit XML
    juce::String currentMidiMapPath;  // Ruta completa del midimap XML
    std::atomic<bool> isLoadingKit{false};
    std::atomic<bool> isLoadingMidiMap{false};
    int numOutputChannels = 2;
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSamplerProcessor)
};
