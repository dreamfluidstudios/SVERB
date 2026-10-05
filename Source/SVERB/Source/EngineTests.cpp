// Standalone regression harness. Build separately with SVERB_ENGINE_TESTS=1 and
// link against the project's Debug Shared Code/SVERB.lib (not part of the app).
#if SVERB_ENGINE_TESTS
#include "PluginProcessor.h"
#include "AudioFileLoader.h"
#include "OfflineExporter.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void check (bool condition, const char* description)
    {
        if (! condition)
            throw std::runtime_error (description);
        std::cout << "PASS: " << description << std::endl;
    }
    void pump ()
    {
        MSG message;
        while (PeekMessage (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessage (&message);
        }
    }
    template <typename Predicate> void waitFor (Predicate done)
    {
        const auto deadline = juce::Time::getMillisecondCounter () + 15000;
        while (! done () && juce::Time::getMillisecondCounter () < deadline)
        {
            pump ();
            juce::Thread::sleep (2);
        }
        pump ();
        if (! done ())
            throw std::runtime_error ("Background operation timed out");
    }
    float value (SVERBAudioProcessor& p, const char* id)
    {
        return p.apvts.getRawParameterValue (id)->load ();
    }
    bool nearlyEqual (double a, double b, double tolerance = 1.0e-5)
    {
        return std::abs (a - b) <= tolerance;
    }
}
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    try
    {
        const auto directory =
            juce::File::getCurrentWorkingDirectory ().getChildFile ("Application/Verification");
        directory.createDirectory ();
        const auto range = sverb::params::makeSpeedRange ();
        bool invertible = true;
        for (int i = 0; i <= 1000; ++i)
        {
            float n = i / 1000.0f;
            invertible &= nearlyEqual (range.convertTo0to1 (range.convertFrom0to1 (n)), n);
        }
        check (invertible && nearlyEqual (range.convertFrom0to1 (0.5f), 1.0),
               "Nonlinear speed range round-trips and centres at 1x");
        sverb::LoadedAudio source;
        source.sampleRate = 48000;
        source.buffer.setSize (1, 48000);
        source.sourceFile = directory.getChildFile ("test-source.wav");
        for (int i = 0; i < 48000; ++i)
            source.buffer.setSample (
                0,
                i,
                0.2f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * i / 48000.0f));
        juce::AudioBuffer<float> output (2, 480);
        sverb::VarispeedPlayer player;
        player.prepare (44100, 480);
        player.play ();
        player.render (source, output, 441, 1.0);
        check (nearlyEqual (player.getPositionSeconds (), 0.01),
               "Playback corrects file/device sample-rate mismatch");
        check (output.getMagnitude (0, 0, 441) > 0.01f && output.getMagnitude (1, 0, 441) > 0.01f,
               "Mono source feeds both stereo channels");
        player.pause ();
        player.render (source, output, 480, 1.0);
        const auto pausedPosition = player.getPositionSeconds ();
        player.render (source, output, 480, 1.0);
        check (nearlyEqual (pausedPosition, player.getPositionSeconds ()) &&
                   output.getMagnitude (0, 480) == 0,
               "Pause fades then freezes position");
        player.requestStop ();
        player.render (source, output, 480, 1.0);
        check (player.getPositionSeconds () == 0, "Stop rewinds a paused transport");
        player.play ();
        player.render (source, output, 480, 1.0);
        player.requestStop ();
        player.play ();
        player.render (source, output, 480, 1.0);
        player.render (source, output, 480, 1.0);
        check (player.isPlaying () && player.getPositionSeconds () < 0.012,
               "Rapid Stop/Play completes rewind before restarting");
        sverb::LoadedAudio tiny;
        tiny.sampleRate = 48000;
        tiny.buffer.setSize (1, 1);
        tiny.buffer.setSample (0, 0, 0.25f);
        sverb::VarispeedPlayer loop;
        loop.prepare (48000, 480);
        loop.setLooping (true);
        loop.play ();
        loop.render (tiny, output, 480, 2.0);
        check (loop.isPlaying () && std::isfinite (output.getSample (0, 479)),
               "One-sample loops wrap safely at 2x");
        loop.setLooping (false);
        loop.render (tiny, output, 480, 2.0);
        check (! loop.isPlaying () && loop.consumeFinishedFlag () && ! loop.consumeFinishedFlag (),
               "Natural completion resets and reports exactly once");
        std::atomic<bool> cancel{false};
        {
            std::unique_ptr<juce::OutputStream> stream = source.sourceFile.createOutputStream ();
            if (stream)
            {
                stream->setPosition (0);
                static_cast<juce::FileOutputStream*> (stream.get ())->truncate ();
            }
            auto writer = juce::WavAudioFormat ().createWriterFor (stream,
                                                                   juce::AudioFormatWriterOptions ()
                                                                       .withSampleRate (48000)
                                                                       .withNumChannels (1)
                                                                       .withBitsPerSample (16));
            check (writer && writer->writeFromAudioSampleBuffer (source.buffer, 0, 48000),
                   "WAV source fixture written");
        }
        juce::AudioFormatManager formats;
        formats.registerBasicFormats ();
        const auto decoded = sverb::decodeAudio (formats, source.sourceFile, cancel);
        check (decoded && decoded->sampleRate == 48000 && decoded->buffer.getNumSamples () == 48000,
               "WAV loading preserves native sample rate and length");
        if (argc > 1)
        {
            if (argc > 2 && juce::String (argv[2]) == "--probe")
            {
                for (int index = 0; index < formats.getNumKnownFormats (); ++index)
                {
                    auto* format = formats.getKnownFormat (index);
                    auto stream = juce::File (juce::String (argv[1])).createInputStream ();
                    std::unique_ptr<juce::AudioFormatReader> reader (
                        format->createReaderFor (stream.release (), true));
                    if (! reader)
                        continue;
                    std::cout << "Decoder: " << format->getFormatName () << ", rate "
                              << reader->sampleRate << ", channels " << reader->numChannels
                              << ", advertised samples " << reader->lengthInSamples << std::endl;
                    juce::AudioBuffer<float> probe (static_cast<int> (reader->numChannels), 65536);
                    for (juce::int64 offset = 0; offset < reader->lengthInSamples; offset += 65536)
                    {
                        const auto count = static_cast<int> (
                            juce::jmin<juce::int64> (65536, reader->lengthInSamples - offset));
                        if (! reader->read (
                                &probe, 0, count, offset, true, reader->numChannels > 1))
                        {
                            std::cout << "FAIL at " << offset << ", requested " << count
                                      << ", magnitude " << probe.getMagnitude (0, count)
                                      << std::endl;
                            break;
                        }
                        if (offset + count == reader->lengthInSamples)
                            std::cout << "FULL DECODE SUCCESS" << std::endl;
                    }
                }
                return 0;
            }
            const auto mp3 =
                sverb::decodeAudio (formats, juce::File (juce::String (argv[1])), cancel);
            check (mp3 && mp3->buffer.getNumSamples () > 0 &&
                       mp3->buffer.getMagnitude (0, mp3->buffer.getNumSamples ()) > 0.001f,
                   "MP3 decoding succeeds with non-silent audio");
        }
        SVERBAudioProcessor processor;
        sverb::setParameter (processor.apvts, sverb::params::speed, 0.8f);
        sverb::setParameter (processor.apvts, sverb::params::reverbOn, 0.0f);
        processor.perfection.cycle ();
        check (nearlyEqual (value (processor, "speed"), 0.91) &&
                   nearlyEqual (value (processor, "reverb_wet"), 0.36) &&
                   nearlyEqual (value (processor, "gain"), 0.6) &&
                   value (processor, "hz432") == 1 && value (processor, "reverb_on") == 1,
               "Slow preset applies all five parameters");
        processor.perfection.cycle ();
        check (nearlyEqual (value (processor, "speed"), 1.16) &&
                   nearlyEqual (value (processor, "gain"), -1.5),
               "Fast preset applies speed and gain");
        processor.perfection.cycle ();
        check (nearlyEqual (value (processor, "speed"), 0.8) &&
                   value (processor, "reverb_on") == 0 && value (processor, "hz432") == 0,
               "Leaving presets restores original snapshot");
        processor.perfection.cycle ();
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        SVERBAudioProcessor restored;
        restored.setStateInformation (state.getData (), static_cast<int> (state.getSize ()));
        pump ();
        check (nearlyEqual (value (restored, "speed"), 0.91) && value (restored, "perfection") == 1,
               "APVTS state restores without reapplying preset");
        restored.perfection.cycle ();
        restored.perfection.cycle ();
        check (value (restored, "speed") == 1 && value (restored, "hz432") == 0,
               "Restored preset returns to factory defaults without snapshot");
        processor.loadFileAsync (source.sourceFile);
        waitFor (
            [&]
            {
                return ! processor.isLoading ();
            });
        check (processor.hasAudio (), "Background load installs decoded audio");
        if (argc > 1)
        {
            const juce::File mp3File{juce::String (argv[1])};
            processor.loadFileAsync (mp3File);
            waitFor (
                [&]
                {
                    return ! processor.isLoading ();
                });
            const auto loadError = processor.takePendingError ();
            if (loadError)
                throw std::runtime_error (loadError->second.toStdString ());
            const auto loaded = processor.makeExportRequest ().audio;
            check (loaded && loaded->sourceFile == mp3File &&
                       loaded->buffer.getMagnitude (0, loaded->buffer.getNumSamples ()) > 0.001f,
                   "MP3 background loading installs the selected file with audible samples");
            processor.prepareToPlay (44100, 512);
            juce::AudioBuffer<float> mp3Block (2, 512);
            juce::MidiBuffer mp3Midi;
            processor.transport.play ();
            float peak = 0;
            for (int i = 0; i < 200; ++i)
            {
                processor.processBlock (mp3Block, mp3Midi);
                peak = juce::jmax (peak, mp3Block.getMagnitude (0, 512));
            }
            check (peak > 0.001f, "Loaded MP3 produces audio through processBlock");
            processor.transport.requestStop ();
            processor.releaseResources ();
            processor.loadFileAsync (source.sourceFile);
            waitFor (
                [&]
                {
                    return ! processor.isLoading ();
                });
        }
        processor.loadFileAsync (directory.getChildFile ("missing.wav"));
        waitFor (
            [&]
            {
                return ! processor.isLoading ();
            });
        check (processor.hasAudio () && processor.takePendingError ().has_value (),
               "Failed replacement preserves previous audio");
        const auto oversized = directory.getChildFile ("oversized.wav");
        {
            auto stream = oversized.createOutputStream ();
            stream->setPosition (sverb::kMaxUploadBytes);
            stream->writeByte (0);
        }
        processor.loadFileAsync (oversized);
        const auto error = processor.takePendingError ();
        check (error && error->first == "File Too Large" && processor.hasAudio (),
               "150 MiB upload guard preserves existing audio");
        oversized.deleteFile ();
        sverb::setParameter (processor.apvts, sverb::params::speed, 1.0f);
        sverb::setParameter (processor.apvts, sverb::params::hz432, 0.0f);
        sverb::setParameter (processor.apvts, sverb::params::reverbOn, 0.0f);
        sverb::setParameter (processor.apvts, sverb::params::gain, 0.0f);
        processor.prepareToPlay (48000, 128);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block (2, 8192);
        processor.transport.play ();
        processor.processBlock (block, midi);
        check (block.getMagnitude (0, 8192) > 0.1f && std::isfinite (block.getSample (1, 8191)),
               "processBlock renders audio and handles oversized host blocks");
        processor.transport.pause ();
        processor.processBlock (block, midi);
        auto request = processor.makeExportRequest ();
        const auto destination = directory.getChildFile ("dry-export.wav");
        processor.exportAsync (destination, request);
        waitFor (
            [&]
            {
                return ! processor.isExporting ();
            });
        std::unique_ptr<juce::AudioFormatReader> exported (formats.createReaderFor (destination));
        check (exported && exported->sampleRate == 48000 && exported->numChannels == 1 &&
                   exported->bitsPerSample == 16 && exported->lengthInSamples == 192000,
               "Background WAV export has 16-bit PCM, source sample rate, mono and 3-second tail");
        juce::AudioBuffer<float> exportedBuffer (1, 192000);
        exported->read (&exportedBuffer, 0, 192000, 0, true, false);
        double maxError = 0;
        for (int i = 0; i < 48000; ++i)
            maxError =
                juce::jmax (maxError,
                            std::abs (static_cast<double> (exportedBuffer.getSample (0, i) -
                                                           decoded->buffer.getSample (0, i))));
        check (maxError < 0.0001 && exportedBuffer.getMagnitude (0, 48000, 144000) == 0,
               "Dry export matches source without initial ramps and has silent tail");
        auto wetValues = sverb::Snapshot{0.91f, 1.0f, 0.6f, true, true};
        sverb::renderWav (*decoded, wetValues, directory.getChildFile ("wet-export.wav"), cancel);
        std::unique_ptr<juce::AudioFormatReader> wetReader (
            formats.createReaderFor (directory.getChildFile ("wet-export.wav")));
        const auto expected = static_cast<juce::int64> (
            std::ceil (48000 / sverb::effectiveRate (0.91f, true)) + 144000);
        check (wetReader && wetReader->lengthInSamples == expected,
               "432Hz and speed determine exact offline duration");
        juce::AudioBuffer<float> tail (1, 4800);
        wetReader->read (&tail, 0, 4800, expected - 144000, true, false);
        check (tail.getMagnitude (0, 4800) > 0.0001f, "Wet export contains a decaying reverb tail");
        processor.releaseResources ();
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor ());
            // Exercise the actual button while a preset update is still queued.
            processor.transport.setLooping (true);
            sverb::setParameter (processor.apvts, sverb::params::perfection, 2.0f);
            sverb::setParameter (processor.apvts, sverb::params::speed, 1.8f);
            auto* reset = dynamic_cast<juce::Button*> (editor->findChildWithID ("reset"));
            check (reset != nullptr && reset->isEnabled (), "Reset button is available");
            reset->onClick ();
            pump ();
            bool defaults = ! processor.transport.isLooping () && processor.hasAudio ();
            for (const auto* id :
                 {"speed", "reverb_wet", "gain", "reverb_on", "hz432", "perfection"})
            {
                auto* parameter = processor.apvts.getParameter (id);
                defaults &= nearlyEqual (parameter->getValue (), parameter->getDefaultValue ());
            }
            check (
                defaults,
                "Reset restores all defaults, clears pending preset and disables loop while retaining audio");
            processor.perfection.cycle ();
            processor.perfection.cycle ();
            processor.perfection.cycle ();
            pump ();
            check (value (processor, "speed") == 1.0f && value (processor, "reverb_wet") == 0.5f &&
                       value (processor, "gain") == 0.0f && value (processor, "hz432") == 0.0f,
                   "Preset cycling after Reset cannot resurrect the previous settings");
            check (editor->getWidth () == 550 && editor->getHeight () == 730 &&
                       ! editor->isResizable (),
                   "Native editor has specified fixed dimensions");
            auto image = editor->createComponentSnapshot (editor->getLocalBounds ());
            auto stream = directory.getChildFile ("native-editor.png").createOutputStream ();
            if (stream)
            {
                stream->setPosition (0);
                static_cast<juce::FileOutputStream*> (stream.get ())->truncate ();
            }
            check (stream && juce::PNGImageFormat ().writeImageToStream (image, *stream),
                   "Native editor rendered for visual inspection");
        }
        std::cout << "All engine checks passed." << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what () << std::endl;
        return 1;
    }
}
#endif
