#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>
#include <map>

struct AudioSample
{
    juce::String channelName;
    juce::File audioFile;
    int fileChannel;
};

struct DrumSample
{
    juce::String name;
    float power;
    std::vector<AudioSample> audioFiles;
};

struct Instrument
{
    juce::String name;
    juce::String group;
    std::vector<DrumSample> samples;
    std::map<juce::String, juce::String> channelMap;
};

struct DrumKit
{
    juce::String name;
    juce::String description;
    std::vector<juce::String> channels;
    std::vector<std::unique_ptr<Instrument>> instruments;
    std::map<int, juce::String> midiMap; // MIDI note -> instrument name
    juce::File basePath;
};

class DrumKitLoader
{
public:
    DrumKitLoader() = default;
    
    std::unique_ptr<DrumKit> loadKit(const juce::File& kitFile);

private:
    bool parseKitXML(const juce::File& kitFile, DrumKit& kit);
    bool parseInstrumentXML(const juce::File& instrumentFile, Instrument& instrument);
    bool parseMidiMapXML(const juce::File& midiMapFile, DrumKit& kit);
    
    juce::File findMidiMapFile(const juce::File& kitFile);
};
