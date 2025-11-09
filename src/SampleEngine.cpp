#include "SampleEngine.h"
#include <set>
#include <algorithm>

#if JUCE_LINUX
    #include <malloc.h>
#endif

SampleEngine::SampleEngine() {}
SampleEngine::~SampleEngine() {}

void SampleEngine::prepare(double sr, int samplesPerBlock)
{
    sampleRate = sr;
}

void SampleEngine::reset()
{
    // Marcar como no cargado primero para detener cualquier acceso
    kitLoaded = false;
    
    // Esperar un momento para que cualquier thread de audio termine de usar los buffers
    juce::Thread::sleep(10);
    
    juce::ScopedLock lock(cacheLock);
    
    // Liberar explícitamente cada buffer antes de clear
    for (auto& entry : audioBufferCache)
    {
        if (entry.second)
        {
            entry.second->setSize(0, 0);
            entry.second.reset();
        }
    }
    
    // Limpiar y forzar shrink de los contenedores para devolver memoria al OS
    audioBufferCache.clear();
    std::map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>>().swap(audioBufferCache);
    
    originalSampleRates.clear();
    std::map<juce::String, double>().swap(originalSampleRates);
    
    lastSampleIndex.clear();
    std::map<int, int>().swap(lastSampleIndex);
    
    // Liberar el kit anterior
    currentKit.reset();
    
    // Forzar devolución de memoria al OS (Linux específico)
    #if JUCE_LINUX
        malloc_trim(0);
    #endif
}

void SampleEngine::loadKit(std::unique_ptr<DrumKit> kit, bool async)
{
    // Si ya tenemos un kit cargado
    if (currentKit && kitLoaded)
    {
        // Si es el mismo kit Y los samples están cargados, solo actualizar el kit
        if (currentKit->name == kit->name && !audioBufferCache.empty())
        {
            // Actualizar solo la estructura del kit (por si cambió algo)
            currentKit = std::move(kit);
            return;
        }
        // Es un kit diferente, resetear
        reset();
    }
    
    currentKit = std::move(kit);
    
    if (async)
        loadSamplesAsync();
    else
        loadSamplesSync();
}

bool SampleEngine::loadMidiMap(const juce::File& midiMapFile)
{
    if (!currentKit)
        return false;

    auto xml = juce::parseXML(midiMapFile);
    if (xml == nullptr || xml->getTagName() != "midimap")
        return false;

    // Limpiar midimap actual
    currentKit->midiMap.clear();

    // Parsear nuevo midimap
    for (auto* mapNode : xml->getChildIterator())
    {
        if (mapNode->hasTagName("map"))
        {
            int note = mapNode->getIntAttribute("note");
            juce::String instr = mapNode->getStringAttribute("instr");
            currentKit->midiMap[note] = instr;
        }
    }

    return !currentKit->midiMap.empty();
}

void SampleEngine::loadSamplesAsync()
{
    if (!currentKit)
        return;

    // Agrupar samples por archivo para evitar cargar el mismo archivo múltiples veces
    std::map<juce::String, std::vector<AudioSample>> fileGroups;
    
    for (const auto& instrument : currentKit->instruments)
    {
        for (const auto& sample : instrument->samples)
        {
            for (const auto& audioSample : sample.audioFiles)
            {
                juce::String filePath = audioSample.audioFile.getFullPathName();
                fileGroups[filePath].push_back(audioSample);
            }
        }
    }

    // Cargar samples en background thread
    juce::Thread::launch([this, fileGroups]()
    {
        // Cargar cada archivo UNA SOLA VEZ y extraer todos los canales necesarios
        for (const auto& fileGroup : fileGroups)
        {
            const juce::String& filePath = fileGroup.first;
            const std::vector<AudioSample>& channels = fileGroup.second;
            loadSampleFileOnce(filePath, channels);
        }
        
        kitLoaded = true;
        
        // Llamar callback si existe (copiar primero para thread-safety)
        std::function<void(bool)> callback;
        {
            juce::ScopedLock lock(cacheLock);
            if (loadingCallback)
            {
                callback = loadingCallback;
                loadingCallback = nullptr; // Limpiar para evitar llamadas múltiples
            }
        }
        
        if (callback)
            callback(true);
    });
}

void SampleEngine::loadSamplesSync()
{
    if (!currentKit)
        return;

    // Agrupar samples por archivo para evitar cargar el mismo archivo múltiples veces
    std::map<juce::String, std::vector<AudioSample>> fileGroups;
    
    for (const auto& instrument : currentKit->instruments)
    {
        for (const auto& sample : instrument->samples)
        {
            for (const auto& audioSample : sample.audioFiles)
            {
                juce::String filePath = audioSample.audioFile.getFullPathName();
                fileGroups[filePath].push_back(audioSample);
            }
        }
    }

    // Cargar samples de forma SÍNCRONA (para offline rendering)
    for (const auto& fileGroup : fileGroups)
    {
        const juce::String& filePath = fileGroup.first;
        const std::vector<AudioSample>& channels = fileGroup.second;
        loadSampleFileOnce(filePath, channels);
    }
    
    kitLoaded = true;
    
    if (loadingCallback)
        loadingCallback(true);
}

bool SampleEngine::loadSampleFileOnce(const juce::String& filePath, 
                                      const std::vector<AudioSample>& channels)
{
    juce::File audioFile(filePath);
    if (!audioFile.existsAsFile())
        return false;

    // Cargar archivo de audio UNA SOLA VEZ
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(audioFile));

    if (reader == nullptr)
        return false;

    double originalSampleRate = reader->sampleRate;
    int numSamples = static_cast<int>(reader->lengthInSamples);
    int numChannels = reader->numChannels;

    // Leer el archivo completo UNA SOLA VEZ en un buffer temporal
    juce::AudioBuffer<float> tempBuffer(numChannels, numSamples);
    if (!reader->read(&tempBuffer, 0, numSamples, 0, true, true))
        return false;

    // Extraer cada canal necesario del buffer en memoria (no del disco)
    for (const auto& audioSample : channels)
    {
        if (audioSample.fileChannel > numChannels)
            continue;

        juce::String cacheKey = filePath + "_ch" + juce::String(audioSample.fileChannel);
        
        // Verificar si ya está en caché
        {
            juce::ScopedLock lock(cacheLock);
            if (audioBufferCache.find(cacheKey) != audioBufferCache.end())
                continue;
        }

        // Extraer el canal específico del buffer temporal (operación en memoria, muy rápida)
        auto buffer = std::make_unique<juce::AudioBuffer<float>>(1, numSamples);
        buffer->copyFrom(0, 0, tempBuffer, audioSample.fileChannel - 1, 0, numSamples);

        // Resamplear si es necesario
        if (std::abs(originalSampleRate - sampleRate) > 0.1)
            resampleBuffer(*buffer, originalSampleRate, sampleRate);

        juce::ScopedLock lock(cacheLock);
        audioBufferCache[cacheKey] = std::move(buffer);
        originalSampleRates[cacheKey] = originalSampleRate;
    }
    
    return true;
}



void SampleEngine::resampleBuffer(juce::AudioBuffer<float>& buffer, 
                                  double sourceSampleRate, double targetSampleRate)
{
    if (std::abs(sourceSampleRate - targetSampleRate) < 0.1)
        return; // No es necesario resamplear

    double ratio = targetSampleRate / sourceSampleRate;
    int newLength = static_cast<int>(buffer.getNumSamples() * ratio);

    if (newLength <= 0)
        return;

    // Crear buffer temporal para el resultado
    juce::AudioBuffer<float> resampledBuffer(1, newLength);

    // Usar interpolación Lagrange de alta calidad
    juce::LagrangeInterpolator interpolator;
    interpolator.reset();

    const float* sourceData = buffer.getReadPointer(0);
    float* destData = resampledBuffer.getWritePointer(0);

    // Procesar usando el interpolador correctamente
    int samplesProcessed = interpolator.process(ratio, sourceData, destData, 
                                               newLength, buffer.getNumSamples(), 0);

    // Si no se procesaron todos los samples, copiar el resto
    if (samplesProcessed < newLength)
    {
        for (int i = samplesProcessed; i < newLength; ++i)
            destData[i] = 0.0f;
    }

    // Reemplazar el buffer original con el resampleado
    buffer.setSize(1, newLength, false, false, false);
    buffer.copyFrom(0, 0, resampledBuffer, 0, 0, newLength);
}

double SampleEngine::getOriginalSampleRate(const DrumSample* sample, const juce::String& channelName)
{
    if (!sample)
        return sampleRate;

    for (const auto& audioSample : sample->audioFiles)
    {
        if (audioSample.channelName == channelName)
        {
            juce::String cacheKey = audioSample.audioFile.getFullPathName() + 
                                   "_ch" + juce::String(audioSample.fileChannel);

            juce::ScopedLock lock(cacheLock);
            auto it = originalSampleRates.find(cacheKey);
            if (it != originalSampleRates.end())
                return it->second;
        }
    }

    return sampleRate;
}

const DrumSample* SampleEngine::getSampleForNote(int midiNote, float velocity, float roundRobinAmount)
{
    if (!currentKit)
        return nullptr;

    // Buscar instrumento para esta nota MIDI
    auto it = currentKit->midiMap.find(midiNote);
    if (it == currentKit->midiMap.end())
        return nullptr;

    const juce::String& instrumentName = it->second;
    
    // Encontrar el instrumento
    Instrument* targetInstrument = nullptr;
    for (const auto& instrument : currentKit->instruments)
    {
        if (instrument->name == instrumentName)
        {
            targetInstrument = instrument.get();
            break;
        }
    }

    if (!targetInstrument || targetInstrument->samples.empty())
        return nullptr;

    // Normalizar power values para encontrar el rango
    float minPower = targetInstrument->samples[0].power;
    float maxPower = targetInstrument->samples[0].power;
    
    for (const auto& sample : targetInstrument->samples)
    {
        minPower = std::min(minPower, sample.power);
        maxPower = std::max(maxPower, sample.power);
    }
    
    // Normalizar velocity al rango de power
    float normalizedVelocity = minPower + (velocity * (maxPower - minPower));
    
    // Encontrar samples candidatos basados en velocity (power)
    std::vector<const DrumSample*> candidates;
    float tolerance = (maxPower - minPower) * 0.25f; // 25% del rango total para más variedad

    for (const auto& sample : targetInstrument->samples)
    {
        float diff = std::abs(normalizedVelocity - sample.power);
        if (diff < tolerance)
            candidates.push_back(&sample);
    }

    // Si no hay candidatos en el rango, ampliar búsqueda
    if (candidates.empty())
    {
        // Buscar los 3-5 samples más cercanos para tener pool de round robin
        std::vector<std::pair<const DrumSample*, float>> samplesWithDiff;
        
        for (const auto& sample : targetInstrument->samples)
        {
            float diff = std::abs(normalizedVelocity - sample.power);
            samplesWithDiff.push_back({&sample, diff});
        }
        
        // Ordenar por cercanía
        std::sort(samplesWithDiff.begin(), samplesWithDiff.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Tomar los 4 más cercanos (o todos si hay menos)
        int numCandidates = std::min(4, static_cast<int>(samplesWithDiff.size()));
        for (int i = 0; i < numCandidates; ++i)
        {
            candidates.push_back(samplesWithDiff[i].first);
        }
    }
    
    // Asegurar que siempre hay al menos un candidato
    if (candidates.empty())
    {
        return &targetInstrument->samples[0];
    }

    // ROUND ROBIN ANTI-METRALLETA
    // Evita repetir el mismo sample consecutivamente
    int lastIndex = lastSampleIndex[midiNote];
    int selectedIndex = 0;

    if (candidates.size() == 1)
    {
        // Solo hay un candidato, usarlo
        selectedIndex = 0;
    }
    else if (roundRobinAmount < 0.01f)
    {
        // Modo velocity puro: siempre el más cercano
        float minDiff = std::abs(normalizedVelocity - candidates[0]->power);
        for (size_t i = 1; i < candidates.size(); ++i)
        {
            float diff = std::abs(normalizedVelocity - candidates[i]->power);
            if (diff < minDiff)
            {
                minDiff = diff;
                selectedIndex = static_cast<int>(i);
            }
        }
    }
    else if (roundRobinAmount > 0.99f)
    {
        // Modo rotación pura: siguiente sample diferente al último
        // Ordenar por cercanía a velocity
        std::vector<std::pair<int, float>> indexedCandidates;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            float diff = std::abs(normalizedVelocity - candidates[i]->power);
            indexedCandidates.push_back({static_cast<int>(i), diff});
        }
        
        std::sort(indexedCandidates.begin(), indexedCandidates.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Rotar entre los mejores candidatos, evitando el último usado
        int poolSize = std::min(4, static_cast<int>(indexedCandidates.size()));
        int nextIndex = (lastIndex + 1) % poolSize;
        selectedIndex = indexedCandidates[nextIndex].first;
    }
    else
    {
        // Modo híbrido inteligente: weighted random con penalización al último usado
        std::vector<std::pair<int, float>> weightedCandidates;
        float totalWeight = 0.0f;
        
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            float diff = std::abs(normalizedVelocity - candidates[i]->power);
            
            // Peso base: inversamente proporcional a la diferencia de velocity
            float weight = 1.0f / (1.0f + diff * 5.0f);
            
            // PENALIZACIÓN MUY FUERTE al último sample usado (evita metralleta)
            if (static_cast<int>(i) == lastIndex)
            {
                // Penalización escalada por roundRobinAmount
                // Con 0.7 default: penalización del 93%
                float penalty = 0.1f - (roundRobinAmount * 0.08f);
                weight *= juce::jmax(0.01f, penalty);
            }
            // Bonus al siguiente en rotación (más fuerte con roundRobinAmount alto)
            else if (static_cast<int>(i) == (lastIndex + 1) % static_cast<int>(candidates.size()))
            {
                weight *= (1.0f + roundRobinAmount * 1.5f);
            }
            
            weightedCandidates.push_back({static_cast<int>(i), weight});
            totalWeight += weight;
        }
        
        // Selección weighted random
        float randomValue = juce::Random::getSystemRandom().nextFloat() * totalWeight;
        float cumulative = 0.0f;
        
        for (const auto& wc : weightedCandidates)
        {
            cumulative += wc.second;
            if (randomValue <= cumulative)
            {
                selectedIndex = wc.first;
                break;
            }
        }
    }

    lastSampleIndex[midiNote] = selectedIndex;
    return candidates[selectedIndex];
}

juce::AudioBuffer<float>* SampleEngine::getAudioBuffer(const DrumSample* sample,
                                                       const juce::String& channelName)
{
    if (!sample)
        return nullptr;

    // Buscar el audio file para este canal
    for (const auto& audioSample : sample->audioFiles)
    {
        if (audioSample.channelName == channelName)
        {
            juce::String cacheKey = audioSample.audioFile.getFullPathName() + 
                                   "_ch" + juce::String(audioSample.fileChannel);

            juce::ScopedLock lock(cacheLock);
            auto it = audioBufferCache.find(cacheKey);
            if (it != audioBufferCache.end())
                return it->second.get();
            
            return nullptr;
        }
    }

    // El canal no existe en este sample
    return nullptr;
}
