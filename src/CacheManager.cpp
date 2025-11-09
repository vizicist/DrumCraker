#include "CacheManager.h"
#include <juce_audio_formats/juce_audio_formats.h>

CacheManager& CacheManager::getInstance()
{
    static CacheManager instance;
    return instance;
}

CacheManager::CacheManager()
{
    // Crear directorio de caché si no existe
    auto cacheDir = getCacheDirectory();
    if (!cacheDir.exists())
        cacheDir.createDirectory();
}

juce::File CacheManager::getCacheDirectory()
{
    // ~/.local/drumcraker/cache
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile(".local")
        .getChildFile("drumcraker")
        .getChildFile("cache");
}

juce::AudioBuffer<float>* CacheManager::getBuffer(const juce::String& cacheKey)
{
    juce::ScopedLock lock(cacheLock);
    
    auto it = cache.find(cacheKey);
    if (it != cache.end())
    {
        // Actualizar tiempo de último acceso
        it->second.lastAccessTime = juce::Time::currentTimeMillis();
        return it->second.buffer.get();
    }
    
    // Intentar cargar desde disco
    auto cacheFile = getCacheDirectory().getChildFile(cacheKey.replace("/", "_").replace(":", "_") + ".wav");
    if (cacheFile.existsAsFile())
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(cacheFile));
        if (reader != nullptr)
        {
            auto buffer = std::make_unique<juce::AudioBuffer<float>>(
                reader->numChannels, 
                static_cast<int>(reader->lengthInSamples));
            
            reader->read(buffer.get(), 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
            
            size_t bufferSize = buffer->getNumSamples() * buffer->getNumChannels() * sizeof(float);
            
            CacheEntry entry;
            entry.buffer = std::move(buffer);
            entry.lastAccessTime = juce::Time::currentTimeMillis();
            entry.size = bufferSize;
            
            currentCacheSize += bufferSize;
            auto* bufferPtr = entry.buffer.get();
            cache[cacheKey] = std::move(entry);
            
            cleanupIfNeeded();
            
            return bufferPtr;
        }
    }
    
    return nullptr;
}

void CacheManager::putBuffer(const juce::String& cacheKey, std::unique_ptr<juce::AudioBuffer<float>> buffer)
{
    if (!buffer)
        return;
    
    juce::ScopedLock lock(cacheLock);
    
    size_t bufferSize = buffer->getNumSamples() * buffer->getNumChannels() * sizeof(float);
    
    // Si ya existe, actualizar
    auto it = cache.find(cacheKey);
    if (it != cache.end())
    {
        currentCacheSize -= it->second.size;
        cache.erase(it);
    }
    
    CacheEntry entry;
    entry.buffer = std::move(buffer);
    entry.lastAccessTime = juce::Time::currentTimeMillis();
    entry.size = bufferSize;
    
    currentCacheSize += bufferSize;
    cache[cacheKey] = std::move(entry);
    
    cleanupIfNeeded();
    
    // Guardar en disco de forma asíncrona
    juce::Thread::launch([this, cacheKey]()
    {
        juce::ScopedLock lock(cacheLock);
        auto it = cache.find(cacheKey);
        if (it != cache.end())
        {
            auto cacheFile = getCacheDirectory().getChildFile(
                cacheKey.replace("/", "_").replace(":", "_") + ".wav");
            
            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> outStream(cacheFile.createOutputStream());
            
            if (outStream != nullptr)
            {
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    wavFormat.createWriterFor(outStream.get(), 48000.0, 
                                             it->second.buffer->getNumChannels(), 
                                             24, {}, 0));
                
                if (writer != nullptr)
                {
                    outStream.release(); // Writer toma ownership
                    writer->writeFromAudioSampleBuffer(*it->second.buffer, 0, 
                                                       it->second.buffer->getNumSamples());
                }
            }
        }
    });
}

void CacheManager::cleanupIfNeeded()
{
    // Si excedemos el límite, eliminar las entradas más antiguas
    while (currentCacheSize > maxCacheSize && !cache.empty())
    {
        // Encontrar la entrada más antigua
        auto oldestIt = cache.begin();
        juce::int64 oldestTime = oldestIt->second.lastAccessTime;
        
        for (auto it = cache.begin(); it != cache.end(); ++it)
        {
            if (it->second.lastAccessTime < oldestTime)
            {
                oldestTime = it->second.lastAccessTime;
                oldestIt = it;
            }
        }
        
        // Eliminar del disco también
        auto cacheFile = getCacheDirectory().getChildFile(
            oldestIt->first.replace("/", "_").replace(":", "_") + ".wav");
        if (cacheFile.existsAsFile())
            cacheFile.deleteFile();
        
        currentCacheSize -= oldestIt->second.size;
        cache.erase(oldestIt);
    }
}
