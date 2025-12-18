#include "VoiceManager.h"
#include <juce_audio_processors/juce_audio_processors.h>

void Voice::start(const DrumSample* sample, float velocity, SampleEngine* engine, 
                 int offset, double sampleRate, const juce::String& instrName, int busIdx)
{
    currentSample = sample;
    currentVelocity = velocity;
    sampleEngine = engine;
    currentPosition = 0;
    startOffset = offset;
    playbackSampleRate = sampleRate;
    instrumentName = instrName;
    busIndex = busIdx;
    active = true;
    
    // Pre-cache channel routing and gain for ultra-fast rendering
    if (sample && !sample->audioFiles.empty())
    {
        const int numChannels = static_cast<int>(sample->audioFiles.size());
        
        // Pre-determine channel routing (avoid string operations in audio thread)
        cachedChannelRouting.resize(numChannels);
        cachedAudioBuffers.resize(numChannels); // Resize buffer cache
        
        int numLeftChannels = 0;
        int numRightChannels = 0;
        int numBothChannels = 0;
        
        for (int i = 0; i < numChannels; ++i)
        {
            const juce::String& channelName = sample->audioFiles[i].channelName;
            const juce::juce_wchar lastChar = channelName.getLastCharacter();
            
            // Check last character first (fastest), then full string if needed
            if (lastChar == 'L' || channelName.containsIgnoreCase("Left"))
            {
                cachedChannelRouting[i] = 0; // Left
                numLeftChannels++;
            }
            else if (lastChar == 'R' || channelName.containsIgnoreCase("Right"))
            {
                cachedChannelRouting[i] = 1; // Right
                numRightChannels++;
            }
            else
            {
                cachedChannelRouting[i] = 2; // Both/Center (mono, ambience, etc.)
                numBothChannels++;
            }
            
            // OPTIMIZATION: Cache buffer pointer NOW (one lock per note start)
            // instead of per block (many locks per second)
            cachedAudioBuffers[i] = sampleEngine->getAudioBuffer(sample, channelName);
        }
        
        // Simple and universal gain compensation
        // Rule: Only "Both" channels sum together (to both L+R outputs)
        // L/R channels go to separate outputs and don't sum with each other
        
        float channelGain = 1.0f;
        
        // Only compensate for "Both" channels that actually sum
        if (numBothChannels > 1)
        {
            // Use sqrt for proper RMS compensation
            channelGain = 1.0f / std::sqrt(static_cast<float>(numBothChannels));
        }
        
        // No additional compensation needed - L/R channels don't interfere
        // This works universally for all drumkits
        
        finalGain = velocity * channelGain;
    }
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
            return;
        
        actualStartSample += startOffset;
        actualNumSamples -= startOffset;
    }

    const bool isStereoOutput = outputBuffer.getNumChannels() >= 2;
    const int numChannels = static_cast<int>(currentSample->audioFiles.size());
    
    // Get output pointers once (avoid repeated calls)
    float* outL = outputBuffer.getWritePointer(0);
    float* outR = isStereoOutput ? outputBuffer.getWritePointer(1) : nullptr;
    
    // Ultra-optimized rendering loop with pre-cached routing and SIMD operations
    for (int i = 0; i < numChannels; ++i)
    {
        // USE CACHED POINTER (Lock-Free!)
        const auto* buffer = cachedAudioBuffers[i];
        
        if (!buffer) [[unlikely]]
            continue;

        const int samplesToRender = juce::jmin(actualNumSamples, 
                                              buffer->getNumSamples() - currentPosition,
                                              outputBuffer.getNumSamples() - actualStartSample);

        if (samplesToRender <= 0) [[unlikely]]
            continue;

        const int routing = cachedChannelRouting[i];
        const float* sourceData = buffer->getReadPointer(0) + currentPosition;
        
        if (isStereoOutput) [[likely]]
        {
            if (routing == 0) [[unlikely]] // Left only
            {
                juce::FloatVectorOperations::addWithMultiply(outL + actualStartSample, sourceData, finalGain, samplesToRender);
            }
            else if (routing == 1) [[unlikely]] // Right only
            {
                juce::FloatVectorOperations::addWithMultiply(outR + actualStartSample, sourceData, finalGain, samplesToRender);
            }
            else [[likely]] // Both channels (most common case)
            {
                juce::FloatVectorOperations::addWithMultiply(outL + actualStartSample, sourceData, finalGain, samplesToRender);
                juce::FloatVectorOperations::addWithMultiply(outR + actualStartSample, sourceData, finalGain, samplesToRender);
            }
        }
        else
        {
            juce::FloatVectorOperations::addWithMultiply(outL + actualStartSample, sourceData, finalGain, samplesToRender);
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



    // USE CACHED POINTER
    const auto* buffer = cachedAudioBuffers.empty() ? nullptr : cachedAudioBuffers[0];
                                               
    if (!buffer)
    {
        stop();
        return;
    }

    currentPosition += juce::jmin(numSamples, buffer->getNumSamples() - currentPosition);

    if (currentPosition >= buffer->getNumSamples())
        stop();
}

VoiceManager::VoiceManager()
{
    voices.resize(maxVoices);
}

void VoiceManager::prepare(double sr, int)
{
    sampleRate = sr;
}

void VoiceManager::reset()
{
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

    // Get bus index for this instrument
    int busIdx = 0;
    auto it = instrumentToBusMap.find(instrumentName);
    if (it != instrumentToBusMap.end())
        busIdx = it->second;

    // Find free voice
    for (auto& voice : voices)
    {
        if (!voice.isActive())
        {
            voice.start(sample, velocity, engine, sampleOffset, sampleRate, instrumentName, busIdx);
            return;
        }
    }

    // If no free voices, use SMART STEALING logic
    // We want to steal the "most finished" voice and Protect "fresh" voices
    
    int bestVoiceToSteal = -1;
    int highestScore = -1;
    
    for (int i = 0; i < voices.size(); ++i)
    {
        const auto& voice = voices[i];
        if (!voice.isActive()) 
            continue; // Should have been caught above, but safety first
            
        int score = 0;
        
        // BASE SCORE: How much has it played?
        // Old voices >>> Fresh voices
        score += voice.getPlaybackPosition();
        
        // PENALTY: Fresh voices (< 2000 samples ~ 45ms) should be protected
        if (voice.getPlaybackPosition() < 2000)
            score -= 1000000; // Huge penalty
            
        // CRITICAL PENALTY: Pending voices (haven't even started!)
        // ABSOLUTELY DO NOT STEAL unless catastrophic
        if (voice.isPending())
            score -= 10000000;
            
        if (score > highestScore)
        {
            highestScore = score;
            bestVoiceToSteal = i;
        }
    }
    
    // Steal the best candidate
    if (bestVoiceToSteal >= 0)
    {
        voices[bestVoiceToSteal].start(sample, velocity, engine, sampleOffset, sampleRate, instrumentName, busIdx);
    }
}

void VoiceManager::noteOff(int)
{
    // Drum samples don't respond to note off
}

void VoiceManager::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                  int startSample, int numSamples)
{
    for (auto& voice : voices)
    {
        if (voice.isActive())
            voice.renderToBuffer(outputBuffer, startSample, numSamples);
    }
    
    advanceAllVoices(numSamples);
}

void VoiceManager::renderNextBlockMultiBus(juce::AudioBuffer<float>& mainBuffer,
                                           int startSample, int numSamples,
                                           float gainLinear,
                                           juce::AudioProcessor* processor)
{
    // Pre-cache enabled buses to avoid repeated virtual calls
    bool busEnabled[16] = {false};
    for (int i = 0; i < 16; ++i)
    {
        auto* bus = processor->getBus(false, i);
        busEnabled[i] = (bus && bus->isEnabled());
    }
    
    // Render each voice ONCE to its pre-assigned bus (ultra-fast)
    for (auto& voice : voices)
    {
        if (!voice.isActive())
            continue;
        
        const int busIndex = voice.getBusIndex();
        
        // Check if bus is enabled (cached, no virtual call)
        if (!busEnabled[busIndex])
            continue;
        
        // Get buffer for this bus and render
        auto busBuffer = processor->getBusBuffer(mainBuffer, false, busIndex);
        voice.renderToBuffer(busBuffer, startSample, numSamples);
    }
    
    // Apply gain to all enabled buses (using cached info)
    for (int busIndex = 0; busIndex < 16; ++busIndex)
    {
        if (busEnabled[busIndex])
        {
            auto busBuffer = processor->getBusBuffer(mainBuffer, false, busIndex);
            busBuffer.applyGain(gainLinear);
        }
    }
    
    advanceAllVoices(numSamples);
}

void VoiceManager::advanceAllVoices(int numSamples)
{
    // Optimized: process only active voices
    for (auto& voice : voices)
    {
        if (voice.isActive()) [[likely]]
            voice.advancePosition(numSamples);
    }
}
