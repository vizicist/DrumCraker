#include "VoiceManager.h"
#include <set>

void Voice::start(const DrumSample* sample, float velocity, SampleEngine* engine, 
                 int offset, double sampleRate, const juce::String& instrName)
{
    currentSample = sample;
    currentVelocity = velocity;
    sampleEngine = engine;
    currentPosition = 0;
    startOffset = offset;
    playbackSampleRate = sampleRate;
    instrumentName = instrName;
    active = true;
}

void Voice::stop()
{
    active = false;
    currentSample = nullptr;
}

void Voice::renderToBuffer(juce::AudioBuffer<float>& outputBuffer,
                          int startSample, int numSamples)
{
    if (!active || !currentSample || !sampleEngine)
        return;

    int actualStartSample = startSample;
    int actualNumSamples = numSamples;

    // Apply timing offset if necessary
    if (startOffset > 0)
    {
        if (startOffset >= numSamples)
            return; // Not time to sound yet
        
        actualStartSample += startOffset;
        actualNumSamples -= startOffset;
    }

    // Pre-calculate channel count and gain
    int numChannels = 0;
    for (const auto& audioFile : currentSample->audioFiles)
    {
        if (sampleEngine->getAudioBuffer(currentSample, audioFile.channelName))
            numChannels++;
    }
    
    if (numChannels == 0)
        return;
    
    // Normalize gain by number of channels (cached calculation)
    float channelGain = 2.0f / static_cast<float>(numChannels);
    channelGain = juce::jlimit(0.1f, 1.0f, channelGain);
    const float finalGain = currentVelocity * channelGain;
    const bool isStereoOutput = outputBuffer.getNumChannels() >= 2;
    
    // Render channels with proper stereo routing (optimized)
    for (const auto& audioFile : currentSample->audioFiles)
    {
        auto* buffer = sampleEngine->getAudioBuffer(currentSample, audioFile.channelName);
        if (!buffer)
            continue;

        const int samplesToRender = juce::jmin(actualNumSamples, 
                                              buffer->getNumSamples() - currentPosition,
                                              outputBuffer.getNumSamples() - actualStartSample);

        if (samplesToRender <= 0)
            continue;

        // Determine channel routing (cached string operations)
        const juce::String& channelName = audioFile.channelName;
        const bool isLeft = channelName.containsIgnoreCase("Left") || channelName.endsWithIgnoreCase("L");
        const bool isRight = channelName.containsIgnoreCase("Right") || channelName.endsWithIgnoreCase("R");
        const bool isCenter = channelName.containsIgnoreCase("Center") || channelName.containsIgnoreCase("Centre");

        if (isStereoOutput)
        {
            // STEREO OUTPUT - optimized routing
            if (isLeft)
            {
                outputBuffer.addFrom(0, actualStartSample, *buffer, 0, currentPosition, samplesToRender, finalGain);
            }
            else if (isRight)
            {
                outputBuffer.addFrom(1, actualStartSample, *buffer, 0, currentPosition, samplesToRender, finalGain);
            }
            else
            {
                // Center or mono channels → both L/R
                outputBuffer.addFrom(0, actualStartSample, *buffer, 0, currentPosition, samplesToRender, finalGain);
                outputBuffer.addFrom(1, actualStartSample, *buffer, 0, currentPosition, samplesToRender, finalGain);
            }
        }
        else
        {
            // MONO OUTPUT
            outputBuffer.addFrom(0, actualStartSample, *buffer, 0, currentPosition, samplesToRender, finalGain);
        }
    }
}



void Voice::advancePosition(int numSamples)
{
    if (!active || !currentSample || !sampleEngine)
        return;

    // Handle timing offset
    if (startOffset > 0)
    {
        if (startOffset >= numSamples)
        {
            startOffset -= numSamples;
            return;
        }
        numSamples -= startOffset;
        startOffset = 0;
    }

    // Get first buffer as reference for length
    if (currentSample->audioFiles.empty())
    {
        stop();
        return;
    }

    auto* buffer = sampleEngine->getAudioBuffer(currentSample, 
                                               currentSample->audioFiles[0].channelName);
    if (!buffer)
    {
        stop();
        return;
    }

    int samplesToAdvance = juce::jmin(numSamples, 
                                      buffer->getNumSamples() - currentPosition);

    currentPosition += samplesToAdvance;

    if (currentPosition >= buffer->getNumSamples())
        stop();
}

VoiceManager::VoiceManager()
{
    voices.resize(maxVoices);
}

void VoiceManager::prepare(double sr, int samplesPerBlock)
{
    sampleRate = sr;
}

void VoiceManager::reset()
{
    // Stop all active voices
    for (auto& voice : voices)
    {
        if (voice.isActive())
            voice.stop();
    }
}

void VoiceManager::noteOn(int midiNote, float velocity, SampleEngine* engine,
                         int sampleOffset, float roundRobinAmount)
{
    juce::String instrumentName;
    auto* sample = engine->getSampleForNote(midiNote, velocity, roundRobinAmount, &instrumentName);
    if (!sample)
        return;

    // Find free voice
    for (auto& voice : voices)
    {
        if (!voice.isActive())
        {
            voice.start(sample, velocity, engine, sampleOffset, sampleRate, instrumentName);
            return;
        }
    }

    // If no free voices, steal oldest (first active)
    for (auto& voice : voices)
    {
        if (voice.isActive())
        {
            voice.start(sample, velocity, engine, sampleOffset, sampleRate, instrumentName);
            return;
        }
    }
}

void VoiceManager::noteOff(int midiNote)
{
    // Drum samples normally don't respond to note off
    // but we leave it just in case
}

void VoiceManager::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                  int startSample, int numSamples)
{
    for (auto& voice : voices)
    {
        if (voice.isActive())
        {
            voice.renderToBuffer(outputBuffer, startSample, numSamples);
        }
    }
    
    // Advance all voices after rendering
    advanceAllVoices(numSamples);
}

void VoiceManager::renderNextBlockForBus(juce::AudioBuffer<float>& busBuffer,
                                        int startSample, int numSamples,
                                        int targetBusIndex,
                                        const std::map<juce::String, int>& instrumentToBusMap)
{
    // Render only voices assigned to this specific bus
    for (auto& voice : voices)
    {
        if (!voice.isActive())
            continue;
        
        // Get instrument name for this voice
        juce::String instrumentName = voice.getInstrumentName();
        
        // Find bus assignment (default to bus 0 if not found)
        int busIndex = 0;
        auto it = instrumentToBusMap.find(instrumentName);
        if (it != instrumentToBusMap.end())
            busIndex = it->second;
        
        // Only render if this voice belongs to the target bus
        if (busIndex == targetBusIndex)
        {
            voice.renderToBuffer(busBuffer, startSample, numSamples);
        }
    }
}

void VoiceManager::advanceAllVoices(int numSamples)
{
    for (auto& voice : voices)
    {
        if (voice.isActive())
        {
            voice.advancePosition(numSamples);
        }
    }
}




