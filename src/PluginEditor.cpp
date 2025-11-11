#include "PluginEditor.h"

DrumSamplerEditor::DrumSamplerEditor(DrumSamplerProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    
    // Load background image from VST3 bundle resources
    juce::File backgroundFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("background.png");
    
    if (!backgroundFile.existsAsFile())
    {
        // Fallback: try from working directory
        backgroundFile = juce::File::getCurrentWorkingDirectory()
            .getChildFile("assets").getChildFile("background.png");
    }
    
    if (backgroundFile.existsAsFile())
        backgroundImage = juce::ImageFileFormat::loadFrom(backgroundFile);
    
    // Window size equal to background
    setSize(1024, 939);
    
    // Timer to update UI
    startTimer(100);

    // Version label
    versionLabel.setText("v" PLUGIN_VERSION, juce::dontSendNotification);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    versionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFD700)); // Gold
    addAndMakeVisible(versionLabel);

    // Load buttons with modern style and more opaque shadow
    auto setupButton = [](juce::TextButton& button, const juce::String& text) {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xD0151515));  // More opaque
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xE0252525));
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFFFD700));
        button.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xFF808080));
    };
    
    setupButton(loadKitButton, "LOAD DRUMKIT");
    loadKitButton.onClick = [this] { loadKitButtonClicked(); };
    addAndMakeVisible(loadKitButton);

    setupButton(loadMidiMapButton, "LOAD MIDI MAP");
    loadMidiMapButton.onClick = [this] { loadMidiMapButtonClicked(); };
    addAndMakeVisible(loadMidiMapButton);

    // Status labels with more opaque shadow for better visibility
    auto setupStatusLabel = [](juce::Label& label, const juce::String& text, juce::Colour textColour) {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(17.0f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, textColour);
        label.setColour(juce::Label::backgroundColourId, juce::Colour(0xC0000000));  // More opaque
        label.setJustificationType(juce::Justification::centred);
    };
    
    setupStatusLabel(kitNameLabel, "No kit loaded", juce::Colours::white);
    addAndMakeVisible(kitNameLabel);

    setupStatusLabel(midiMapLabel, "No MIDI map loaded", juce::Colours::white);
    addAndMakeVisible(midiMapLabel);

    setupStatusLabel(statusLabel, "Ready", juce::Colour(0xFF00FF00));
    statusLabel.setFont(juce::Font(16.0f, juce::Font::italic));
    addAndMakeVisible(statusLabel);

    // Sliders with modern style and more opaque shadow
    auto setupSlider = [](juce::Slider& slider, juce::Label& label, const juce::String& labelText) {
        label.setText(labelText, juce::dontSendNotification);
        label.setFont(juce::Font(18.0f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, juce::Colours::white);
        label.setColour(juce::Label::backgroundColourId, juce::Colour(0xB0000000));  // More opaque
        label.setJustificationType(juce::Justification::centred);
        
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 24);
        slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFFFD700));
        slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF707070));
        slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFD700));
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xC0000000));  // More opaque
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xFF505050));
    };

    // Master Volume
    setupSlider(masterVolumeSlider, masterVolumeLabel, "MASTER");
    masterVolumeSlider.setRange(-60.0, 12.0, 0.1);
    masterVolumeSlider.setValue(processor.masterVolume->get());
    masterVolumeSlider.setTextValueSuffix(" dB");
    masterVolumeSlider.onValueChange = [this]() {
        *processor.masterVolume = static_cast<float>(masterVolumeSlider.getValue());
    };
    addAndMakeVisible(masterVolumeSlider);
    addAndMakeVisible(masterVolumeLabel);

    // Velocity Randomness
    setupSlider(velocityRandomnessSlider, velocityRandomnessLabel, "VELOCITY");
    velocityRandomnessSlider.setRange(0.0, 0.3, 0.01);
    velocityRandomnessSlider.setValue(processor.velocityRandomness->get());
    velocityRandomnessSlider.onValueChange = [this]() {
        *processor.velocityRandomness = static_cast<float>(velocityRandomnessSlider.getValue());
    };
    addAndMakeVisible(velocityRandomnessSlider);
    addAndMakeVisible(velocityRandomnessLabel);

    // Timing Randomness
    setupSlider(timingRandomnessSlider, timingRandomnessLabel, "TIMING");
    timingRandomnessSlider.setRange(0.0, 20.0, 0.1);
    timingRandomnessSlider.setValue(processor.timingRandomness->get());
    timingRandomnessSlider.setTextValueSuffix(" ms");
    timingRandomnessSlider.onValueChange = [this]() {
        *processor.timingRandomness = static_cast<float>(timingRandomnessSlider.getValue());
    };
    addAndMakeVisible(timingRandomnessSlider);
    addAndMakeVisible(timingRandomnessLabel);

    // Round Robin
    setupSlider(roundRobinSlider, roundRobinLabel, "ROUND ROBIN");
    roundRobinSlider.setRange(0.0, 1.0, 0.01);
    roundRobinSlider.setValue(processor.roundRobinVariation->get());
    roundRobinSlider.onValueChange = [this]() {
        *processor.roundRobinVariation = static_cast<float>(roundRobinSlider.getValue());
    };
    addAndMakeVisible(roundRobinSlider);
    addAndMakeVisible(roundRobinLabel);
    
    // Update UI with current state
    updateUIFromProcessor();
}

void DrumSamplerEditor::timerCallback()
{
    // Update sliders if parameters changed
    if (std::abs(masterVolumeSlider.getValue() - processor.masterVolume->get()) > 0.01)
        masterVolumeSlider.setValue(processor.masterVolume->get(), juce::dontSendNotification);
    
    if (std::abs(velocityRandomnessSlider.getValue() - processor.velocityRandomness->get()) > 0.001)
        velocityRandomnessSlider.setValue(processor.velocityRandomness->get(), juce::dontSendNotification);
    
    if (std::abs(timingRandomnessSlider.getValue() - processor.timingRandomness->get()) > 0.01)
        timingRandomnessSlider.setValue(processor.timingRandomness->get(), juce::dontSendNotification);
    
    if (std::abs(roundRobinSlider.getValue() - processor.roundRobinVariation->get()) > 0.001)
        roundRobinSlider.setValue(processor.roundRobinVariation->get(), juce::dontSendNotification);
    
    // Update status according to loading state
    bool loadingKit = processor.getIsLoadingKit();
    bool loadingMidiMap = processor.getIsLoadingMidiMap();
    
    if (loadingKit || loadingMidiMap)
    {
        juce::String loadingText = "Loading";
        if (loadingKit)
            loadingText += " drumkit";
        if (loadingMidiMap)
            loadingText += (loadingKit ? " and" : "") + juce::String(" MIDI map");
        loadingText += "...";
        
        if (statusLabel.getText() != loadingText)
        {
            statusLabel.setText(loadingText, juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFAA00)); // Orange
        }
    }
    else if (processor.isKitFullyLoaded() && statusLabel.getText() != "Ready")
    {
        statusLabel.setText("Ready", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF00FF00)); // Green
    }
}

void DrumSamplerEditor::updateUIFromProcessor()
{
    // Update labels with loaded kit information
    if (!processor.getCurrentKitName().isEmpty())
    {
        kitNameLabel.setText("Kit: " + processor.getCurrentKitName(), juce::dontSendNotification);
        statusLabel.setText("Ready", juce::dontSendNotification);
    }
    
    // Update MIDI map label
    if (!processor.getCurrentMidiMapName().isEmpty())
    {
        midiMapLabel.setText("MIDI Map: " + processor.getCurrentMidiMapName(), juce::dontSendNotification);
    }
}

DrumSamplerEditor::~DrumSamplerEditor() {}

void DrumSamplerEditor::paint(juce::Graphics& g)
{
    // Draw background image
    if (backgroundImage.isValid())
    {
        g.drawImage(backgroundImage, getLocalBounds().toFloat());
    }
    else
    {
        // Fallback: dark gradient
        g.fillAll(juce::Colour(0xFF1A1A1A));
        juce::ColourGradient gradient(juce::Colour(0xFF2A2A2A), 0, 0,
                                     juce::Colour(0xFF0A0A0A), 0, static_cast<float>(getHeight()), false);
        g.setGradientFill(gradient);
        g.fillAll();
    }
}

void DrumSamplerEditor::resized()
{
    auto bounds = getLocalBounds();
    
    // Version label in top right corner
    versionLabel.setBounds(bounds.getWidth() - 100, 95, 80, 25);
    
    // Load buttons HIGHER UP - above "drumcraker" text in background
    int buttonWidth = 280;
    int buttonHeight = 45;
    int buttonSpacing = 60;
    int buttonY = 80;  // 50px higher
    int totalButtonWidth = (buttonWidth * 2) + buttonSpacing;
    int buttonStartX = (bounds.getWidth() - totalButtonWidth) / 2;
    
    loadKitButton.setBounds(buttonStartX, buttonY, buttonWidth, buttonHeight);
    loadMidiMapButton.setBounds(buttonStartX + buttonWidth + buttonSpacing, buttonY, buttonWidth, buttonHeight);
    
    // Knobs LOWER DOWN - near amplifiers in background
    int knobSize = 140;
    int knobSpacing = 50;
    int knobY = 540;  // Lower, near amps
    int totalKnobWidth = (knobSize * 4) + (knobSpacing * 3);
    int knobStartX = (bounds.getWidth() - totalKnobWidth) / 2;
    
    // Status labels EVEN LOWER - near footer
    int labelY = 750;  // Closer to footer
    int labelWidth = bounds.getWidth() - 200;
    int labelX = (bounds.getWidth() - labelWidth) / 2;
    
    kitNameLabel.setBounds(labelX, labelY, labelWidth, 28);
    midiMapLabel.setBounds(labelX, labelY + 35, labelWidth, 28);
    statusLabel.setBounds(labelX, labelY + 70, labelWidth, 25);
    
    // Master Volume
    masterVolumeLabel.setBounds(knobStartX, knobY, knobSize, 25);
    masterVolumeSlider.setBounds(knobStartX, knobY + 25, knobSize, knobSize);
    
    // Velocity
    int velocityX = knobStartX + knobSize + knobSpacing;
    velocityRandomnessLabel.setBounds(velocityX, knobY, knobSize, 25);
    velocityRandomnessSlider.setBounds(velocityX, knobY + 25, knobSize, knobSize);
    
    // Timing
    int timingX = velocityX + knobSize + knobSpacing;
    timingRandomnessLabel.setBounds(timingX, knobY, knobSize, 25);
    timingRandomnessSlider.setBounds(timingX, knobY + 25, knobSize, knobSize);
    
    // Round Robin
    int roundRobinX = timingX + knobSize + knobSpacing;
    roundRobinLabel.setBounds(roundRobinX, knobY, knobSize, 25);
    roundRobinSlider.setBounds(roundRobinX, knobY + 25, knobSize, knobSize);
}

void DrumSamplerEditor::loadKitButtonClicked()
{
    auto chooser = std::make_shared<juce::FileChooser>("Select DrumKit XML file",
                                                        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                                                        "*.xml");

    auto fileFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync(fileFlags, [this, chooser](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            statusLabel.setText("Loading kit...", juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFAA00)); // Orange
            
            if (processor.loadDrumKit(file))
            {
                kitNameLabel.setText("Kit: " + processor.getCurrentKitName(), juce::dontSendNotification);
                statusLabel.setText("Loading samples...", juce::dontSendNotification);
                
                // Callback will be called when async loading finishes
            }
            else
            {
                statusLabel.setText("Error loading kit", juce::dontSendNotification);
                statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFF0000)); // Red
            }
        }
    });
}

void DrumSamplerEditor::loadMidiMapButtonClicked()
{
    auto chooser = std::make_shared<juce::FileChooser>("Select MIDI Map XML file",
                                                        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                                                        "*.xml");

    auto fileFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync(fileFlags, [this, chooser](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            statusLabel.setText("Loading MIDI map...", juce::dontSendNotification);
            
            if (processor.loadMidiMap(file))
            {
                midiMapLabel.setText("MIDI Map: " + processor.getCurrentMidiMapName(), juce::dontSendNotification);
                statusLabel.setText("MIDI map loaded successfully", juce::dontSendNotification);
            }
            else
            {
                statusLabel.setText("Error loading MIDI map", juce::dontSendNotification);
            }
        }
    });
}
