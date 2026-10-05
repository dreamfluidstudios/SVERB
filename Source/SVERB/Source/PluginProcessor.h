#pragma once
#include <JuceHeader.h>
#include "Parameters.h"
#include "PerfectionController.h"
#include "VarispeedPlayer.h"
#include <deque>

class SVERBAudioProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AsyncUpdater
{
  public:
    SVERBAudioProcessor ();
    ~SVERBAudioProcessor () override;
    void prepareToPlay (double, int) override;
    void releaseResources () override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor () override;
    bool hasEditor () const override
    {
        return true;
    }
    const juce::String getName () const override
    {
        return JucePlugin_Name;
    }
    bool acceptsMidi () const override
    {
        return false;
    }
    bool producesMidi () const override
    {
        return false;
    }
    bool isMidiEffect () const override
    {
        return false;
    }
    double getTailLengthSeconds () const override
    {
        return 3.0;
    }
    int getNumPrograms () override
    {
        return 1;
    }
    int getCurrentProgram () override
    {
        return 0;
    }
    void setCurrentProgram (int) override
    {
    }
    const juce::String getProgramName (int) override
    {
        return {};
    }
    void changeProgramName (int, const juce::String&) override
    {
    }
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    sverb::PerfectionController perfection;
    sverb::VarispeedPlayer transport;
    void resetToDefaults ()
    {
        perfection.resetToDefaults ();
        transport.setLooping (false);
    }
    void loadFileAsync (const juce::File&);
    struct ExportRequest
    {
        std::shared_ptr<const sverb::LoadedAudio> audio;
        sverb::Snapshot values;
    };
    ExportRequest makeExportRequest () const;
    void exportAsync (const juce::File&, ExportRequest);
    void exportAsync (const juce::File& file)
    {
        exportAsync (file, makeExportRequest ());
    }
    juce::File suggestedExportFile (const ExportRequest&) const;
    bool hasAudio () const;
    bool isExporting () const
    {
        return exporting.load ();
    }
    bool isLoading () const
    {
        return loading.load ();
    }
    juce::String getLoadedFileName () const;
    std::optional<std::pair<juce::String, juce::String>> takePendingError ();
    juce::String takePendingStatus ();

  private:
    struct Completion
    {
        bool isLoad = false;
        unsigned generation = 0;
        std::shared_ptr<sverb::LoadedAudio> audio;
        juce::String title, error;
    };
    void postCompletion (Completion);
    void handleAsyncUpdate () override;
    void reportError (juce::String, juce::String);
    juce::AudioFormatManager formatManager;
    juce::ThreadPool threadPool{1};
    mutable juce::SpinLock audioLock;
    std::shared_ptr<sverb::LoadedAudio> currentAudio;
    juce::CriticalSection completionLock;
    std::deque<Completion> completions;
    std::atomic<bool> shuttingDown{false}, exporting{false}, loading{false};
    unsigned loadGeneration = 0;
    juce::String pendingStatus;
    std::optional<std::pair<juce::String, juce::String>> pendingError;
    juce::dsp::Reverb reverb;
    juce::dsp::DryWetMixer<float> mixer{0};
    juce::dsp::Gain<float> masterGain;
    int preparedBlockSize = 1;
    std::atomic<float>*speedParam{}, *reverbOnParam{}, *reverbWetParam{}, *gainParam{},
        *hz432Param{};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SVERBAudioProcessor)

    // This is another comment that you shouldn't see unless you zoom in. If you see this, congrats you win!
};
