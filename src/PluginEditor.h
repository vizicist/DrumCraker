#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class DrumSamplerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DrumSamplerEditor(DrumSamplerProcessor&);
    ~DrumSamplerEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    void loadKitButtonClicked();
    void loadMidiMapButtonClicked();
    void updateUIFromProcessor();
    
    DrumSamplerProcessor& processor;
    
    // Background image
    juce::Image backgroundImage;
    
    juce::TextButton loadKitButton;
    juce::TextButton loadMidiMapButton;
    juce::Label kitNameLabel;
    juce::Label midiMapLabel;
    juce::Label statusLabel;
    juce::Label versionLabel;
    
    // Master Volume
    juce::Slider masterVolumeSlider;
    juce::Label masterVolumeLabel;
    
    // Humanization controls
    juce::Slider velocityRandomnessSlider;
    juce::Label velocityRandomnessLabel;
    juce::Slider timingRandomnessSlider;
    juce::Label timingRandomnessLabel;
    juce::Slider roundRobinSlider;
    juce::Label roundRobinLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSamplerEditor)
};
