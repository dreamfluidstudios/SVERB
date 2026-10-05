#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AudioFileLoader.h"
#include "OfflineExporter.h"

SVERBAudioProcessor::SVERBAudioProcessor ()
    : AudioProcessor (
          BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true)),
      apvts (*this, nullptr, "SVERB_STATE", sverb::params::createParameterLayout ()),
      perfection (apvts)
{
    formatManager.registerBasicFormats ();
    speedParam = apvts.getRawParameterValue (sverb::params::speed);
    reverbOnParam = apvts.getRawParameterValue (sverb::params::reverbOn);
    reverbWetParam = apvts.getRawParameterValue (sverb::params::reverbWet);
    gainParam = apvts.getRawParameterValue (sverb::params::gain);
    hz432Param = apvts.getRawParameterValue (sverb::params::hz432);
}
SVERBAudioProcessor::~SVERBAudioProcessor ()
{
    shuttingDown.store (true);
    // Jobs cooperatively cancel between decode/render chunks. Never destroy their owner early.
    threadPool.removeAllJobs (true, -1);
    cancelPendingUpdate ();
}
void SVERBAudioProcessor::prepareToPlay (double sr, int maximumBlockSize)
{
    preparedBlockSize = juce::jmax (1, maximumBlockSize);
    const juce::dsp::ProcessSpec spec{sr,
                                      static_cast<juce::uint32> (preparedBlockSize),
                                      static_cast<juce::uint32> (getTotalNumOutputChannels ())};
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        transport.prepare (sr, maximumBlockSize);
    }
    reverb.prepare (spec);
    reverb.setParameters (sverb::reverbParameters ());
    reverb.reset ();
    mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
    mixer.setWetMixProportion (reverbOnParam->load () >= 0.5f ? reverbWetParam->load () : 0.0f);
    mixer.prepare (spec);
    mixer.reset ();
    masterGain.prepare (spec);
    masterGain.setRampDurationSeconds (0.05);
    masterGain.setGainDecibels (gainParam->load ());
    masterGain.reset ();
}
void SVERBAudioProcessor::releaseResources ()
{
    reverb.reset ();
    mixer.reset ();
    masterGain.reset ();
}
bool SVERBAudioProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet ().isDisabled () &&
           (layout.getMainOutputChannelSet () == juce::AudioChannelSet::mono () ||
            layout.getMainOutputChannelSet () == juce::AudioChannelSet::stereo ());
}
void SVERBAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    {
        const juce::SpinLock::ScopedTryLockType lock (audioLock);
        if (! lock.isLocked () || ! currentAudio)
            buffer.clear ();
        else
            transport.render (
                *currentAudio,
                buffer,
                buffer.getNumSamples (),
                sverb::effectiveRate (speedParam->load (), hz432Param->load () >= 0.5f));
    }
    mixer.setWetMixProportion (reverbOnParam->load () >= 0.5f ? reverbWetParam->load () : 0.0f);
    masterGain.setGainDecibels (gainParam->load ());
    // Hosts can exceed their advertised block size. Slice without allocating or resizing DSP buffers.
    juce::dsp::AudioBlock<float> entireBlock (buffer);
    for (int offset = 0; offset < buffer.getNumSamples (); offset += preparedBlockSize)
    {
        auto block = entireBlock.getSubBlock (
            static_cast<size_t> (offset),
            static_cast<size_t> (juce::jmin (preparedBlockSize, buffer.getNumSamples () - offset)));
        mixer.pushDrySamples (block);
        reverb.process (juce::dsp::ProcessContextReplacing<float> (block));
        mixer.mixWetSamples (block);
        masterGain.process (juce::dsp::ProcessContextReplacing<float> (block));
    }
}
void SVERBAudioProcessor::loadFileAsync (const juce::File& file)
{
    const auto generation = ++loadGeneration;
    if (file.getSize () > sverb::kMaxUploadBytes)
    {
        loading.store (false);
        reportError ("File Too Large", "Please choose a file smaller than 150 MB.");
        return;
    }
    loading.store (true);
    pendingStatus = "Decoding audio...";
    sendChangeMessage ();
    threadPool.addJob (
        [this, file, generation]
        {
            Completion result;
            result.isLoad = true;
            result.generation = generation;
            try
            {
                if (! shuttingDown.load ())
                    result.audio = sverb::decodeAudio (formatManager, file, shuttingDown);
            }
            catch (const std::length_error& e)
            {
                result.title = "File Too Large";
                result.error = e.what ();
            }
            catch (const std::exception& e)
            {
                result.title = "Audio Error";
                result.error = e.what ();
            }
            postCompletion (std::move (result));
        });
}
void SVERBAudioProcessor::postCompletion (Completion result)
{
    if (shuttingDown.load ())
        return;
    {
        const juce::ScopedLock lock (completionLock);
        completions.push_back (std::move (result));
    }
    triggerAsyncUpdate ();
}
void SVERBAudioProcessor::handleAsyncUpdate ()
{
    std::deque<Completion> ready;
    {
        const juce::ScopedLock lock (completionLock);
        ready.swap (completions);
    }
    for (auto& result : ready)
    {
        if (result.isLoad)
        {
            if (result.generation != loadGeneration)
                continue;
            loading.store (false);
            if (result.audio)
            {
                transport.requestStop ();
                {
                    const juce::SpinLock::ScopedLockType lock (audioLock);
                    currentAudio.swap (result.audio);
                    transport.resetPositionUnsafe ();
                }
                result.audio
                    .reset (); // Old allocation released on message thread, outside the spin lock.
                pendingStatus = "Audio loaded and ready.";
            }
            else
                pendingStatus = hasAudio () ? "Audio loaded and ready." : "Ready to load audio.";
        }
        else
        {
            exporting.store (false);
            pendingStatus =
                result.error.isEmpty () ? "Export complete! File saved." : "Export failed.";
        }
        if (result.error.isNotEmpty ())
            reportError (result.title, result.error);
    }
    sendChangeMessage ();
}
bool SVERBAudioProcessor::hasAudio () const
{
    const juce::SpinLock::ScopedLockType lock (audioLock);
    return currentAudio != nullptr;
}
juce::String SVERBAudioProcessor::getLoadedFileName () const
{
    std::shared_ptr<const sverb::LoadedAudio> audio;
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        audio = currentAudio;
    }
    return audio ? audio->sourceFile.getFileName () : juce::String ();
}
SVERBAudioProcessor::ExportRequest SVERBAudioProcessor::makeExportRequest () const
{
    ExportRequest request;
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        request.audio = currentAudio;
    }
    request.values = {speedParam->load (),
                      reverbWetParam->load (),
                      gainParam->load (),
                      reverbOnParam->load () >= 0.5f,
                      hz432Param->load () >= 0.5f};
    return request;
}
juce::File SVERBAudioProcessor::suggestedExportFile (const ExportRequest& request) const
{
    if (! request.audio)
        return {};
    auto name = "SVERB_SPEED" + juce::String (request.values.speed, 2) + "x";
    if (request.values.hz432)
        name += "_432Hz";
    name += "_" + request.audio->sourceFile.getFileNameWithoutExtension () + ".wav";
    return request.audio->sourceFile.getSiblingFile (name);
}
void SVERBAudioProcessor::exportAsync (const juce::File& destination, ExportRequest request)
{
    if (transport.isPlaying ())
    {
        reportError ("Playback Active",
                     "Please stop or pause playback before starting the export process.");
        return;
    }
    if (! request.audio)
    {
        reportError ("No Audio", "Please load an audio file before exporting.");
        return;
    }
    if (destination == request.audio->sourceFile)
    {
        reportError ("Export Failed",
                     "Choose a different filename to preserve the original audio.");
        return;
    }
    if (exporting.exchange (true))
        return;
    pendingStatus = "Rendering audio effects... Please wait.";
    sendChangeMessage ();
    threadPool.addJob (
        [this, destination, request = std::move (request)]
        {
            Completion result;
            try
            {
                if (! shuttingDown.load ())
                    sverb::renderWav (*request.audio, request.values, destination, shuttingDown);
            }
            catch (const std::exception& e)
            {
                result.title = "Export Failed";
                result.error = "An error occurred during the audio rendering process: " +
                               juce::String (e.what ());
            }
            postCompletion (std::move (result));
        });
}
void SVERBAudioProcessor::reportError (juce::String title, juce::String message)
{
    pendingError = std::make_pair (std::move (title), std::move (message));
    sendChangeMessage ();
}
std::optional<std::pair<juce::String, juce::String>> SVERBAudioProcessor::takePendingError ()
{
    auto result = std::move (pendingError);
    pendingError.reset ();
    return result;
}
juce::String SVERBAudioProcessor::takePendingStatus ()
{
    auto result = pendingStatus;
    pendingStatus.clear ();
    return result;
}
void SVERBAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState ().createXml ())
        copyXmlToBinary (*xml, dest);
}
void SVERBAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType ()))
        {
            perfection.beginStateRestore ();
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            perfection.syncAfterStateRestore ();
        }
}
juce::AudioProcessorEditor* SVERBAudioProcessor::createEditor ()
{
    return new SVERBAudioProcessorEditor (*this);
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter ()
{
    return new SVERBAudioProcessor ();
}
