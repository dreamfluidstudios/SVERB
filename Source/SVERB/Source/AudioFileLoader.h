#pragma once
#include "VarispeedPlayer.h"
#include <stdexcept>

namespace sverb
{
    namespace detail
    {
        inline std::shared_ptr<LoadedAudio> decodeWithReader (juce::AudioFormatReader& reader,
                                                              const juce::File& file,
                                                              const std::atomic<bool>& shutdown)
        {
            if (reader.lengthInSamples <= 0 || reader.numChannels == 0 ||
                ! std::isfinite (reader.sampleRate) || reader.sampleRate <= 0)
                return {};
            const int channels = juce::jmin (static_cast<int> (reader.numChannels), 2);
            const double bytes =
                static_cast<double> (reader.lengthInSamples) * channels * sizeof (float);
            if (bytes > static_cast<double> (kMaxDecodedBytes))
                throw std::length_error (
                    ("Decoded audio would require approx " +
                     juce::String (bytes / kMaxDecodedBytes, 2) +
                     " GiB memory which exceeds the allowed 1.0 GiB. Please choose a shorter file or reduce size.")
                        .toStdString ());
            auto data = std::make_shared<LoadedAudio> ();
            const int length = static_cast<int> (reader.lengthInSamples);
            data->buffer.setSize (channels, length);
            for (int offset = 0; offset < length;)
            {
                if (shutdown.load ())
                    return {};
                const int count = juce::jmin (65536, length - offset);
                if (! reader.read (&data->buffer, offset, count, offset, true, channels > 1))
                    return {}; // Discard partial data; the caller can retry another registered decoder.
                offset += count;
            }
            data->sampleRate = reader.sampleRate;
            data->sourceFile = file;
            return data;
        }
    }

    inline std::shared_ptr<LoadedAudio> decodeAudio (juce::AudioFormatManager& formats,
                                                     const juce::File& file,
                                                     const std::atomic<bool>& shutdown)
    {
        bool recognised = false;
        // AudioFormatManager only falls back when a reader cannot open a file. A reader
        // can also fail later: JUCE's MP3 reader may overestimate its sample count and
        // fail at the final chunk. Retry a full decode with the next compatible format
        // (Windows Media supports MP3), rather than accepting a partial/silent buffer.
        for (int index = 0; index < formats.getNumKnownFormats (); ++index)
        {
            if (shutdown.load ())
                return {};
            auto* format = formats.getKnownFormat (index);
            if (! format->canHandleFile (file))
                continue;
            auto stream = file.createInputStream ();
            if (! stream)
                throw std::runtime_error ("Could not open the audio file for reading.");
            std::unique_ptr<juce::AudioFormatReader> reader (
                format->createReaderFor (stream.release (), true));
            if (! reader)
                continue;
            recognised = true;
            if (auto audio = detail::decodeWithReader (*reader, file, shutdown))
                return audio;
        }
        if (shutdown.load ())
            return {};
        throw std::runtime_error (
            recognised
                ? "Could not read the complete audio file with any available decoder."
                : "Could not decode the audio file. Please ensure it is a valid MP3 or WAV file.");
    }
}
