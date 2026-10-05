#pragma once
#include <JuceHeader.h>
#include <cmath>

namespace sverb
{
    struct LoadedAudio
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
        juce::File sourceFile;
    };
    constexpr juce::int64 kMaxUploadBytes = 150LL * 1024 * 1024;
    constexpr juce::int64 kMaxDecodedBytes = 1024LL * 1024 * 1024;

    class VarispeedPlayer
    {
      public:
        void prepare (double sr, int)
        {
            deviceRate = sr;
            rateSmoother.reset (sr, 0.02);
            fadeGain.reset (sr, 0.01);
            fadeGain.setCurrentAndTargetValue (0.0f);
            rateInitialised = false;
        }
        void play ()
        {
            finished.store (false);
            playing.store (true);
        }
        void pause ()
        {
            playing.store (false);
        }
        void requestStop ()
        {
            playing.store (false);
            stopPending.store (true);
            finished.store (false);
        }
        void setLooping (bool b)
        {
            looping.store (b);
        }
        bool isLooping () const
        {
            return looping.load ();
        }
        bool isPlaying () const
        {
            return playing.load ();
        }
        double getPositionSeconds () const
        {
            return positionSeconds.load ();
        }
        bool consumeFinishedFlag ()
        {
            return finished.exchange (false);
        }
        // Caller must exclude the audio thread (audioLock); never resets shared DSP here.
        void resetPositionUnsafe ()
        {
            readPos = 0.0;
            positionSeconds.store (0.0);
            playing.store (false);
            stopPending.store (false);
            finished.store (false);
            resetAfterFade = false;
            fadeGain.setCurrentAndTargetValue (0.0f);
            rateInitialised = false;
        }
        void startOffline (double rate)
        {
            rateSmoother.setCurrentAndTargetValue (rate);
            rateInitialised = true;
            fadeGain.setCurrentAndTargetValue (1.0f);
            playing.store (true);
        }
        static float hermiteRead (const float* data, int len, double pos, bool wrap)
        {
            const int i = static_cast<int> (pos);
            const float t = static_cast<float> (pos - i);
            auto at = [=] (int k)
            {
                if (wrap)
                {
                    k %= len;
                    if (k < 0)
                        k += len;
                }
                return k < 0 || k >= len ? 0.0f : data[k];
            };
            const float x0 = at (i - 1), x1 = at (i), x2 = at (i + 1), x3 = at (i + 2);
            const float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
            const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
            return ((c3 * t + c2) * t + c1) * t + x1;
        }
        void render (const LoadedAudio& src, juce::AudioBuffer<float>& out, int count, double rate)
        {
            out.clear ();
            if (stopPending.exchange (false))
                resetAfterFade = true;
            const bool wantsPlay = playing.load (), wrap = looping.load ();
            fadeGain.setTargetValue (wantsPlay && ! resetAfterFade ? 1.0f : 0.0f);
            const double increment = rate * src.sampleRate / deviceRate;
            if (! rateInitialised)
            {
                rateSmoother.setCurrentAndTargetValue (increment);
                rateInitialised = true;
            }
            rateSmoother.setTargetValue (increment);
            const int len = src.buffer.getNumSamples ();
            if (len <= 0)
                return;
            const auto* const* source = src.buffer.getArrayOfReadPointers ();
            auto* const* output = out.getArrayOfWritePointers ();
            for (int n = 0; n < count; ++n)
            {
                if (! fadeGain.isSmoothing () && fadeGain.getCurrentValue () == 0.0f)
                {
                    if (resetAfterFade)
                    {
                        readPos = 0.0;
                        resetAfterFade = false;
                    }
                    if (! wantsPlay)
                        break;
                    fadeGain.setTargetValue (1.0f);
                }
                const float g = fadeGain.getNextValue ();
                for (int ch = 0; ch < out.getNumChannels (); ++ch)
                    output[ch][n] =
                        g * hermiteRead (source[juce::jmin (ch, src.buffer.getNumChannels () - 1)],
                                         len,
                                         readPos,
                                         wrap);
                readPos += rateSmoother.getNextValue ();
                if (readPos >= len)
                {
                    if (wrap)
                        readPos = std::fmod (readPos, static_cast<double> (len));
                    else
                    {
                        playing.store (false);
                        readPos = 0.0;
                        finished.store (wantsPlay && ! resetAfterFade);
                        resetAfterFade = false;
                        fadeGain.setCurrentAndTargetValue (0.0f);
                        break;
                    }
                }
            }
            positionSeconds.store (readPos / src.sampleRate);
        }

      private:
        std::atomic<bool> playing{false}, looping{false}, stopPending{false}, finished{false};
        std::atomic<double> positionSeconds{0.0};
        double readPos = 0.0, deviceRate = 44100.0;
        bool resetAfterFade = false, rateInitialised = false;
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> rateSmoother{1.0};
        juce::SmoothedValue<float> fadeGain{0.0f};

        // Here goes another comment that I'm adding just for content purposes, if you  see this then you know I'm not typing real code, congrats you win!
    };
}
