#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "DrumKitLoader.h"
#include "LockFreeRandom.h"
#include <atomic>
#include <unordered_map>
#include <utility>
#include <array>
#include <future>

class SampleEngine
{
public:
    SampleEngine();
    ~SampleEngine();

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    void loadKit(std::unique_ptr<DrumKit> kit, bool async = true);
    bool loadMidiMap(const juce::File& midiMapFile);
    bool isLoaded() const { return kitLoaded.load(); }
    
    const DrumSample* getSampleForNote(int midiNote, float velocity, float roundRobinAmount, juce::String* outInstrumentName = nullptr);
    juce::AudioBuffer<float>* getAudioBuffer(const DrumSample* sample, const juce::String& channelName);
    
    // Access to current kit for routing configuration
    const DrumKit* getCurrentKit() const { return currentKit.get(); }
    
    // Loading progress (for UI progress bar)
    int getLoadedSampleCount() const { return loadedSampleCount.load(); }
    int getTotalSampleCount() const { return totalSampleCount.load(); }
    float getLoadingProgress() const { 
        int total = totalSampleCount.load();
        return total > 0 ? static_cast<float>(loadedSampleCount.load()) / total : 0.0f;
    }
    
    // Callback para notificar cuando termina la carga
    std::function<void(bool)> loadingCallback;

private:
    void loadSamplesAsync();
    void loadSamplesSync();

    // Fase 1: Decode each unique audio file ONCE, extract all requested channels.
    // `channels` is a list of (fileChannel, cacheKey) pairs to extract from this file.
    bool loadUniqueFile(const juce::File& audioFile,
                        const std::vector<std::pair<int, juce::String>>& channels,
                        juce::AudioFormatManager& formatManager);

    void resampleBuffer(juce::AudioBuffer<float>& buffer, double sourceSampleRate, double targetSampleRate);

    // Fase 2: Disk cache (.dcc) — pre-decoded, resampled sample cache for instant reload.
    static juce::File getCacheDirectory();
    juce::File getCacheFileForKit(const juce::String& kitPath, double targetSR);
    uint64_t computeKitSignature(const DrumKit& kit, double targetSR);
    bool loadFromDiskCache(const juce::File& cacheFile, double expectedSR, uint64_t expectedSig);
    void writeDiskCacheAsync(const juce::File& cacheFile, double targetSR, uint64_t kitSig);
    void waitForCacheWrite();
    void pruneDiskCache();

    std::unique_ptr<DrumKit> currentKit;
    std::atomic<bool> kitLoaded{false};
    std::atomic<bool> isLoadingAsync{false};
    std::atomic<bool> shouldStopLoading{false};
    
    // Cache de buffers de audio cargados (optimized with unordered_map)
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>> audioBufferCache;
    // Sample rate the buffer is currently stored at (NOT the original file rate).
    // Updated after every resample so prepare() always resamples from the correct rate.
    std::unordered_map<juce::String, double> bufferSampleRates;
    juce::CriticalSection cacheLock;

    // Lock-free buffer access: atomic pointers for audio thread
    struct LockFreeBufferEntry {
        std::atomic<juce::AudioBuffer<float>*> bufferPtr{nullptr};
        std::atomic<bool> ready{false};
    };
    std::unordered_map<juce::String, std::unique_ptr<LockFreeBufferEntry>> lockFreeBufferCache;
    
    // Round robin tracking (optimized with unordered_map)
    // Using atomic<int> for thread-safe access from audio thread
    // NOTE: Using array instead of unordered_map<atomic> because atomic is not movable in C++17
    std::array<std::atomic<int>, 128> lastSampleIndex; // midiNote -> last used index
    
    // Instrument cache for faster lookups
    std::unordered_map<juce::String, Instrument*> instrumentCache;
    
    // Loading progress counters
    std::atomic<int> loadedSampleCount{0};
    std::atomic<int> totalSampleCount{0};
    
    double sampleRate = 44100.0;

    // Audio-thread RNG for round-robin sample selection (see LockFreeRandom.h).
    LockFreeRandom rrRng;

    // Pre-allocated arrays to avoid allocations in audio thread (getSampleForNote)
    static constexpr int maxCandidates = 8;
    static constexpr int maxSamplesPerInstrument = 32;
    const DrumSample* candidatePool[maxCandidates];
    std::pair<const DrumSample*, float> samplesWithDiffPool[maxSamplesPerInstrument];
    std::pair<int, float> weightedCandidatesPool[maxCandidates];
    std::pair<int, float> indexedCandidatesPool[maxCandidates];

    // Disk cache write thread handle
    std::future<void> cacheWriteFuture;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleEngine)
};
