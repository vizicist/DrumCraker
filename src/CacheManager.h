#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>
#include <memory>

class CacheManager
{
public:
    static CacheManager& getInstance();
    
    // Obtener buffer desde caché o nullptr si no existe
    juce::AudioBuffer<float>* getBuffer(const juce::String& cacheKey);
    
    // Guardar buffer en caché
    void putBuffer(const juce::String& cacheKey, std::unique_ptr<juce::AudioBuffer<float>> buffer);
    
    // Limpiar caché si excede el límite
    void cleanupIfNeeded();
    
    // Obtener tamaño actual de caché en bytes
    size_t getCurrentCacheSize() const { return currentCacheSize; }
    
    // Límite de caché (1GB por defecto)
    void setMaxCacheSize(size_t maxSize) { maxCacheSize = maxSize; }
    
private:
    CacheManager();
    ~CacheManager() = default;
    
    struct CacheEntry
    {
        std::unique_ptr<juce::AudioBuffer<float>> buffer;
        juce::int64 lastAccessTime;
        size_t size;
    };
    
    std::map<juce::String, CacheEntry> cache;
    juce::CriticalSection cacheLock;
    
    size_t currentCacheSize = 0;
    size_t maxCacheSize = 1024 * 1024 * 1024; // 1GB
    
    juce::File getCacheDirectory();
    void loadCacheFromDisk();
    void saveCacheToDisk();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CacheManager)
};
