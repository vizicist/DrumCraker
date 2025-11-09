#include "PluginProcessor.h"
#include "PluginEditor.h"

DrumSamplerProcessor::DrumSamplerProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    drumKitLoader = std::make_unique<DrumKitLoader>();
    sampleEngine = std::make_unique<SampleEngine>();
    voiceManager = std::make_unique<VoiceManager>();

    // Parámetros
    addParameter(masterVolume = new juce::AudioParameterFloat(
        "masterVol", "Master Volume", -60.0f, 12.0f, 0.0f));
    
    // Velocity: 0-100% de variación (default 8% para humanización sutil)
    addParameter(velocityRandomness = new juce::AudioParameterFloat(
        "velocityRnd", "Velocity Humanization", 0.0f, 1.0f, 0.08f));
    
    // Timing: 0-20ms de variación (default 5ms para groove natural)
    addParameter(timingRandomness = new juce::AudioParameterFloat(
        "timingRnd", "Timing Humanization (ms)", 0.0f, 20.0f, 5.0f));
    
    // Round Robin: 0=velocity puro, 1=rotación pura (default 0.7 para mezcla natural)
    addParameter(roundRobinVariation = new juce::AudioParameterFloat(
        "roundRobin", "Round Robin Mix", 0.0f, 1.0f, 0.7f));
}

DrumSamplerProcessor::~DrumSamplerProcessor() {}

void DrumSamplerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    sampleEngine->prepare(sampleRate, samplesPerBlock);
    voiceManager->prepare(sampleRate, samplesPerBlock);
}

void DrumSamplerProcessor::releaseResources()
{
    // NO resetear el sampleEngine aquí
    // Mantener los samples en memoria para exportaciones rápidas
    // Solo detener las voces activas
    voiceManager->reset();
}

void DrumSamplerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Si el kit no está cargado, no procesar
    if (!sampleEngine->isLoaded())
        return;

    // Verificar buffer size válido
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return;

    // Procesar eventos MIDI con humanización natural
    for (const auto metadata : midiMessages)
    {
        auto message = metadata.getMessage();
        int sampleOffset = metadata.samplePosition;
        
        if (message.isNoteOn())
        {
            float velocity = message.getVelocity() / 127.0f;
            int midiNote = message.getNoteNumber();
            
            // HUMANIZACIÓN DE VELOCITY - Añade variación natural
            // Funciona incluso si todas las notas MIDI tienen la misma velocity
            float velocityHumanization = velocityRandomness->get();
            if (velocityHumanization > 0.001f)
            {
                // Box-Muller para distribución gaussiana perfecta
                float u1 = juce::Random::getSystemRandom().nextFloat();
                float u2 = juce::Random::getSystemRandom().nextFloat();
                
                // Evitar log(0)
                u1 = juce::jmax(0.0001f, u1);
                
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * juce::MathConstants<float>::pi * u2);
                
                // Variación proporcional al parámetro (0-100%)
                // Con 8% default: ±8% de variación en velocity
                float randomAmount = gaussian * velocityHumanization * 0.33f; // 0.33 = 1 sigma cubre ~68%
                
                // Aplicar variación manteniendo rango válido
                velocity = juce::jlimit(0.05f, 1.0f, velocity + randomAmount);
            }
            
            // HUMANIZACIÓN DE TIMING - Groove natural
            float timingHumanization = timingRandomness->get();
            if (timingHumanization > 0.001f)
            {
                // Box-Muller para timing gaussiano
                float u1 = juce::Random::getSystemRandom().nextFloat();
                float u2 = juce::Random::getSystemRandom().nextFloat();
                
                // Evitar log(0)
                u1 = juce::jmax(0.0001f, u1);
                
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::sin(2.0f * juce::MathConstants<float>::pi * u2);
                
                // Bias según velocity: notas fuertes se adelantan, suaves se retrasan
                float velocityBias = (velocity - 0.5f) * 0.4f; // ±20% bias
                
                // Aplicar timing con bias (gaussian * 0.5 para mantener en rango ±1.5 sigma)
                float randomMs = (gaussian * 0.5f + velocityBias) * timingHumanization;
                int sampleDelay = static_cast<int>((randomMs / 1000.0f) * currentSampleRate);
                
                // Limitar para no salir del buffer (permitir adelantos y retrasos)
                int newOffset = sampleOffset + sampleDelay;
                sampleOffset = juce::jlimit(0, buffer.getNumSamples() - 1, newOffset);
            }
            
            // Round robin con variación inteligente para evitar repeticiones mecánicas
            float rrVariation = roundRobinVariation->get();
            
            voiceManager->noteOn(midiNote, velocity, 
                               sampleEngine.get(), sampleOffset, rrVariation);
        }
        else if (message.isNoteOff())
        {
            voiceManager->noteOff(message.getNoteNumber());
        }
    }

    // Renderizar voces activas
    voiceManager->renderNextBlock(buffer, 0, buffer.getNumSamples());
    
    // Aplicar master volume
    float gainLinear = juce::Decibels::decibelsToGain(masterVolume->get());
    buffer.applyGain(gainLinear);
}

bool DrumSamplerProcessor::loadDrumKit(const juce::File& kitFile, bool async)
{
    isLoadingKit = true;
    
    // Detener todas las voces activas antes de descargar el kit anterior
    voiceManager->reset();
    
    auto kit = drumKitLoader->loadKit(kitFile);
    if (kit == nullptr)
    {
        isLoadingKit = false;
        return false;
    }

    currentKitName = kit->name;
    currentKitPath = kitFile.getFullPathName();  // Guardar ruta completa
    numOutputChannels = static_cast<int>(kit->channels.size());
    
    // Si no hay callback configurado, crear uno por defecto
    if (!sampleEngine->loadingCallback)
    {
        sampleEngine->loadingCallback = [this](bool success)
        {
            isLoadingKit = false;
        };
    }
    
    // Cargar el nuevo kit (esto descargará el anterior automáticamente)
    sampleEngine->loadKit(std::move(kit), async);
    
    // Si es síncrono, actualizar estado inmediatamente
    if (!async)
        isLoadingKit = false;
    
    return true;
}

bool DrumSamplerProcessor::loadMidiMap(const juce::File& midiMapFile)
{
    isLoadingMidiMap = true;
    
    bool success = sampleEngine->loadMidiMap(midiMapFile);
    if (success)
    {
        currentMidiMapName = midiMapFile.getFileNameWithoutExtension();
        currentMidiMapPath = midiMapFile.getFullPathName();  // Guardar ruta completa
    }
    
    isLoadingMidiMap = false;
    return success;
}

juce::AudioProcessorEditor* DrumSamplerProcessor::createEditor()
{
    return new DrumSamplerEditor(*this);
}

void DrumSamplerProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Crear XML para guardar el estado
    auto xml = std::make_unique<juce::XmlElement>("DrumCrakerState");
    
    // Guardar parámetros
    xml->setAttribute("masterVolume", static_cast<double>(masterVolume->get()));
    xml->setAttribute("velocityRandomness", static_cast<double>(velocityRandomness->get()));
    xml->setAttribute("timingRandomness", static_cast<double>(timingRandomness->get()));
    xml->setAttribute("roundRobinVariation", static_cast<double>(roundRobinVariation->get()));
    
    // Guardar rutas completas de los archivos XML
    xml->setAttribute("kitPath", currentKitPath);
    xml->setAttribute("midiMapPath", currentMidiMapPath);
    
    // Guardar nombres (para referencia en UI)
    xml->setAttribute("kitName", currentKitName);
    xml->setAttribute("midiMapName", currentMidiMapName);
    
    // Convertir XML a binary
    copyXmlToBinary(*xml, destData);
}

void DrumSamplerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    
    if (xml != nullptr && xml->hasTagName("DrumCrakerState"))
    {
        // Restaurar parámetros inmediatamente
        *masterVolume = static_cast<float>(xml->getDoubleAttribute("masterVolume", 0.0));
        *velocityRandomness = static_cast<float>(xml->getDoubleAttribute("velocityRandomness", 0.0));
        *timingRandomness = static_cast<float>(xml->getDoubleAttribute("timingRandomness", 0.0));
        *roundRobinVariation = static_cast<float>(xml->getDoubleAttribute("roundRobinVariation", 0.5));
        
        // Restaurar rutas de archivos
        juce::String savedKitPath = xml->getStringAttribute("kitPath");
        juce::String savedMidiMapPath = xml->getStringAttribute("midiMapPath");
        juce::String savedKitName = xml->getStringAttribute("kitName");
        juce::String savedMidiMapName = xml->getStringAttribute("midiMapName");
        
        // Verificar si necesitamos recargar
        bool kitLoaded = sampleEngine->isLoaded();
        bool isSameKit = (savedKitPath == currentKitPath && !currentKitPath.isEmpty());
        
        // Solo recargar si es diferente o no está cargado
        if ((!isSameKit || !kitLoaded) && savedKitPath.isNotEmpty())
        {
            juce::File kitFile(savedKitPath);
            if (kitFile.existsAsFile())
            {
                // Actualizar variables
                currentKitPath = savedKitPath;
                currentKitName = savedKitName;
                currentMidiMapPath = savedMidiMapPath;
                currentMidiMapName = savedMidiMapName;
                
                // Cargar ASÍNCRONAMENTE para no bloquear Reaper
                isLoadingKit = true;
                
                // Capturar variables para el lambda
                juce::String midiMapPath = currentMidiMapPath;
                
                // Configurar callback antes de cargar
                sampleEngine->loadingCallback = [this, midiMapPath](bool success)
                {
                    isLoadingKit = false;
                    
                    if (success && midiMapPath.isNotEmpty())
                    {
                        juce::File midiMapFile(midiMapPath);
                        if (midiMapFile.existsAsFile())
                        {
                            isLoadingMidiMap = true;
                            sampleEngine->loadMidiMap(midiMapFile);
                            isLoadingMidiMap = false;
                        }
                    }
                };
                
                voiceManager->reset();
                auto kit = drumKitLoader->loadKit(kitFile);
                if (kit != nullptr)
                {
                    numOutputChannels = static_cast<int>(kit->channels.size());
                    sampleEngine->loadKit(std::move(kit), true); // ASYNC
                }
                else
                {
                    isLoadingKit = false;
                }
            }
        }
        else if (isSameKit && kitLoaded)
        {
            // Kit ya cargado, solo actualizar MIDI map si cambió
            if (savedMidiMapPath != currentMidiMapPath && savedMidiMapPath.isNotEmpty())
            {
                currentMidiMapPath = savedMidiMapPath;
                currentMidiMapName = savedMidiMapName;
                
                juce::File midiMapFile(currentMidiMapPath);
                if (midiMapFile.existsAsFile())
                {
                    isLoadingMidiMap = true;
                    sampleEngine->loadMidiMap(midiMapFile);
                    isLoadingMidiMap = false;
                }
            }
            else
            {
                // Actualizar nombres aunque no recargue
                currentMidiMapPath = savedMidiMapPath;
                currentMidiMapName = savedMidiMapName;
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumSamplerProcessor();
}
