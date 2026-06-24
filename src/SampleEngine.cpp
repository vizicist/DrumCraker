#include "SampleEngine.h"
#include <set>
#include <algorithm>
#include <utility>
#include <cstring>

#if JUCE_LINUX
    #include <malloc.h>
#endif

SampleEngine::SampleEngine()
{
    for (auto& idx : lastSampleIndex)
        idx.store(-1, std::memory_order_relaxed);
}

SampleEngine::~SampleEngine()
{
    // CRITICAL: Signal all threads to stop IMMEDIATELY
    shouldStopLoading = true;
    kitLoaded = false;

    // Wait for any background cache write to finish before destroying buffers
    waitForCacheWrite();

    // Clear callback immediately to prevent use-after-free
    {
        juce::ScopedLock lock(cacheLock);
        loadingCallback = nullptr;
    }
    
    // Wait for async loading to finish (with longer timeout for thread pool)
    int maxWait = 100; // Increased timeout for thread pool cleanup
    while (isLoadingAsync.load() && maxWait-- > 0)
    {
        juce::Thread::sleep(50);
    }
    
    // Force flag to false if timeout
    isLoadingAsync = false;
    
    // Ensure all memory is freed on destruction
    reset();
}

void SampleEngine::prepare(double sr, int samplesPerBlock)
{
    if (sr <= 0.0)
        return;

    // Seed the round-robin RNG per-instance so duplicated kits don't pick
    // identical sequences. Cheap and idempotent.
    const uint64_t seedMix = static_cast<uint64_t>(juce::Time::getHighResolutionTicks())
                           ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
    rrRng.seed(static_cast<uint32_t>(seedMix ^ (seedMix >> 32)));
        
    // If sample rate changes and we have samples loaded, we MUST resample
    // Otherwise render speed will be wrong (pitch shift + sync loss)
    if (sampleRate > 0.0 && sr != sampleRate && !audioBufferCache.empty())
    {
        juce::ScopedLock lock(cacheLock);
        
        // Iterate all buffers and resample in place from their CURRENT rate
        // (bufferSampleRates tracks the rate each buffer is actually stored at)
        // to the NEW target rate. Always resamples from the correct rate, no
        // cumulative quality loss from stale "original" rate assumptions.
        for (auto& entry : audioBufferCache)
        {
            if (entry.second)
            {
                double currentRate = bufferSampleRates[entry.first];
                resampleBuffer(*entry.second, currentRate, sr);
                bufferSampleRates[entry.first] = sr;
            }
        }
    }

    sampleRate = sr;
}

void SampleEngine::reset()
{
    // Signal cancellation FIRST
    shouldStopLoading = true;
    kitLoaded = false;

    // Wait for background cache write to finish (it reads the buffers we're about to free)
    waitForCacheWrite();

    // Clear callback safely BEFORE waiting
    {
        juce::ScopedLock lock(cacheLock);
        loadingCallback = nullptr;
    }
    
    // Wait for async thread to finish (longer timeout for thread pool)
    int maxWait = 50;
    while (isLoadingAsync.load() && maxWait-- > 0)
    {
        juce::Thread::sleep(50);
    }
    
    // Force flag to false if timeout
    isLoadingAsync = false;
    
    // Lock for cleanup operations
    juce::ScopedLock lock(cacheLock);
    
    // Free audio buffers efficiently
    for (auto& entry : audioBufferCache)
    {
        if (entry.second)
        {
            entry.second->setSize(0, 0);
            entry.second.reset();
        }
    }
    
    // Clear containers and return memory to OS
    audioBufferCache.clear();
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>>().swap(audioBufferCache);

    bufferSampleRates.clear();
    std::unordered_map<juce::String, double>().swap(bufferSampleRates);

    // Clear lock-free cache
    for (auto& entry : lockFreeBufferCache)
    {
        if (entry.second)
        {
            entry.second->ready.store(false, std::memory_order_release);
            entry.second->bufferPtr.store(nullptr, std::memory_order_release);
        }
    }
    lockFreeBufferCache.clear();
    std::unordered_map<juce::String, std::unique_ptr<LockFreeBufferEntry>>().swap(lockFreeBufferCache);

    // Reset lastSampleIndex array (atomic values)
    for (auto& idx : lastSampleIndex)
        idx.store(-1, std::memory_order_relaxed);

    instrumentCache.clear();
    std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);

    currentKit.reset();

    // Force memory return to OS (Linux-specific)
    // On macOS/Windows, this does nothing but is harmless
    #if JUCE_LINUX
        malloc_trim(0);
    #endif
}

void SampleEngine::loadKit(std::unique_ptr<DrumKit> kit, bool async)
{
    if (!kit)
        return;
    
    // If we already have a kit loaded
    if (currentKit && kitLoaded.load())
    {
        // If it's the same kit AND samples are loaded, reuse
        if (currentKit->name == kit->name && !audioBufferCache.empty())
        {
            // Update only kit structure (in case something changed)
            currentKit = std::move(kit);
            
            // Call callback immediately since kit is already loaded
            if (loadingCallback)
            {
                auto callback = loadingCallback;
                loadingCallback = nullptr;
                callback(true);
            }
            return;
        }
    }
    
    // CRITICAL: If loading a different kit, cancel previous load SAFELY
    if (isLoadingAsync.load())
    {
        // Signal cancellation
        shouldStopLoading = true;
        
        // Clear callback to prevent use-after-free
        {
            juce::ScopedLock lock(cacheLock);
            loadingCallback = nullptr;
        }
        
        // Wait for thread pool to finish (longer timeout)
        int maxWait = 50;
        while (isLoadingAsync.load() && maxWait-- > 0)
        {
            juce::Thread::sleep(50);
        }
        
        // Force flag to false if timeout
        isLoadingAsync = false;
        
        kitLoaded = false;
    }

    // Mark as not loaded while we're loading
    kitLoaded = false;

    // CRITICAL: Signal the cache write thread to abort early (we're about to
    // discard the old kit's buffers it may still be reading). Then wait for
    // it to finish BEFORE freeing anything. This prevents use-after-free.
    shouldStopLoading = true;
    waitForCacheWrite();
    shouldStopLoading = false;

    // Now safe to clear old kit data - force memory release
    {
        juce::ScopedLock lock(cacheLock);
        
        // Explicitly free all audio buffers before clearing
        for (auto& entry : audioBufferCache)
        {
            if (entry.second)
            {
                entry.second->setSize(0, 0, false, false, false);
                entry.second.reset();
            }
        }
        
        // Clear and swap to force memory deallocation
        audioBufferCache.clear();
        std::unordered_map<juce::String, std::unique_ptr<juce::AudioBuffer<float>>>().swap(audioBufferCache);

        bufferSampleRates.clear();
        std::unordered_map<juce::String, double>().swap(bufferSampleRates);

        // Clear lock-free cache
        for (auto& entry : lockFreeBufferCache)
        {
            if (entry.second)
            {
                entry.second->ready.store(false, std::memory_order_release);
                entry.second->bufferPtr.store(nullptr, std::memory_order_release);
            }
        }
        lockFreeBufferCache.clear();
        std::unordered_map<juce::String, std::unique_ptr<LockFreeBufferEntry>>().swap(lockFreeBufferCache);

        // Reset lastSampleIndex array (atomic values)
        for (auto& idx : lastSampleIndex)
            idx.store(-1, std::memory_order_relaxed);

        instrumentCache.clear();
        std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);
    }

    // Force memory return to OS (Linux-specific)
    // On macOS/Windows, this does nothing but is harmless
    #if JUCE_LINUX
        malloc_trim(0);
    #endif
    
    // Set new kit
    currentKit = std::move(kit);
    
    // Load samples
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

    // Clear current midimap
    currentKit->midiMap.clear();

    // Parse new midimap
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

// ============================================================================
// FASE 1 + 2: Async sample loading with grouped decode and disk cache
// ============================================================================

void SampleEngine::loadSamplesAsync()
{
    if (!currentKit)
        return;

    // Mark that we're loading
    isLoadingAsync = true;
    shouldStopLoading = false;
    
    // Reset progress counters
    loadedSampleCount = 0;
    totalSampleCount = 0;

    juce::Thread::launch([this]()
    {
        // CRITICAL: isLoadingAsync MUST be the last thing cleared before the
        // thread exits. The destructor waits on this flag — if it's cleared
        // before the thread finishes accessing `this`, the destructor can
        // destroy the object while the thread is still running.
        struct ScopeGuard {
            std::atomic<bool>& flag;
            ~ScopeGuard() { flag.store(false, std::memory_order_release); }
        } guard{ isLoadingAsync };

        if (!currentKit || shouldStopLoading.load())
            return;

        const double targetSR = sampleRate;

        // ---- FASE 2: Try disk cache first (near-instant on hit) ----
        const uint64_t kitSig = computeKitSignature(*currentKit, targetSR);
        juce::File cacheFile = getCacheFileForKit(currentKit->kitFile.getFullPathName(), targetSR);

        if (loadFromDiskCache(cacheFile, targetSR, kitSig))
        {
            // Cache hit — samples are ready
            kitLoaded = true;

            std::function<void(bool)> callback;
            {
                juce::ScopedLock lock(cacheLock);
                if (loadingCallback)
                {
                    callback = loadingCallback;
                    loadingCallback = nullptr;
                }
            }
            if (callback && !shouldStopLoading.load())
                callback(true);
            return;
        }

        // ---- FASE 1: Cache miss — decode from source files ----
        // Group all audiofile requests by unique file path so each file is
        // decoded ONCE and all requested channels are extracted from a single
        // read. This eliminates the 10-16x redundant decoding of the old
        // per-channel approach.
        struct FileJob
        {
            juce::File file;
            std::vector<std::pair<int, juce::String>> channels; // (fileChannel, cacheKey)
        };
        std::unordered_map<juce::String, FileJob> jobsByPath;

        for (const auto& instrument : currentKit->instruments)
        {
            if (shouldStopLoading.load())
            {
                isLoadingAsync = false;
                return;
            }

            for (const auto& sample : instrument->samples)
            {
                for (const auto& audioSample : sample.audioFiles)
                {
                    const juce::String path = audioSample.audioFile.getFullPathName();
                    auto& job = jobsByPath[path];
                    job.file = audioSample.audioFile;
                    job.channels.emplace_back(audioSample.fileChannel,
                                              path + "_ch" + juce::String(audioSample.fileChannel));
                }
            }
        }

        // Progress is tracked per unique file (not per channel)
        totalSampleCount = static_cast<int>(jobsByPath.size());

        // Shared format manager (thread-safe for createReaderFor — the format
        // list is immutable after registration)
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        // Use thread pool for parallel loading (90% of available cores)
        const int numCpus = juce::SystemStats::getNumCpus();
        const int numThreads = juce::jmax(2, static_cast<int>(numCpus * 0.9f));
        juce::ThreadPool pool(numThreads);
        std::atomic<bool> hasError{false};

        for (const auto& kv : jobsByPath)
        {
            if (shouldStopLoading.load())
                break;

            const auto& job = kv.second;
            // CRITICAL: Capture by value (the map outlives the pool, but be safe)
            pool.addJob([this, &job, &formatManager, &hasError]()
            {
                if (shouldStopLoading.load() || hasError.load())
                    return;
                try
                {
                    if (!loadUniqueFile(job.file, job.channels, formatManager))
                        hasError = true;
                    loadedSampleCount++;
                }
                catch (...)
                {
                    hasError = true;
                }
            });
        }

        // CRITICAL: Wait for ALL jobs to complete before exiting
        // This ensures no job is accessing 'this' after destruction
        while (pool.getNumJobs() > 0)
        {
            if (shouldStopLoading.load())
            {
                // Cancel remaining jobs but wait for active ones
                pool.removeAllJobs(true, 5000); // Wait up to 5 seconds
                break;
            }
            juce::Thread::sleep(5);
        }

        // Ensure pool is fully stopped before continuing
        pool.removeAllJobs(true, 2000);

        // Only mark as loaded if we completed successfully
        if (!shouldStopLoading.load() && !hasError.load())
        {
            kitLoaded = true;

            // ---- FASE 2: Write disk cache in background (non-blocking) ----
            writeDiskCacheAsync(cacheFile, targetSR, kitSig);
        }

        // Call callback if exists (thread-safe)
        std::function<void(bool)> callback;
        {
            juce::ScopedLock lock(cacheLock);
            if (loadingCallback)
            {
                callback = loadingCallback;
                loadingCallback = nullptr;
            }
        }

        if (callback && !shouldStopLoading.load())
            callback(!hasError.load());

        // isLoadingAsync is cleared by ScopeGuard at thread exit — AFTER all
        // access to `this` is complete. This prevents the destructor from
        // destroying the object while the thread is still running.
    });
}

void SampleEngine::loadSamplesSync()
{
    if (!currentKit)
        return;

    const double targetSR = sampleRate;

    // ---- FASE 2: Try disk cache first ----
    const uint64_t kitSig = computeKitSignature(*currentKit, targetSR);
    juce::File cacheFile = getCacheFileForKit(currentKit->kitFile.getFullPathName(), targetSR);

    if (loadFromDiskCache(cacheFile, targetSR, kitSig))
    {
        kitLoaded = true;
        if (loadingCallback)
            loadingCallback(true);
        return;
    }

    // ---- FASE 1: Cache miss — decode from source (synchronous) ----
    using ChannelReq = std::pair<int, juce::String>;
    std::unordered_map<juce::String, std::pair<juce::File, std::vector<ChannelReq>>> jobsByPath;

    for (const auto& instrument : currentKit->instruments)
    {
        for (const auto& sample : instrument->samples)
        {
            for (const auto& audioSample : sample.audioFiles)
            {
                const juce::String path = audioSample.audioFile.getFullPathName();
                auto& job = jobsByPath[path];
                job.first = audioSample.audioFile;
                job.second.emplace_back(audioSample.fileChannel,
                                        path + "_ch" + juce::String(audioSample.fileChannel));
            }
        }
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    for (const auto& kv : jobsByPath)
    {
        loadUniqueFile(kv.second.first, kv.second.second, formatManager);
    }

    kitLoaded = true;

    // Write disk cache in background
    writeDiskCacheAsync(cacheFile, targetSR, kitSig);

    if (loadingCallback)
        loadingCallback(true);
}

// ============================================================================
// FASE 1: Decode one unique file and extract all requested channels
// ============================================================================

bool SampleEngine::loadUniqueFile(const juce::File& audioFile,
                                  const std::vector<std::pair<int, juce::String>>& channels,
                                  juce::AudioFormatManager& formatManager)
{
    if (!audioFile.existsAsFile())
        return false;

    // Skip channels already cached (e.g., from a partial previous load)
    std::vector<std::pair<int, juce::String>> toLoad;
    {
        juce::ScopedLock lock(cacheLock);
        for (const auto& ch : channels)
        {
            if (audioBufferCache.find(ch.second) == audioBufferCache.end())
                toLoad.push_back(ch);
        }
    }

    if (toLoad.empty())
        return true;

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
    if (reader == nullptr)
        return false;

    const double originalSampleRate = reader->sampleRate;
    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int numChannels = reader->numChannels;

    if (numSamples <= 0 || numChannels <= 0)
        return false;

    // Decode the entire file ONCE into a multichannel buffer.
    // Uses the correct multi-channel read API (float* const*) instead of the
    // 2-channel-only read(AudioBuffer*, ..., bool, bool) which silently leaves
    // channels > 1 as silence for files with more than 2 channels.
    juce::AudioBuffer<float> fullBuffer(numChannels, numSamples);
    fullBuffer.clear();

    if (numChannels == 1)
    {
        if (!reader->read(&fullBuffer, 0, numSamples, 0, true, false))
            return false;
    }
    else
    {
        // Read ALL channels in one pass — the correct API for >2 channel files
        std::vector<float*> chanPtrs(numChannels);
        for (int c = 0; c < numChannels; ++c)
            chanPtrs[c] = fullBuffer.getWritePointer(c);

        if (!reader->read(chanPtrs.data(), numChannels, 0, numSamples))
            return false;
    }

    // Close the file ASAP (free the file handle)
    reader.reset();

    // Extract each requested channel into its own mono buffer, resample, and
    // publish to the caches. Resampling is done OUTSIDE the lock so concurrent
    // prepare() calls aren't blocked by per-channel DSP work.
    for (const auto& ch : toLoad)
    {
        const int fileChannel = ch.first;
        const juce::String& cacheKey = ch.second;

        if (fileChannel < 1 || fileChannel > numChannels)
            continue;

        auto monoBuffer = std::make_unique<juce::AudioBuffer<float>>(1, numSamples);
        monoBuffer->copyFrom(0, 0, fullBuffer, fileChannel - 1, 0, numSamples);

        // Determine the rate the buffer will be stored at after resampling
        double bufferRate = originalSampleRate;
        if (sampleRate > 0.0 && std::abs(originalSampleRate - sampleRate) > 0.1)
        {
            resampleBuffer(*monoBuffer, originalSampleRate, sampleRate);
            bufferRate = sampleRate;
        }

        // Brief lock just to publish into the caches
        {
            juce::ScopedLock lock(cacheLock);
            audioBufferCache[cacheKey] = std::move(monoBuffer);
            bufferSampleRates[cacheKey] = bufferRate;

            auto& lfEntry = lockFreeBufferCache[cacheKey];
            if (!lfEntry)
                lfEntry = std::make_unique<LockFreeBufferEntry>();

            juce::AudioBuffer<float>* bufPtr = audioBufferCache[cacheKey].get();
            lfEntry->bufferPtr.store(bufPtr, std::memory_order_release);
            lfEntry->ready.store(true, std::memory_order_release);
        }
    }

    return true;
}

void SampleEngine::resampleBuffer(juce::AudioBuffer<float>& buffer, 
                                  double sourceSampleRate, double targetSampleRate)
{
    // Validate sample rates
    if (sourceSampleRate <= 0.0 || targetSampleRate <= 0.0)
        return;
    
    if (std::abs(sourceSampleRate - targetSampleRate) < 0.1)
        return; // No resampling needed

    // Calculate speed ratio for interpolator (source/target)
    // For upsampling (44.1->48kHz): ratio = 44100/48000 = 0.91875
    // For downsampling (48->44.1kHz): ratio = 48000/44100 = 1.08843
    const double speedRatio = sourceSampleRate / targetSampleRate;
    
    // Calculate new buffer length
    const int originalLength = buffer.getNumSamples();
    const int newLength = static_cast<int>(std::ceil(originalLength / speedRatio));

    if (newLength <= 0 || newLength > originalLength * 4) // Sanity check
        return;

    // Create temporary buffer for result
    juce::AudioBuffer<float> resampledBuffer(1, newLength);
    resampledBuffer.clear();

    const float* sourceData = buffer.getReadPointer(0);
    float* destData = resampledBuffer.getWritePointer(0);
    
    // Determine which interpolator to use based on ratio difference
    const double ratioError = std::abs(speedRatio - 1.0);
    
    if (ratioError < 0.15) // Common conversions: 44.1<->48kHz
    {
        // Fast linear interpolation for common sample rate conversions
        juce::LinearInterpolator interpolator;
        interpolator.reset();
        
        int samplesProcessed = interpolator.process(speedRatio, sourceData, destData, 
                                                   newLength, originalLength, 0);
        
        // Fill rest with silence if needed
        if (samplesProcessed < newLength)
            juce::FloatVectorOperations::clear(destData + samplesProcessed, newLength - samplesProcessed);
    }
    else
    {
        // High-quality Lagrange for larger differences
        juce::LagrangeInterpolator interpolator;
        interpolator.reset();
        
        int samplesProcessed = interpolator.process(speedRatio, sourceData, destData, 
                                                   newLength, originalLength, 0);
        
        // Fill rest with silence if needed
        if (samplesProcessed < newLength)
            juce::FloatVectorOperations::clear(destData + samplesProcessed, newLength - samplesProcessed);
    }

    // Replace original buffer with resampled one
    buffer.setSize(1, newLength, false, false, false);
    buffer.copyFrom(0, 0, resampledBuffer, 0, 0, newLength);
}

// ============================================================================
// FASE 2: Disk cache (.dcc) — pre-decoded, resampled sample cache
// ============================================================================

juce::File SampleEngine::getCacheDirectory()
{
#if JUCE_WINDOWS
    // %LOCALAPPDATA%\DrumCraker\cache
    juce::String localApp = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", "");
    juce::File dir;
    if (localApp.isNotEmpty())
        dir = juce::File(localApp).getChildFile("DrumCraker").getChildFile("cache");
    else
        dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
              .getParentDirectory().getChildFile("Local")
              .getChildFile("DrumCraker").getChildFile("cache");
#elif JUCE_MAC
    juce::File dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile("Library").getChildFile("Caches").getChildFile("DrumCraker");
#else
    // Linux, FreeBSD — respect XDG_CACHE_HOME
    juce::String xdg = juce::SystemStats::getEnvironmentVariable("XDG_CACHE_HOME", "");
    juce::File dir;
    if (xdg.isNotEmpty())
        dir = juce::File(xdg).getChildFile("DrumCraker");
    else
        dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
              .getChildFile(".cache").getChildFile("DrumCraker");
#endif
    dir.createDirectory();
    return dir;
}

juce::File SampleEngine::getCacheFileForKit(const juce::String& kitPath, double targetSR)
{
    // FNV-1a 64-bit hash of (kitPath | targetSR) for a unique cache filename
    const juce::String hashInput = kitPath + "|" + juce::String(targetSR, 2);
    uint64_t h = 14695981039346656037ULL;
    auto utf8 = hashInput.toUTF8();
    for (size_t i = 0; i < utf8.sizeInBytes() - 1; ++i)
    {
        h ^= static_cast<uint8_t>(utf8[i]);
        h *= 1099511628211ULL;
    }
    return getCacheDirectory().getChildFile(juce::String::toHexString(static_cast<int64_t>(h)) + ".dcc");
}

uint64_t SampleEngine::computeKitSignature(const DrumKit& kit, double targetSR)
{
    // FNV-1a 64-bit hash of: kit XML path + mtime + size, all unique source
    // file paths + mtimes + sizes, and the target sample rate. This detects
    // any change to the kit or its samples and invalidates the cache.
    uint64_t h = 14695981039346656037ULL;

    auto mix = [&](const void* data, size_t len)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };

    auto mixStr = [&](const juce::String& s)
    {
        auto utf8 = s.toUTF8();
        mix(utf8.getAddress(), utf8.sizeInBytes() - 1); // exclude null terminator
    };

    // Kit XML file identity
    if (kit.kitFile.existsAsFile())
    {
        mixStr(kit.kitFile.getFullPathName());
        const int64_t mtime = kit.kitFile.getLastModificationTime().toMilliseconds();
        const int64_t size = kit.kitFile.getSize();
        mix(&mtime, sizeof(mtime));
        mix(&size, sizeof(size));
    }

    // All unique source audio files (sorted for deterministic ordering)
    std::set<juce::String> paths;
    for (const auto& instr : kit.instruments)
    {
        for (const auto& sample : instr->samples)
        {
            for (const auto& af : sample.audioFiles)
                paths.insert(af.audioFile.getFullPathName());
        }
    }

    for (const auto& path : paths)
    {
        mixStr(path);
        juce::File f(path);
        const int64_t m = f.getLastModificationTime().toMilliseconds();
        const int64_t s = f.getSize();
        mix(&m, sizeof(m));
        mix(&s, sizeof(s));
    }

    // Target sample rate — different SR = different cache
    mix(&targetSR, sizeof(targetSR));

    return h;
}

bool SampleEngine::loadFromDiskCache(const juce::File& cacheFile, double expectedSR, uint64_t expectedSig)
{
    if (!cacheFile.existsAsFile())
        return false;

    // Incremental GZIP decompression — no need to load the entire file into RAM.
    // FileInputStream reads sequentially (fast on SSD/HDD), GZIPDecompressorInputStream
    // decompresses on-the-fly. Each entry's sample data is read directly into its
    // AudioBuffer, so peak memory is just the buffers themselves (no intermediate copy).
    juce::FileInputStream rawIn(cacheFile);
    if (!rawIn.openedOk())
        return false;

    juce::GZIPDecompressorInputStream in(rawIn);

    // Helper: read exactly `len` bytes from the decompressing stream (handles
    // short reads that GZIPDecompressorInputStream may return).
    auto readExact = [&](void* dest, size_t len) -> bool
    {
        size_t totalRead = 0;
        while (totalRead < len)
        {
            auto bytesRead = in.read(static_cast<char*>(dest) + totalRead,
                                     static_cast<int>(len - totalRead));
            if (bytesRead <= 0)
                return false;
            totalRead += static_cast<size_t>(bytesRead);
        }
        return true;
    };

    // ---- Header (32 bytes) ----
    char magic[4];
    if (!readExact(magic, 4))
        return false;
    if (std::memcmp(magic, "DCC2", 4) != 0)
        return false; // Unknown or old uncompressed format — regenerate

    uint32_t version = 0;
    if (!readExact(&version, 4))
        return false;
    if (version != 2)
        return false; // Incompatible cache version — regenerate

    double cacheSR = 0.0;
    if (!readExact(&cacheSR, 8))
        return false;
    if (std::abs(cacheSR - expectedSR) > 0.1)
        return false; // Different sample rate — cache miss

    uint64_t cacheSig = 0;
    if (!readExact(&cacheSig, 8))
        return false;
    if (cacheSig != expectedSig)
        return false; // Kit changed — cache miss

    uint32_t numEntries = 0;
    if (!readExact(&numEntries, 4))
        return false;

    // ---- Entries ----
    // Read each entry's data WITHOUT holding cacheLock (disk I/O can take
    // seconds for large kits). Only lock briefly to publish each entry into
    // the caches. This allows prepare() to interleave between entries if the
    // DAW calls it during load.
    for (uint32_t i = 0; i < numEntries; ++i)
    {
        if (shouldStopLoading.load())
            return false;

        uint32_t keyLen = 0;
        if (!readExact(&keyLen, 4))
            return false;

        std::string keyBuf(keyLen, '\0');
        if (!readExact(keyBuf.data(), keyLen))
            return false;
        juce::String cacheKey = juce::String::fromUTF8(keyBuf.data(), static_cast<int>(keyLen));

        uint32_t numSamples = 0;
        if (!readExact(&numSamples, 4))
            return false;

        const size_t dataLen = static_cast<size_t>(numSamples) * sizeof(float);

        auto buffer = std::make_unique<juce::AudioBuffer<float>>(1, static_cast<int>(numSamples));
        // Read sample data directly into the AudioBuffer (zero-copy from decompressor)
        if (!readExact(buffer->getWritePointer(0), dataLen))
            return false;

        // Brief lock to publish into the caches
        {
            juce::ScopedLock lock(cacheLock);
            // Buffer is stored at cacheSR (= expectedSR = current project SR)
            bufferSampleRates[cacheKey] = cacheSR;
            audioBufferCache[cacheKey] = std::move(buffer);

            auto& lfEntry = lockFreeBufferCache[cacheKey];
            if (!lfEntry)
                lfEntry = std::make_unique<LockFreeBufferEntry>();
            lfEntry->bufferPtr.store(audioBufferCache[cacheKey].get(), std::memory_order_release);
            lfEntry->ready.store(true, std::memory_order_release);
        }
    }

    return true;
}

void SampleEngine::writeDiskCacheAsync(const juce::File& cacheFile, double targetSR, uint64_t kitSig)
{
    // Wait for any previous write to finish before starting a new one
    waitForCacheWrite();

    // Use a promise so waitForCacheWrite() can block until the thread finishes.
    auto promise = std::make_shared<std::promise<void>>();
    cacheWriteFuture = promise->get_future();

    // Launch at background priority so cache writing doesn't compete with
    // audio, UI, or other real-time work for CPU and I/O.
    if (!juce::Thread::launch(juce::Thread::Priority::background,
        [this, cacheFile, targetSR, kitSig, promise]()
    {
        // Ensure the promise is always fulfilled, even on early return.
        struct ScopeGuard {
            std::shared_ptr<std::promise<void>> p;
            ~ScopeGuard() { try { p->set_value(); } catch (...) {} }
        } guard{ promise };

        // Snapshot the list of cache keys under a brief lock.
        std::vector<juce::String> keys;
        {
            juce::ScopedLock lock(cacheLock);
            keys.reserve(audioBufferCache.size());
            for (const auto& kv : audioBufferCache)
                keys.push_back(kv.first);
        }

        juce::FileOutputStream rawOut(cacheFile);
        if (!rawOut.openedOk())
            return;

        // GZIP compression — reduces cache file size by ~50-70% compared to
        // raw float32, cutting disk I/O and space usage dramatically.
        // Level 1 = fastest (good enough for audio sample data).
        juce::GZIPCompressorOutputStream out(rawOut, 1);

        // Write header (32 bytes) — DCC2 = compressed format
        const char magic[4] = { 'D', 'C', 'C', '2' };
        out.write(magic, 4);
        const uint32_t version = 2;
        out.write(&version, 4);
        out.write(&targetSR, 8);
        out.write(&kitSig, 8);
        const uint32_t numEntries = static_cast<uint32_t>(keys.size());
        out.write(&numEntries, 4);

        // Write entries — read directly from the cached buffer under a brief
        // per-entry lock (NO intermediate copy). This avoids doubling memory
        // usage during cache write. The lock is held only for the duration of
        // the sequential write call, which is fast (buffered I/O).
        for (const auto& key : keys)
        {
            if (shouldStopLoading.load())
            {
                out.flush();
                return;
            }

            uint32_t numSamples = 0;
            const float* dataPtr = nullptr;

            {
                juce::ScopedLock lock(cacheLock);
                auto it = audioBufferCache.find(key);
                if (it == audioBufferCache.end() || !it->second)
                    continue; // Buffer was freed (kit changed) — skip
                numSamples = static_cast<uint32_t>(it->second->getNumSamples());
                dataPtr = it->second->getReadPointer(0);
            }

            const std::string keyUtf8 = key.toStdString();
            const uint32_t keyLen = static_cast<uint32_t>(keyUtf8.size());
            out.write(&keyLen, 4);
            out.write(keyUtf8.data(), keyLen);
            out.write(&numSamples, 4);
            // Write directly from the cached buffer — no copy, no extra memory
            out.write(dataPtr, static_cast<size_t>(numSamples) * sizeof(float));
        }

        out.flush();

        // Prune stale cache files after writing a new one (best-effort,
        // non-blocking, runs at background priority).
        pruneDiskCache();
    }))
    {
        // Thread creation failed — fulfill the promise immediately so
        // waitForCacheWrite() doesn't hang.
        promise->set_value();
    }
}

void SampleEngine::waitForCacheWrite()
{
    if (cacheWriteFuture.valid())
        cacheWriteFuture.wait();
}

void SampleEngine::pruneDiskCache()
{
    // Best-effort cleanup of stale .dcc files. Runs at background priority
    // after a new cache file is written. Removes .dcc files older than 30 days
    // and enforces a total cache directory size limit of 5 GB (keeps the most
    // recently used files). This prevents orphaned cache files from kits that
    // were changed or deleted from accumulating forever.
    juce::Thread::launch(juce::Thread::Priority::background, []()
    {
        const juce::File cacheDir = getCacheDirectory();
        if (!cacheDir.isDirectory())
            return;

        juce::Array<juce::File> cacheFiles;
        cacheDir.findChildFiles(cacheFiles, juce::File::findFiles, false, "*.dcc");

        if (cacheFiles.isEmpty())
            return;

        const juce::Time now = juce::Time::getCurrentTime();
        const int64_t maxAgeMs = 30LL * 24 * 60 * 60 * 1000; // 30 days
        const int64_t maxTotalBytes = 5LL * 1024 * 1024 * 1024; // 5 GB

        // Phase 1: Remove files older than 30 days
        int64_t totalSize = 0;
        for (const auto& f : cacheFiles)
        {
            const int64_t age = now.toMilliseconds() - f.getLastModificationTime().toMilliseconds();
            if (age > maxAgeMs)
            {
                f.deleteFile();
            }
            else
            {
                totalSize += f.getSize();
            }
        }

        // Phase 2: If still over the size limit, remove oldest files first
        if (totalSize > maxTotalBytes)
        {
            // Re-scan surviving files and sort by modification time (oldest first)
            juce::Array<juce::File> survivors;
            cacheDir.findChildFiles(survivors, juce::File::findFiles, false, "*.dcc");

            std::sort(survivors.begin(), survivors.end(),
                [](const juce::File& a, const juce::File& b)
                {
                    return a.getLastModificationTime() < b.getLastModificationTime();
                });

            for (const auto& f : survivors)
            {
                if (totalSize <= maxTotalBytes)
                    break;
                totalSize -= f.getSize();
                f.deleteFile();
            }
        }
    });
}

const DrumSample* SampleEngine::getSampleForNote(int midiNote, float velocity, float roundRobinAmount, juce::String* outInstrumentName)
{
    if (!currentKit)
        return nullptr;

    // Find instrument for this MIDI note
    auto it = currentKit->midiMap.find(midiNote);
    if (it == currentKit->midiMap.end())
        return nullptr;

    const juce::String& instrumentName = it->second;

    // Return instrument name if requested
    if (outInstrumentName)
        *outInstrumentName = instrumentName;

    // Use cached instrument lookup for performance
    Instrument* targetInstrument = nullptr;
    auto cacheIt = instrumentCache.find(instrumentName);
    if (cacheIt != instrumentCache.end())
    {
        targetInstrument = cacheIt->second;
    }
    else
    {
        // Find and cache the instrument
        for (const auto& instrument : currentKit->instruments)
        {
            if (instrument->name == instrumentName)
            {
                targetInstrument = instrument.get();
                instrumentCache[instrumentName] = targetInstrument;
                break;
            }
        }
    }

    if (!targetInstrument || targetInstrument->samples.empty())
        return nullptr;

    // Normalize power values to find range
    float minPower = targetInstrument->samples[0].power;
    float maxPower = targetInstrument->samples[0].power;

    for (const auto& sample : targetInstrument->samples)
    {
        minPower = std::min(minPower, sample.power);
        maxPower = std::max(maxPower, sample.power);
    }

    // Clamp power values to reasonable range (0-1)
    // Some kits have incorrect power values outside this range
    minPower = juce::jlimit(0.0f, 1.0f, minPower);
    maxPower = juce::jlimit(0.0f, 1.0f, maxPower);

    // Handle edge case: single sample or all samples have same power
    if (std::abs(maxPower - minPower) < 0.001f)
    {
        // All samples have same power - use round robin if multiple samples
        // This happens with kits that don't have velocity layers
        if (targetInstrument->samples.size() == 1)
        {
            return &targetInstrument->samples[0];
        }

        const int noteIndex = midiNote % 128;  // Clamp to array size
        const int numSamples = static_cast<int>(targetInstrument->samples.size());
        const int lastIndex = lastSampleIndex[noteIndex].load(std::memory_order_relaxed);
        int selectedIndex = 0;

        if (roundRobinAmount < 0.01f)
        {
            // Stable mode: keep repeated notes on the same sample.
            selectedIndex = 0;
        }
        else if (roundRobinAmount > 0.99f)
        {
            // Full rotation mode: step through samples predictably.
            selectedIndex = (lastIndex >= 0 && lastIndex < numSamples)
                ? (lastIndex + 1) % numSamples
                : 0;
        }
        else
        {
            // Hybrid mode: randomize with anti-repeat weighting instead of
            // walking monotonically through the sample list.
            const int nextIndex = (lastIndex >= 0 && lastIndex < numSamples)
                ? (lastIndex + 1) % numSamples
                : 0;

            float totalWeight = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                float weight = 1.0f;

                if (i == lastIndex)
                {
                    const float penalty = 0.1f - (roundRobinAmount * 0.08f);
                    weight *= juce::jmax(0.01f, penalty);
                }
                else if (i == nextIndex)
                {
                    weight *= (1.0f + roundRobinAmount * 1.5f);
                }

                totalWeight += weight;
            }

            float randomValue = rrRng.nextFloat() * totalWeight;
            float cumulative = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                float weight = 1.0f;

                if (i == lastIndex)
                {
                    const float penalty = 0.1f - (roundRobinAmount * 0.08f);
                    weight *= juce::jmax(0.01f, penalty);
                }
                else if (i == nextIndex)
                {
                    weight *= (1.0f + roundRobinAmount * 1.5f);
                }

                cumulative += weight;
                if (randomValue <= cumulative)
                {
                    selectedIndex = i;
                    break;
                }
            }
        }

        lastSampleIndex[noteIndex].store(selectedIndex, std::memory_order_relaxed);
        return &targetInstrument->samples[selectedIndex];
    }

    // Normalize velocity to power range
    float normalizedVelocity = minPower + (velocity * (maxPower - minPower));

    // Find candidate samples based on velocity (power) - using pre-allocated array
    int numCandidates = 0;
    float tolerance = (maxPower - minPower) * 0.25f; // 25% of total range for more variety

    for (const auto& sample : targetInstrument->samples)
    {
        float diff = std::abs(normalizedVelocity - sample.power);
        if (diff < tolerance && numCandidates < maxCandidates)
        {
            candidatePool[numCandidates++] = &sample;
        }
    }

    // If no candidates in range, expand search
    if (numCandidates == 0)
    {
        // Find 3-5 closest samples to have round robin pool - using pre-allocated array
        int numSamplesWithDiff = 0;

        for (const auto& sample : targetInstrument->samples)
        {
            if (numSamplesWithDiff < maxSamplesPerInstrument)
            {
                float diff = std::abs(normalizedVelocity - sample.power);
                samplesWithDiffPool[numSamplesWithDiff++] = {&sample, diff};
            }
        }

        // Sort by proximity (simple insertion sort for small arrays)
        for (int i = 1; i < numSamplesWithDiff; ++i)
        {
            for (int j = i; j > 0 && samplesWithDiffPool[j].second < samplesWithDiffPool[j - 1].second; --j)
            {
                std::swap(samplesWithDiffPool[j], samplesWithDiffPool[j - 1]);
            }
        }

        // Take 4 closest (or all if less)
        numCandidates = std::min(4, numSamplesWithDiff);
        for (int i = 0; i < numCandidates; ++i)
        {
            candidatePool[i] = samplesWithDiffPool[i].first;
        }
    }

    // Ensure there's always at least one candidate
    if (numCandidates == 0)
    {
        return &targetInstrument->samples[0];
    }

    // ROUND ROBIN ANTI-MACHINE GUN
    // Avoids repeating same sample consecutively
    int noteIndex = midiNote % 128;  // Clamp to array size
    int lastIndex = lastSampleIndex[noteIndex].load(std::memory_order_relaxed);
    int selectedIndex = 0;

    if (numCandidates == 1)
    {
        // Only one candidate, use it
        selectedIndex = 0;
    }
    else if (roundRobinAmount < 0.01f)
    {
        // Pure velocity mode: always closest
        float minDiff = std::abs(normalizedVelocity - candidatePool[0]->power);
        for (int i = 1; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);
            if (diff < minDiff)
            {
                minDiff = diff;
                selectedIndex = i;
            }
        }
    }
    else if (roundRobinAmount > 0.99f)
    {
        // Pure rotation mode: next sample different from last
        // Sort by proximity to velocity - using pre-allocated array
        int numIndexedCandidates = 0;
        for (int i = 0; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);
            indexedCandidatesPool[numIndexedCandidates++] = {i, diff};
        }

        // Simple insertion sort
        for (int i = 1; i < numIndexedCandidates; ++i)
        {
            for (int j = i; j > 0 && indexedCandidatesPool[j].second < indexedCandidatesPool[j - 1].second; --j)
            {
                std::swap(indexedCandidatesPool[j], indexedCandidatesPool[j - 1]);
            }
        }

        // Rotate among best candidates, avoiding last used
        int poolSize = std::min(4, numIndexedCandidates);
        int nextIndex = (lastIndex + 1) % poolSize;
        selectedIndex = indexedCandidatesPool[nextIndex].first;
    }
    else
    {
        // Smart hybrid mode: weighted random with penalty to last used - using pre-allocated array
        int numWeightedCandidates = 0;
        float totalWeight = 0.0f;

        for (int i = 0; i < numCandidates; ++i)
        {
            float diff = std::abs(normalizedVelocity - candidatePool[i]->power);

            // Base weight: inversely proportional to velocity difference
            float weight = 1.0f / (1.0f + diff * 5.0f);

            // VERY STRONG PENALTY to last used sample (avoids machine gun)
            if (i == lastIndex)
            {
                // Penalty scaled by roundRobinAmount
                // With 0.7 default: 93% penalty
                float penalty = 0.1f - (roundRobinAmount * 0.08f);
                weight *= juce::jmax(0.01f, penalty);
            }
            // Bonus to next in rotation (stronger with high roundRobinAmount)
            else if (i == (lastIndex + 1) % numCandidates)
            {
                weight *= (1.0f + roundRobinAmount * 1.5f);
            }

            weightedCandidatesPool[numWeightedCandidates++] = {i, weight};
            totalWeight += weight;
        }

        // Weighted random selection (lock-free RNG, safe on audio thread)
        float randomValue = rrRng.nextFloat() * totalWeight;
        float cumulative = 0.0f;

        for (int i = 0; i < numWeightedCandidates; ++i)
        {
            cumulative += weightedCandidatesPool[i].second;
            if (randomValue <= cumulative)
            {
                selectedIndex = weightedCandidatesPool[i].first;
                break;
            }
        }
    }

    lastSampleIndex[noteIndex].store(selectedIndex, std::memory_order_relaxed);
    return candidatePool[selectedIndex];
}

juce::AudioBuffer<float>* SampleEngine::getAudioBuffer(const DrumSample* sample,
                                                       const juce::String& channelName)
{
    if (!sample)
        return nullptr;

    // Find audio file for this channel
    for (const auto& audioSample : sample->audioFiles)
    {
        if (audioSample.channelName == channelName)
        {
            juce::String cacheKey = audioSample.audioFile.getFullPathName() +
                                   "_ch" + juce::String(audioSample.fileChannel);

            // Lock-free access: check if ready and get pointer atomically
            auto it = lockFreeBufferCache.find(cacheKey);
            if (it != lockFreeBufferCache.end())
            {
                auto* entry = it->second.get();
                if (entry && entry->ready.load(std::memory_order_acquire))
                {
                    return entry->bufferPtr.load(std::memory_order_acquire);
                }
            }

            return nullptr;
        }
    }

    // Channel doesn't exist in this sample
    return nullptr;
}
