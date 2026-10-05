#pragma once
#include "PluginProcessor.h"
#include "SverbLookAndFeel.h"

class SVERBAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer,
                                  private juce::ChangeListener
{
  public:
    explicit SVERBAudioProcessorEditor (SVERBAudioProcessor&);
    ~SVERBAudioProcessorEditor () override;
    void paint (juce::Graphics&) override;
    void resized () override;

  private:
    void timerCallback () override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshControls ();
    void chooseAudio ();
    void chooseExport ();
    void showError (const juce::String&, const juce::String&);
    void setStatus (const juce::String&);
    SVERBAudioProcessor& audioProcessor;
    SverbLookAndFeel lnf;
    juce::Label titleLabel, subtitleLabel, statusLabel, hz432Label, perfectionLabel;
    juce::Label speedTitle, speedMinLabel, speedMaxLabel, reverbOnTitle, wetTitle, gainTitle;
    juce::TextButton loadButton{"Load Audio File"}, loopButton{"LOOP"}, playButton{"PLAY"},
        stopButton{"STOP"};
    juce::TextButton resetButton{"RESET"}, perfectionButton,
        exportButton{"Export Processed Audio (WAV)"};
    juce::ToggleButton hz432Button, reverbToggle;
    juce::Slider speedSlider, wetSlider, gainSlider;
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool paused = false;
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> speedAtt, wetAtt, gainAtt;
    std::unique_ptr<ButtonAtt> hz432Att, reverbOnAtt;
    std::unique_ptr<juce::ParameterAttachment> perfectionAtt;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SVERBAudioProcessorEditor)
};
