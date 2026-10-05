#include "PluginEditor.h"

SVERBAudioProcessorEditor::SVERBAudioProcessorEditor (SVERBAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setLookAndFeel (&lnf);
    setOpaque (true);
    auto label = [this] (juce::Label& l,
                         const juce::String& text,
                         float size,
                         juce::Colour colour = juce::Colours::white)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (size));
        l.setColour (juce::Label::textColourId, colour);
        l.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    };
    label (titleLabel, "SVERB", 30);
    titleLabel.setFont (
        juce::Font (juce::FontOptions (30, juce::Font::bold)).withExtraKerningFactor (0.3f));
    label (subtitleLabel, "Speed & Reverb Audio Effect", 13, juce::Colour (0xff9ca3af));
    label (statusLabel, "Status: Ready to load audio.", 13, juce::Colour (0xfffacc15));
    label (hz432Label, "432Hz", 11);
    label (perfectionLabel, "Perfection: OFF", 11);
    label (speedTitle, "SPEED", 18);
    label (speedMinLabel, "0.5x (SLOW)", 11, juce::Colour (0xfff87171));
    label (speedMaxLabel, "2.0x (FAST)", 11, juce::Colour (0xff4ade80));
    speedMinLabel.setJustificationType (juce::Justification::centredLeft);
    speedMaxLabel.setJustificationType (juce::Justification::centredRight);
    label (reverbOnTitle, "REVERB ON", 13);
    label (wetTitle, "WETNESS", 13);
    label (gainTitle, "GAIN", 13);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&loadButton,
                                                                      &loopButton,
                                                                      &playButton,
                                                                      &stopButton,
                                                                      &resetButton,
                                                                      &perfectionButton,
                                                                      &exportButton,
                                                                      &hz432Button,
                                                                      &reverbToggle})
        addAndMakeVisible (c);
    resetButton.setComponentID ("reset");
    resetButton.setTitle ("Reset to defaults");
    resetButton.setTooltip ("Restore default effects and turn looping off");
    resetButton.onClick = [this]
    {
        audioProcessor.resetToDefaults ();
        setStatus ("Default settings restored.");
        refreshControls ();
    };
    loadButton.setComponentID ("load");
    perfectionButton.setComponentID ("perfection");
    hz432Button.setComponentID ("hz432");
    reverbToggle.setComponentID ("reverbToggle");
    hz432Button.setTitle ("432Hz Mode");
    reverbToggle.setTitle ("Reverb On");
    perfectionButton.setTitle ("Perfection preset");
    hz432Button.setTooltip ("432 / 440 varispeed detune");
    perfectionButton.setTooltip ("Cycle Off, Slow, Fast");
    loadButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffffcc00));
    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffcc00));
    loopButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff111111));
    playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff16a34a));
    stopButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffdc2626));
    exportButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5d80e5));
    auto knob = [this] (juce::Slider& slider, const char* id, double initial)
    {
        slider.setComponentID (id);
        slider.setName (id);
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 22);
        slider.setRotaryParameters (
            juce::degreesToRadians (207.0f), juce::degreesToRadians (513.0f), true);
        slider.setMouseDragSensitivity (200);
        slider.setDoubleClickReturnValue (true, initial);
        addAndMakeVisible (slider);
    };
    knob (speedSlider, "speed", 1.0);
    knob (wetSlider, "wet", 0.5);
    knob (gainSlider, "gain", 0.0);
    auto& state = p.apvts;
    speedAtt = std::make_unique<SliderAtt> (state, sverb::params::speed, speedSlider);
    wetAtt = std::make_unique<SliderAtt> (state, sverb::params::reverbWet, wetSlider);
    gainAtt = std::make_unique<SliderAtt> (state, sverb::params::gain, gainSlider);
    hz432Att = std::make_unique<ButtonAtt> (state, sverb::params::hz432, hz432Button);
    reverbOnAtt = std::make_unique<ButtonAtt> (state, sverb::params::reverbOn, reverbToggle);
    perfectionAtt = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (sverb::params::perfection),
        [this] (float v)
        {
            const int index = juce::jlimit (0, 2, static_cast<int> (v));
            const juce::uint32 colours[]{0xff550000, 0xffffcc00, 0xff10b981};
            const char* names[]{"Perfection: OFF", "Perfection: SLOW", "Perfection: FAST"};
            perfectionButton.getProperties ().set ("ledColour",
                                                   static_cast<juce::int64> (colours[index]));
            perfectionButton.repaint ();
            perfectionLabel.setText (names[index], juce::dontSendNotification);
            perfectionLabel.setColour (juce::Label::textColourId,
                                       juce::Colour (index == 0 ? 0xff9ca3af : colours[index]));
        });
    perfectionAtt->sendInitialUpdate ();
    wetSlider.onDragStart = [this]
    {
        if (! reverbToggle.getToggleState ())
            reverbToggle.setToggleState (true, juce::sendNotificationSync);
    };
    loadButton.onClick = [this]
    {
        chooseAudio ();
    };
    exportButton.onClick = [this]
    {
        chooseExport ();
    };
    perfectionButton.onClick = [this]
    {
        audioProcessor.perfection.cycle ();
    };
    loopButton.onClick = [this]
    {
        audioProcessor.transport.setLooping (loopButton.getToggleState ());
    };
    playButton.onClick = [this]
    {
        if (audioProcessor.transport.isPlaying ())
        {
            audioProcessor.transport.pause ();
            paused = true;
        }
        else
        {
            audioProcessor.transport.play ();
            paused = false;
        }
        refreshControls ();
    };
    stopButton.onClick = [this]
    {
        audioProcessor.transport.requestStop ();
        paused = false;
        setStatus ("Audio loaded and ready.");
        refreshControls ();
    };
    p.addChangeListener (this);
    setResizable (false, false);
    setSize (550, 730);
    setStatus (p.hasAudio () ? "Audio loaded and ready." : "Ready to load audio.");
    changeListenerCallback (&p);
    startTimerHz (30);
}
SVERBAudioProcessorEditor::~SVERBAudioProcessorEditor ()
{
    stopTimer ();
    audioProcessor.removeChangeListener (this);
    fileChooser.reset ();
    setLookAndFeel (nullptr);
}
void SVERBAudioProcessorEditor::setStatus (const juce::String& text)
{
    statusLabel.setText ("Status: " + text, juce::dontSendNotification);
}
void SVERBAudioProcessorEditor::showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions ()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton ("OK")
                                      .withAssociatedComponent (this),
                                  nullptr);
}
void SVERBAudioProcessorEditor::chooseAudio ()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Load Audio File", juce::File (), "*.wav;*.mp3;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<SVERBAudioProcessorEditor> (this)] (
            const juce::FileChooser& chooser)
        {
            if (safe && chooser.getResult ().existsAsFile ())
            {
                safe->paused = false;
                safe->audioProcessor.loadFileAsync (chooser.getResult ());
            }
        });
}
void SVERBAudioProcessorEditor::chooseExport ()
{
    if (audioProcessor.transport.isPlaying ())
    {
        showError ("Playback Active",
                   "Please stop or pause playback before starting the export process.");
        return;
    }
    auto request = audioProcessor.makeExportRequest ();
    if (! request.audio)
    {
        showError ("No Audio", "Please load an audio file before exporting.");
        return;
    }
    fileChooser = std::make_unique<juce::FileChooser> (
        "Export Processed Audio (WAV)", audioProcessor.suggestedExportFile (request), "*.wav");
    fileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<SVERBAudioProcessorEditor> (this),
         request] (const juce::FileChooser& chooser)
        {
            if (safe && chooser.getResult () != juce::File ())
                safe->audioProcessor.exportAsync (chooser.getResult (), request);
        });
}
void SVERBAudioProcessorEditor::refreshControls ()
{
    const bool loaded = audioProcessor.hasAudio (), playing = audioProcessor.transport.isPlaying ();
    playButton.setEnabled (loaded);
    stopButton.setEnabled (loaded);
    loopButton.setEnabled (loaded);
    exportButton.setEnabled (loaded && ! audioProcessor.isExporting ());
    loadButton.setEnabled (! audioProcessor.isLoading ());
    playButton.setButtonText (playing ? "PAUSE" : "PLAY");
    loopButton.setToggleState (audioProcessor.transport.isLooping (), juce::dontSendNotification);
    const bool hz = hz432Button.getToggleState ();
    hz432Label.setText (hz ? "432Hz Active" : "432Hz", juce::dontSendNotification);
    hz432Label.setColour (juce::Label::textColourId, juce::Colour (hz ? 0xfff87171 : 0xff9ca3af));
    wetSlider.setColour (juce::Slider::textBoxTextColourId,
                         juce::Colour (reverbToggle.getToggleState () ? 0xffffffff : 0xff6b7280));
    if (! audioProcessor.isExporting () && ! audioProcessor.isLoading ())
    {
        if (playing)
        {
            auto text = "Playing... (Speed: " + juce::String (speedSlider.getValue (), 2) + "x";
            if (hz)
                text += " / 432Hz Mode";
            text += ")";
            if (audioProcessor.transport.isLooping ())
                text += " [LOOP]";
            setStatus (text);
        }
        else if (paused)
            setStatus ("Paused. (Offset: " +
                       juce::String (audioProcessor.transport.getPositionSeconds (), 2) + "s)");
    }
}
void SVERBAudioProcessorEditor::timerCallback ()
{
    if (audioProcessor.transport.consumeFinishedFlag ())
    {
        paused = false;
        setStatus ("Playback finished. Ready to restart.");
    }
    refreshControls ();
}
void SVERBAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto status = audioProcessor.takePendingStatus ();
    if (status.isNotEmpty ())
    {
        paused = false;
        setStatus (status);
    }
    const auto name = audioProcessor.getLoadedFileName ();
    loadButton.setButtonText (audioProcessor.isLoading () ? "Decoding audio..."
                              : name.isEmpty ()           ? "Load Audio File"
                                                          : name.substring (0, 20) + "...");
    if (auto error = audioProcessor.takePendingError ())
    {
        loadButton.setButtonText ("Load Audio File");
        showError (error->first, error->second);
    }
    refreshControls ();
}
void SVERBAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff282828));
    g.setColour (juce::Colour (0xff333333));
    g.drawRect (getLocalBounds (), 1);
    const juce::Rectangle<float> panel (24, 190, 502, 448);
    g.setColour (juce::Colour (0xff1e1e1e));
    g.fillRoundedRectangle (panel, 12);
    g.setColour (juce::Colour (0xff333333));
    g.drawRoundedRectangle (panel, 12, 1);
    g.setColour (juce::Colours::white.withAlpha (0.1f));
    g.fillRect (24, 174, 502, 1);
    g.fillRect (44, 468, 462, 1);
    g.fillRect (24, 654, 502, 1);
}
void SVERBAudioProcessorEditor::resized ()
{
    const juce::Rectangle<int> header (24, 24, 502, 54), toolbar (24, 94, 502, 40);
    titleLabel.setBounds (header.withHeight (36));
    subtitleLabel.setBounds (24, header.getY () + 36, header.getWidth (), 18);
    loadButton.setBounds (toolbar.withWidth (192));
    loopButton.setBounds (326, toolbar.getY (), 56, toolbar.getHeight ());
    playButton.setBounds (398, toolbar.getY (), 56, toolbar.getHeight ());
    stopButton.setBounds (470, toolbar.getY (), 56, toolbar.getHeight ());
    statusLabel.setBounds (24, 142, 502, 20);
    hz432Button.setBounds (91, 292, 60, 60);
    hz432Label.setBounds (66, 358, 110, 16);
    speedTitle.setBounds (195, 210, 160, 24);
    speedSlider.setBounds (195, 242, 160, 190);
    speedMinLabel.setBounds (195, 436, 80, 16);
    speedMaxLabel.setBounds (275, 436, 80, 16);
    perfectionButton.setBounds (399, 292, 60, 60);
    perfectionLabel.setBounds (374, 358, 110, 16);
    reverbOnTitle.setBounds (44, 484, 154, 18);
    reverbToggle.setBounds (66, 506, 110, 90);
    wetTitle.setBounds (198, 484, 154, 18);
    wetSlider.setBounds (230, 506, 90, 112);
    gainTitle.setBounds (352, 484, 154, 18);
    gainSlider.setBounds (384, 506, 90, 112);
    auto footer = juce::Rectangle<int> (99, 670, 352, 36);
    resetButton.setBounds (footer.removeFromLeft (96));
    footer.removeFromLeft (16);
    exportButton.setBounds (footer);
}
