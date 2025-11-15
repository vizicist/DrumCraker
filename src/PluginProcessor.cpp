#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <set>

DrumSamplerProcessor::DrumSamplerProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Out 1", juce::AudioChannelSet::stereo(), true)
        .withOutput("Out 2", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 3", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 4", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 5", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 6", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 7", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 8", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 9", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 10", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 11", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 12", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 13", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 14", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 15", juce::AudioChannelSet::stereo(), false)
        .withOutput("Out 16", juce::AudioChannelSet::stereo(), false))
{
    drumKitLoader = std::make_unique<DrumKitLoader>();
    sampleEngine = std::make_unique<SampleEngine>();
    voiceManager = std::make_unique<VoiceManager>();

    // Parameters
    addParameter(masterVolume = new juce::AudioParameterFloat(
        "masterVol", "Master Volume", -60.0f, 12.0f, 0.0f));
    
    // Velocity: 0-100% variation (default 8% for subtle humanization)
    addParameter(velocityRandomness = new juce::AudioParameterFloat(
        "velocityRnd", "Velocity Humanization", 0.0f, 1.0f, 0.08f));
    
    // Timing: 0-20ms variation (default 5ms for natural groove)
    addParameter(timingRandomness = new juce::AudioParameterFloat(
        "timingRnd", "Timing Humanization (ms)", 0.0f, 20.0f, 5.0f));
    
    // Round Robin: 0=pure velocity, 1=pure rotation (default 0.7 for natural mix)
    addParameter(roundRobinVariation = new juce::AudioParameterFloat(
        "roundRobin", "Round Robin Mix", 0.0f, 1.0f, 0.7f));
}

DrumSamplerProcessor::~DrumSamplerProcessor() {}

bool DrumSamplerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // All output buses must be stereo or disabled
    for (int i = 0; i < layouts.outputBuses.size(); ++i)
    {
        auto& layout = layouts.outputBuses.getReference(i);
        if (layout != juce::AudioChannelSet::stereo() && 
            layout != juce::AudioChannelSet::disabled())
            return false;
    }
    
    return true;
}

bool DrumSamplerProcessor::canAddBus(bool) const
{
    // Don't allow adding/removing buses - they're fixed in constructor
    return false;
}

bool DrumSamplerProcessor::canRemoveBus(bool) const
{
    // Don't allow adding/removing buses - they're fixed in constructor
    return false;
}

void DrumSamplerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    sampleEngine->prepare(sampleRate, samplesPerBlock);
    voiceManager->prepare(sampleRate, samplesPerBlock);
    
    // NOTE: Sample rate is stored and used for resampling when kits are loaded
    // Kit loading happens in setStateInformation or loadDrumKit, which both
    // call prepare() again to ensure correct sample rate for resampling
}

void DrumSamplerProcessor::releaseResources()
{
    // DON'T reset sampleEngine here
    // Keep samples in memory for fast exports
    // Only stop active voices
    voiceManager->reset();
}

void DrumSamplerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // If kit is not loaded, don't process
    if (!sampleEngine->isLoaded())
        return;

    // Verify valid buffer size
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return;
    
    // Pre-cache parameter values (avoid repeated atomic reads)
    const float velocityHumanization = velocityRandomness->get();
    const float timingHumanization = timingRandomness->get();
    const float rrVariation = roundRobinVariation->get();
    const bool hasVelocityHumanization = velocityHumanization > 0.001f;
    const bool hasTimingHumanization = timingHumanization > 0.001f;
    const int bufferSize = buffer.getNumSamples();
    
    // Process MIDI events with humanization (ultra-optimized)
    auto& rng = juce::Random::getSystemRandom();
    
    for (const auto metadata : midiMessages)
    {
        auto message = metadata.getMessage();
        
        if (message.isNoteOn()) [[likely]]
        {
            float velocity = message.getVelocity() / 127.0f;
            int sampleOffset = metadata.samplePosition;
            
            // VELOCITY HUMANIZATION (optimized Box-Muller)
            if (hasVelocityHumanization)
            {
                float u1 = rng.nextFloat() * 0.9999f + 0.0001f;
                float u2 = rng.nextFloat();
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::cos(juce::MathConstants<float>::twoPi * u2);
                // Apply variation: gaussian has σ=1, so multiply by parameter directly
                // 68% of values will be within ±velocityHumanization
                velocity = juce::jlimit(0.05f, 1.0f, velocity + gaussian * velocityHumanization);
            }
            
            // TIMING HUMANIZATION (optimized)
            if (hasTimingHumanization)
            {
                float u1 = rng.nextFloat() * 0.9999f + 0.0001f;
                float u2 = rng.nextFloat();
                float gaussian = std::sqrt(-2.0f * std::log(u1)) * std::sin(juce::MathConstants<float>::twoPi * u2);
                float velocityBias = (velocity - 0.5f) * 0.4f;
                int sampleDelay = static_cast<int>(((gaussian * 0.5f + velocityBias) * timingHumanization / 1000.0f) * currentSampleRate);
                sampleOffset = juce::jlimit(0, bufferSize - 1, sampleOffset + sampleDelay);
            }
            
            voiceManager->noteOn(message.getNoteNumber(), velocity, sampleEngine.get(), sampleOffset, rrVariation);
        }
    }
    
    // Count enabled buses
    int numEnabledBuses = 0;
    for (int i = 0; i < getTotalNumOutputChannels() / 2; ++i)
    {
        auto* bus = getBus(false, i);
        if (bus && bus->isEnabled())
            numEnabledBuses++;
    }
    
    float gainLinear = juce::Decibels::decibelsToGain(masterVolume->get());
    
    // MULTI-CHANNEL MODE: Render all voices once, routing to appropriate buses
    if (numEnabledBuses > 1 && !instrumentToBusMap.empty())
    {
        voiceManager->renderNextBlockMultiBus(buffer, 0, buffer.getNumSamples(),
                                             gainLinear, this);
    }
    else
    {
        // STEREO MODE: Render all voices to main output
        auto* mainBus = getBus(false, 0);
        if (mainBus && mainBus->isEnabled())
        {
            auto mainBuffer = getBusBuffer(buffer, false, 0);
            voiceManager->renderNextBlock(mainBuffer, 0, buffer.getNumSamples());
            mainBuffer.applyGain(gainLinear);
        }
    }
}



void DrumSamplerProcessor::setupInstrumentRouting()
{
    const DrumKit* kit = sampleEngine->getCurrentKit();
    if (!kit)
        return;
    
    instrumentToBusMap.clear();
    instrumentGroups.clear();
    
    int busIndex = 0;
    std::set<juce::String> assignedInstruments;
    
    // Bus 0: All Kicks together
    bool kickBusAssigned = false;
    for (const auto& instrument : kit->instruments)
    {
        if (instrument->name.containsIgnoreCase("KDrum") || 
            instrument->name.containsIgnoreCase("Kick"))
        {
            instrumentToBusMap[instrument->name] = 0;
            assignedInstruments.insert(instrument->name);
            kickBusAssigned = true;
        }
    }
    if (kickBusAssigned)
    {
        instrumentGroups.push_back("Kick");
        busIndex = 1;
    }
    
    // Bus 1: All Snares together
    bool snareBusAssigned = false;
    for (const auto& instrument : kit->instruments)
    {
        if (assignedInstruments.count(instrument->name) > 0)
            continue;
            
        if (instrument->name.containsIgnoreCase("Snare"))
        {
            instrumentToBusMap[instrument->name] = busIndex;
            assignedInstruments.insert(instrument->name);
            snareBusAssigned = true;
        }
    }
    if (snareBusAssigned)
    {
        instrumentGroups.push_back("Snare");
        busIndex++;
    }
    
    // Bus 2: All HiHats together
    bool hihatBusAssigned = false;
    for (const auto& instrument : kit->instruments)
    {
        if (assignedInstruments.count(instrument->name) > 0)
            continue;
            
        if (instrument->name.containsIgnoreCase("Hihat") || 
            instrument->name.containsIgnoreCase("HH"))
        {
            instrumentToBusMap[instrument->name] = busIndex;
            assignedInstruments.insert(instrument->name);
            hihatBusAssigned = true;
        }
    }
    if (hihatBusAssigned)
    {
        instrumentGroups.push_back("HiHat");
        busIndex++;
    }
    
    // Toms: Each tom gets its own bus
    std::vector<juce::String> tomPatterns = {"Tom1", "Tom2", "Tom3", "FTom1", "FTom2", "FTom3"};
    for (const auto& tomPattern : tomPatterns)
    {
        for (const auto& instrument : kit->instruments)
        {
            if (assignedInstruments.count(instrument->name) > 0)
                continue;
            
            if (instrument->name.containsIgnoreCase(tomPattern))
            {
                if (busIndex < 16)
                {
                    instrumentToBusMap[instrument->name] = busIndex;
                    instrumentGroups.push_back(instrument->name);
                    assignedInstruments.insert(instrument->name);
                    busIndex++;
                }
            }
        }
    }
    
    // Cymbals: Each cymbal gets its own bus (Crash, Ride, China, Splash, etc.)
    std::vector<juce::String> cymbalPatterns = {"Ride", "Crash", "China", "Splash", "Bell"};
    for (const auto& cymbalPattern : cymbalPatterns)
    {
        for (const auto& instrument : kit->instruments)
        {
            if (assignedInstruments.count(instrument->name) > 0)
                continue;
            
            if (instrument->name.containsIgnoreCase(cymbalPattern))
            {
                if (busIndex < 16)
                {
                    instrumentToBusMap[instrument->name] = busIndex;
                    instrumentGroups.push_back(instrument->name);
                    assignedInstruments.insert(instrument->name);
                    busIndex++;
                }
            }
        }
    }
    
    // Remaining instruments: each gets its own bus
    for (const auto& instrument : kit->instruments)
    {
        if (assignedInstruments.count(instrument->name) == 0)
        {
            if (busIndex < 16)
            {
                instrumentToBusMap[instrument->name] = busIndex;
                instrumentGroups.push_back(instrument->name);
                assignedInstruments.insert(instrument->name);
                busIndex++;
            }
            else
            {
                // Max buses reached, assign to last bus
                instrumentToBusMap[instrument->name] = 15;
            }
        }
    }
    
    numOutputChannels = busIndex;
    
    // Pass mapping to voice manager for pre-caching bus indices
    voiceManager->setInstrumentToBusMap(instrumentToBusMap);
}

bool DrumSamplerProcessor::loadDrumKit(const juce::File& kitFile, bool async)
{
    isLoadingKit = true;
    
    // Stop all active voices before unloading previous kit
    voiceManager->reset();
    
    // CRITICAL: Clear old instrument routing IMMEDIATELY
    // to prevent using stale mappings during async load
    instrumentToBusMap.clear();
    instrumentGroups.clear();
    voiceManager->setInstrumentToBusMap(instrumentToBusMap);
    
    // CRITICAL: Ensure sampleEngine has correct project sample rate
    // before loading kit (for proper resampling)
    if (currentSampleRate > 0.0)
    {
        sampleEngine->prepare(currentSampleRate, 0);
    }
    
    auto kit = drumKitLoader->loadKit(kitFile);
    if (kit == nullptr)
    {
        isLoadingKit = false;
        return false;
    }

    currentKitName = kit->name;
    currentKitPath = kitFile.getFullPathName();
    
    // Configure callback
    sampleEngine->loadingCallback = [this](bool success)
    {
        if (success)
        {
            // Setup instrument routing after kit is loaded
            setupInstrumentRouting();
        }
        isLoadingKit = false;
    };
    
    // Load new kit (this frees the previous one automatically)
    sampleEngine->loadKit(std::move(kit), async);
    
    // If synchronous, update state immediately
    if (!async)
    {
        setupInstrumentRouting();
        isLoadingKit = false;
    }
    
    return true;
}

bool DrumSamplerProcessor::loadMidiMap(const juce::File& midiMapFile)
{
    isLoadingMidiMap = true;
    
    bool success = sampleEngine->loadMidiMap(midiMapFile);
    if (success)
    {
        currentMidiMapName = midiMapFile.getFileNameWithoutExtension();
        currentMidiMapPath = midiMapFile.getFullPathName();  // Save full path
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
    // Create XML to save state
    auto xml = std::make_unique<juce::XmlElement>("DrumCrakerState");
    
    // Save parameters
    xml->setAttribute("masterVolume", static_cast<double>(masterVolume->get()));
    xml->setAttribute("velocityRandomness", static_cast<double>(velocityRandomness->get()));
    xml->setAttribute("timingRandomness", static_cast<double>(timingRandomness->get()));
    xml->setAttribute("roundRobinVariation", static_cast<double>(roundRobinVariation->get()));
    
    // Save full paths to XML files
    xml->setAttribute("kitPath", currentKitPath);
    xml->setAttribute("midiMapPath", currentMidiMapPath);
    
    // Save names (for UI reference)
    xml->setAttribute("kitName", currentKitName);
    xml->setAttribute("midiMapName", currentMidiMapName);
    
    // Generate unique state ID (timestamp + random)
    stateId = juce::String(juce::Time::currentTimeMillis()) + "_" + 
              juce::String(juce::Random::getSystemRandom().nextInt());
    xml->setAttribute("stateId", stateId);
    
    // Convert XML to binary
    copyXmlToBinary(*xml, destData);
}

void DrumSamplerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    
    if (xml != nullptr && xml->hasTagName("DrumCrakerState"))
    {
        // Restore parameters immediately
        *masterVolume = static_cast<float>(xml->getDoubleAttribute("masterVolume", 0.0));
        *velocityRandomness = static_cast<float>(xml->getDoubleAttribute("velocityRandomness", 0.0));
        *timingRandomness = static_cast<float>(xml->getDoubleAttribute("timingRandomness", 0.0));
        *roundRobinVariation = static_cast<float>(xml->getDoubleAttribute("roundRobinVariation", 0.5));
        
        // Restore file paths
        juce::String savedKitPath = xml->getStringAttribute("kitPath");
        juce::String savedMidiMapPath = xml->getStringAttribute("midiMapPath");
        juce::String savedKitName = xml->getStringAttribute("kitName");
        juce::String savedMidiMapName = xml->getStringAttribute("midiMapName");
        juce::String savedStateId = xml->getStringAttribute("stateId");
        
        // Check if this is a different state (different project/session)
        bool isDifferentState = (savedStateId != stateId) && savedStateId.isNotEmpty();
        
        // Always reload when restoring state OR when state ID is different
        // This ensures correct kit is loaded when switching projects
        if (savedKitPath.isNotEmpty() && (isDifferentState || stateId.isEmpty()))
        {
            juce::File kitFile(savedKitPath);
            
            if (kitFile.existsAsFile())
            {
                // Update variables including state ID
                currentKitPath = savedKitPath;
                currentKitName = savedKitName;
                currentMidiMapPath = savedMidiMapPath;
                currentMidiMapName = savedMidiMapName;
                stateId = savedStateId;
                
                // CRITICAL: Clear old instrument routing IMMEDIATELY
                instrumentToBusMap.clear();
                instrumentGroups.clear();
                voiceManager->setInstrumentToBusMap(instrumentToBusMap);
                
                // Load ASYNCHRONOUSLY to not block Reaper
                isLoadingKit = true;
                
                // Capture variables for lambda
                juce::String midiMapPath = currentMidiMapPath;
                
                // Configure callback before loading
                sampleEngine->loadingCallback = [this, midiMapPath](bool success)
                {
                    if (success)
                    {
                        // Setup instrument routing after kit is loaded
                        setupInstrumentRouting();
                        
                        if (midiMapPath.isNotEmpty())
                        {
                            juce::File midiMapFile(midiMapPath);
                            if (midiMapFile.existsAsFile())
                            {
                                isLoadingMidiMap = true;
                                sampleEngine->loadMidiMap(midiMapFile);
                                isLoadingMidiMap = false;
                            }
                        }
                    }
                    
                    isLoadingKit = false;
                };
                
                voiceManager->reset();
                
                // CRITICAL: Ensure sampleEngine has correct project sample rate
                // before loading kit (for proper resampling)
                if (currentSampleRate > 0.0)
                {
                    sampleEngine->prepare(currentSampleRate, 0);
                }
                
                auto kit = drumKitLoader->loadKit(kitFile);
                if (kit != nullptr)
                {
                    sampleEngine->loadKit(std::move(kit), true); // ASYNC
                }
                else
                {
                    isLoadingKit = false;
                }
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumSamplerProcessor();
}
