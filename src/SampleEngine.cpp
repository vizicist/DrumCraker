#include "SampleEngine.h"
#include <set>
#include <algorithm>

#if JUCE_LINUX
    #include <malloc.h>
#endif

SampleEngine::SampleEngine() {}

SampleEngine::~SampleEngine()
{
    // CRITICAL: Signal all threads to stop IMMEDIATELY
    shouldStopLoading = true;
    kitLoaded = false;
    
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
    sampleRate = sr;
}

void SampleEngine::reset()
{
    // Signal cancellation FIRST
    shouldStopLoading = true;
    kitLoaded = false;
    
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
    
    originalSampleRates.clear();
    std::unordered_map<juce::String, double>().swap(originalSampleRates);
    
    lastSampleIndex.clear();
    std::unordered_map<int, int>().swap(lastSampleIndex);
    
    instrumentCache.clear();
    std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);
    
    currentKit.reset();
    
    // Force memory return to OS
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
        
        // Reset flags for new load
        shouldStopLoading = false;
        kitLoaded = false;
    }
    
    // Mark as not loaded while we're loading
    kitLoaded = false;
    
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
        
        originalSampleRates.clear();
        std::unordered_map<juce::String, double>().swap(originalSampleRates);
        
        lastSampleIndex.clear();
        std::unordered_map<int, int>().swap(lastSampleIndex);
        
        instrumentCache.clear();
        std::unordered_map<juce::String, Instrument*>().swap(instrumentCache);
    }
    
    // Force memory return to OS
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

void SampleEngine::loadSamplesAsync()
{
    if (!currentKit)
        return;

    // Mark that we're loading
    isLoadingAsync = true;
    shouldStopLoading = false;

    // OPTIMIZED: Multi-threaded loading with thread pool (SAFE)
    juce::Thread::launch([this]()
    {
        if (!currentKit || shouldStopLoading.load())
        {
            isLoadingAsync = false;
            return;
        }
        
        // Collect all audio samples to load
        std::vector<AudioSample> samplesToLoad;
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
                    samplesToLoad.push_back(audioSample);
                }
            }
        }
        
        // Use thread pool for parallel loading (90% of available cores)
        const int numCpus = juce::SystemStats::getNumCpus();
        const int numThreads = juce::jmax(2, static_cast<int>(numCpus * 0.9f));
        juce::ThreadPool pool(numThreads);
        std::atomic<int> loadedCount{0};
        std::atomic<bool> hasError{false};
        
        for (const auto& audioSample : samplesToLoad)
        {
            if (shouldStopLoading.load())
                break;
            
            // CRITICAL: Capture by value to avoid use-after-free
            pool.addJob([this, audioSample, &loadedCount, &hasError]()
            {
                // Double-check we're still valid
                if (!shouldStopLoading.load() && !hasError.load())
                {
                    try
                    {
                        loadSampleFile(audioSample);
                        loadedCount++;
                    }
                    catch (...)
                    {
                        hasError = true;
                    }
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
        }
        
        isLoadingAsync = false;
        
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
    });
}

void SampleEngine::loadSamplesSync()
{
    if (!currentKit)
        return;

    // Load samples SYNCHRONOUSLY (for offline rendering)
    for (const auto& instrument : currentKit->instruments)
    {
        for (const auto& sample : instrument->samples)
        {
            for (const auto& audioSample : sample.audioFiles)
            {
                loadSampleFile(audioSample);
            }
        }
    }
    
    kitLoaded = true;
    
    if (loadingCallback)
        loadingCallback(true);
}

bool SampleEngine::loadSampleFile(const AudioSample& audioSample)
{
    juce::File audioFile = audioSample.audioFile;
    if (!audioFile.existsAsFile())
        return false;

    juce::String cacheKey = audioFile.getFullPathName() + "_ch" + juce::String(audioSample.fileChannel);
    
    // Check if already in cache (fast check without lock)
    {
        juce::ScopedLock lock(cacheLock);
        if (audioBufferCache.find(cacheKey) != audioBufferCache.end())
            return true;
    }

    // Load audio file
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(audioFile));

    if (reader == nullptr)
        return false;

    double originalSampleRate = reader->sampleRate;
    int numSamples = static_cast<int>(reader->lengthInSamples);
    int numChannels = reader->numChannels;

    if (audioSample.fileChannel > numChannels)
        return false;

    // Optimized: Read only the channel we need if possible
    auto buffer = std::make_unique<juce::AudioBuffer<float>>(1, numSamples);
    
    if (numChannels == 1)
    {
        // Mono file - read directly
        if (!reader->read(buffer.get(), 0, numSamples, 0, true, false))
            return false;
    }
    else
    {
        // Multi-channel file - read all channels then extract
        juce::AudioBuffer<float> tempBuffer(numChannels, numSamples);
        if (!reader->read(&tempBuffer, 0, numSamples, 0, true, true))
            return false;
        
        buffer->copyFrom(0, 0, tempBuffer, audioSample.fileChannel - 1, 0, numSamples);
    }

    // Resample if necessary (only if sample rates don't match)
    // Use the current project sample rate (may be 0 if not initialized yet)
    if (sampleRate > 0.0 && std::abs(originalSampleRate - sampleRate) > 0.1)
    {
        resampleBuffer(*buffer, originalSampleRate, sampleRate);
    }

    // Store in cache
    juce::ScopedLock lock(cacheLock);
    audioBufferCache[cacheKey] = std::move(buffer);
    originalSampleRates[cacheKey] = originalSampleRate;
    
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
        
        // Multiple samples with same power - use round robin
        int lastIndex = lastSampleIndex[midiNote];
        int nextIndex = (lastIndex + 1) % static_cast<int>(targetInstrument->samples.size());
        lastSampleIndex[midiNote] = nextIndex;
        return &targetInstrument->samples[nextIndex];
    }
    
    // Normalize velocity to power range
    float normalizedVelocity = minPower + (velocity * (maxPower - minPower));
    
    // Find candidate samples based on velocity (power)
    std::vector<const DrumSample*> candidates;
    float tolerance = (maxPower - minPower) * 0.25f; // 25% of total range for more variety

    for (const auto& sample : targetInstrument->samples)
    {
        float diff = std::abs(normalizedVelocity - sample.power);
        if (diff < tolerance)
            candidates.push_back(&sample);
    }

    // If no candidates in range, expand search
    if (candidates.empty())
    {
        // Find 3-5 closest samples to have round robin pool
        std::vector<std::pair<const DrumSample*, float>> samplesWithDiff;
        
        for (const auto& sample : targetInstrument->samples)
        {
            float diff = std::abs(normalizedVelocity - sample.power);
            samplesWithDiff.push_back({&sample, diff});
        }
        
        // Sort by proximity
        std::sort(samplesWithDiff.begin(), samplesWithDiff.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Take 4 closest (or all if less)
        int numCandidates = std::min(4, static_cast<int>(samplesWithDiff.size()));
        for (int i = 0; i < numCandidates; ++i)
        {
            candidates.push_back(samplesWithDiff[i].first);
        }
    }
    
    // Ensure there's always at least one candidate
    if (candidates.empty())
    {
        return &targetInstrument->samples[0];
    }

    // ROUND ROBIN ANTI-MACHINE GUN
    // Avoids repeating same sample consecutively
    int lastIndex = lastSampleIndex[midiNote];
    int selectedIndex = 0;

    if (candidates.size() == 1)
    {
        // Only one candidate, use it
        selectedIndex = 0;
    }
    else if (roundRobinAmount < 0.01f)
    {
        // Pure velocity mode: always closest
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
        // Pure rotation mode: next sample different from last
        // Sort by proximity to velocity
        std::vector<std::pair<int, float>> indexedCandidates;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            float diff = std::abs(normalizedVelocity - candidates[i]->power);
            indexedCandidates.push_back({static_cast<int>(i), diff});
        }
        
        std::sort(indexedCandidates.begin(), indexedCandidates.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Rotate among best candidates, avoiding last used
        int poolSize = std::min(4, static_cast<int>(indexedCandidates.size()));
        int nextIndex = (lastIndex + 1) % poolSize;
        selectedIndex = indexedCandidates[nextIndex].first;
    }
    else
    {
        // Smart hybrid mode: weighted random with penalty to last used
        std::vector<std::pair<int, float>> weightedCandidates;
        float totalWeight = 0.0f;
        
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            float diff = std::abs(normalizedVelocity - candidates[i]->power);
            
            // Base weight: inversely proportional to velocity difference
            float weight = 1.0f / (1.0f + diff * 5.0f);
            
            // VERY STRONG PENALTY to last used sample (avoids machine gun)
            if (static_cast<int>(i) == lastIndex)
            {
                // Penalty scaled by roundRobinAmount
                // With 0.7 default: 93% penalty
                float penalty = 0.1f - (roundRobinAmount * 0.08f);
                weight *= juce::jmax(0.01f, penalty);
            }
            // Bonus to next in rotation (stronger with high roundRobinAmount)
            else if (static_cast<int>(i) == (lastIndex + 1) % static_cast<int>(candidates.size()))
            {
                weight *= (1.0f + roundRobinAmount * 1.5f);
            }
            
            weightedCandidates.push_back({static_cast<int>(i), weight});
            totalWeight += weight;
        }
        
        // Weighted random selection
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

    // Find audio file for this channel
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

    // Channel doesn't exist in this sample
    return nullptr;
}
