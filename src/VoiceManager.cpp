#include "VoiceManager.h"
#include <set>

void Voice::start(const DrumSample* sample, float velocity, SampleEngine* engine, 
                 int offset, double sampleRate)
{
    currentSample = sample;
    currentVelocity = velocity;
    sampleEngine = engine;
    currentPosition = 0;
    startOffset = offset;
    playbackSampleRate = sampleRate;
    active = true;
}

void Voice::stop()
{
    active = false;
    currentSample = nullptr;
}

void Voice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer, 
                           int startSample, int numSamples)
{
    if (!active || !currentSample || !sampleEngine)
        return;

    // Aplicar offset de timing si es necesario
    if (startOffset > 0)
    {
        if (startOffset >= numSamples)
        {
            startOffset -= numSamples;
            return;
        }
        startSample += startOffset;
        numSamples -= startOffset;
        startOffset = 0;
    }

    // Por simplicidad, mezclamos solo el primer canal disponible
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

    int samplesToRender = juce::jmin(numSamples, 
                                     buffer->getNumSamples() - currentPosition);

    if (samplesToRender <= 0)
    {
        stop();
        return;
    }
    
    // Verificar buffer overrun
    if (startSample + samplesToRender > outputBuffer.getNumSamples())
        samplesToRender = outputBuffer.getNumSamples() - startSample;

    // Mezclar el sample en el buffer de salida
    for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
    {
        outputBuffer.addFrom(channel, startSample, 
                           *buffer, 0, currentPosition, 
                           samplesToRender, currentVelocity);
    }

    currentPosition += samplesToRender;

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
    // Detener todas las voces activas
    for (auto& voice : voices)
    {
        if (voice.isActive())
            voice.stop();
    }
}

void VoiceManager::noteOn(int midiNote, float velocity, SampleEngine* engine,
                         int sampleOffset, float roundRobinAmount)
{
    auto* sample = engine->getSampleForNote(midiNote, velocity, roundRobinAmount);
    if (!sample)
        return;

    // Buscar una voz libre
    for (auto& voice : voices)
    {
        if (!voice.isActive())
        {
            voice.start(sample, velocity, engine, sampleOffset, sampleRate);
            return;
        }
    }

    // Si no hay voces libres, robar la más antigua (primera activa)
    for (auto& voice : voices)
    {
        if (voice.isActive())
        {
            voice.start(sample, velocity, engine, sampleOffset, sampleRate);
            return;
        }
    }
}

void VoiceManager::noteOff(int midiNote)
{
    // Los samples de batería normalmente no responden a note off
    // pero lo dejamos por si acaso
}

void VoiceManager::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                  int startSample, int numSamples)
{
    for (auto& voice : voices)
    {
        if (voice.isActive())
            voice.renderNextBlock(outputBuffer, startSample, numSamples);
    }
}
