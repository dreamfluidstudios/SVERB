#pragma once
#include "Parameters.h"
#include "VarispeedPlayer.h"
#include <stdexcept>

namespace sverb
{
    inline void renderWav (const LoadedAudio& src,
                           const Snapshot& values,
                           const juce::File& destination,
                           const std::atomic<bool>& shutdown)
    {
        constexpr int blockSize = 4096;
        const int channels = src.buffer.getNumChannels ();
        const double rate = effectiveRate (values.speed, values.hz432);
        const auto length = static_cast<juce::int64> (
            std::ceil (src.buffer.getNumSamples () / rate) + std::ceil (3.0 * src.sampleRate));
        if (length * channels * 2 > 0xffffffffLL - 64)
            throw std::runtime_error ("The rendered audio exceeds the 4 GiB WAV size limit.");
        // Render to a sibling temporary file; failed writes never destroy an existing destination.
        juce::TemporaryFile temporary (destination);
        std::unique_ptr<juce::OutputStream> stream = temporary.getFile ().createOutputStream ();
        if (! stream)
            throw std::runtime_error ("Could not create the output file.");
        auto writer = juce::WavAudioFormat ().createWriterFor (stream,
                                                               juce::AudioFormatWriterOptions ()
                                                                   .withSampleRate (src.sampleRate)
                                                                   .withNumChannels (channels)
                                                                   .withBitsPerSample (16));
        if (! writer)
            throw std::runtime_error ("Could not create the WAV writer.");
        const juce::dsp::ProcessSpec spec{
            src.sampleRate, blockSize, static_cast<juce::uint32> (channels)};
        VarispeedPlayer player;
        player.prepare (src.sampleRate, blockSize);
        player.startOffline (rate);
        juce::dsp::Reverb reverb;
        reverb.prepare (spec);
        reverb.setParameters (reverbParameters ());
        reverb.reset ();
        juce::dsp::DryWetMixer<float> mixer;
        mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
        mixer.setWetMixProportion (values.reverbOn ? values.wet : 0.0f);
        mixer.prepare (spec);
        mixer.reset ();
        juce::dsp::Gain<float> gain;
        gain.prepare (spec);
        gain.setGainDecibels (values.gainDb);
        gain.reset ();
        juce::AudioBuffer<float> buffer (channels, blockSize);
        for (juce::int64 offset = 0; offset < length;)
        {
            if (shutdown.load ())
                return;
            const int count =
                static_cast<int> (juce::jmin<juce::int64> (blockSize, length - offset));
            player.render (src, buffer, count, rate);
            auto block =
                juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, static_cast<size_t> (count));
            mixer.pushDrySamples (block);
            reverb.process (juce::dsp::ProcessContextReplacing<float> (block));
            mixer.mixWetSamples (block);
            gain.process (juce::dsp::ProcessContextReplacing<float> (block));
            // Explicitly bound PCM input, including any interpolation overshoot.
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < count; ++i)
                    buffer.setSample (ch, i, juce::jlimit (-1.0f, 1.0f, buffer.getSample (ch, i)));
            if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
                throw std::runtime_error ("Could not write audio data (check free disk space).");
            offset += count;
        }
        if (! writer->flush ())
            throw std::runtime_error ("Could not flush the WAV file.");
        writer.reset ();
        if (shutdown.load ())
            return;
        if (! temporary.overwriteTargetFileWithTemporary ())
            throw std::runtime_error ("Could not save the completed WAV file.");
    }
}
