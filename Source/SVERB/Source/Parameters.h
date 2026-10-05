#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <optional>

namespace sverb
{
    inline double effectiveRate (double speed, bool hz432)
    {
        return speed * (hz432 ? 432.0 / 440.0 : 1.0);
    }
    struct Snapshot
    {
        float speed = 1.0f, wet = 0.5f, gainDb = 0.0f;
        bool reverbOn = true, hz432 = false;
    };
    namespace params
    {
        inline constexpr auto speed = "speed", reverbOn = "reverb_on", reverbWet = "reverb_wet",
                              gain = "gain", hz432 = "hz432", perfection = "perfection";
        inline juce::NormalisableRange<float> makeSpeedRange ()
        {
            return {0.5f,
                    2.0f,
                    [] (float, float, float n)
                    {
                        n = juce::jlimit (0.0f, 1.0f, n);
                        return n <= 0.5f ? 1.0f - 0.5f * std::sqrt (1.0f - 2.0f * n)
                                         : 1.0f + std::sqrt (2.0f * n - 1.0f);
                    },
                    [] (float, float, float v)
                    {
                        v = juce::jlimit (0.5f, 2.0f, v);
                        return v <= 1.0f ? 0.5f * (1.0f - std::pow (2.0f - 2.0f * v, 2.0f))
                                         : 0.5f + 0.5f * (v - 1.0f) * (v - 1.0f);
                    },
                    [] (float, float, float v)
                    {
                        return juce::jlimit (0.5f, 2.0f, v);
                    }};
        }
        inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout ()
        {
            using namespace juce;
            AudioProcessorValueTreeState::ParameterLayout layout;
            auto parse = [] (const String& t)
            {
                return t.retainCharacters ("-0123456789.").getFloatValue ();
            };
            auto boolAttributes = AudioParameterBoolAttributes ().withStringFromValueFunction (
                [] (bool b, int)
                {
                    return b ? String ("ON") : String ("OFF");
                });
            layout.add (
                std::make_unique<AudioParameterFloat> (ParameterID{speed, 1},
                                                       "Speed",
                                                       makeSpeedRange (),
                                                       1.0f,
                                                       AudioParameterFloatAttributes ()
                                                           .withStringFromValueFunction (
                                                               [] (float v, int)
                                                               {
                                                                   return String (v, 2) + "x";
                                                               })
                                                           .withValueFromStringFunction (parse)));
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID{reverbOn, 1}, "Reverb On", true, boolAttributes));
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID{reverbWet, 1},
                "Reverb Wetness",
                NormalisableRange<float> (0.0f, 1.0f),
                0.5f,
                AudioParameterFloatAttributes ()
                    .withStringFromValueFunction (
                        [] (float v, int)
                        {
                            return String (roundToInt (v * 100)) + "%";
                        })
                    .withValueFromStringFunction (
                        [parse] (const String& t)
                        {
                            return parse (t) / 100.0f;
                        })));
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID{gain, 1},
                "Gain",
                NormalisableRange<float> (-6.0f, 6.0f),
                0.0f,
                AudioParameterFloatAttributes ()
                    .withStringFromValueFunction (
                        [] (float v, int)
                        {
                            return (v > 0 ? "+" : "") + String (v, 1) + "dB";
                        })
                    .withValueFromStringFunction (parse)));
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID{hz432, 1}, "432Hz Mode", false, boolAttributes));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID{perfection, 1}, "Perfection", StringArray{"Off", "Slow", "Fast"}, 0));
            const auto range = makeSpeedRange ();
            jassert (range.convertFrom0to1 (0.0f) == 0.5f && range.convertFrom0to1 (0.5f) == 1.0f &&
                     range.convertFrom0to1 (1.0f) == 2.0f);
            jassert (std::abs (range.convertTo0to1 (0.91f) - 0.4838f) < 0.0001f);
            jassert (std::abs (range.convertTo0to1 (1.16f) - 0.5128f) < 0.0001f);
            juce::ignoreUnused (range);
            return layout;
        }
    }
    inline Snapshot snapshot (juce::AudioProcessorValueTreeState& state)
    {
        return {state.getRawParameterValue (params::speed)->load (),
                state.getRawParameterValue (params::reverbWet)->load (),
                state.getRawParameterValue (params::gain)->load (),
                state.getRawParameterValue (params::reverbOn)->load () >= 0.5f,
                state.getRawParameterValue (params::hz432)->load () >= 0.5f};
    }
    inline void
    setParameter (juce::AudioProcessorValueTreeState& state, const char* id, float value)
    {
        auto* p = state.getParameter (id);
        p->beginChangeGesture ();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture ();
    }
    inline juce::dsp::Reverb::Parameters reverbParameters ()
    {
        juce::dsp::Reverb::Parameters p;
        p.roomSize = 0.60f;
        p.damping = 0.45f;
        p.width = 1.0f;
        p.wetLevel = 1.0f / 3.0f;
        p.dryLevel = 0.0f;
        p.freezeMode = 0.0f;
        return p;
    }
    // This is a comment I'm adding here for testn purposes. For content, If you see this by zooming in congrats you win!
}
