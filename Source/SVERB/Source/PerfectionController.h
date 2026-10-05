#pragma once
#include "Parameters.h"

namespace sverb
{
    class PerfectionController : private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
    {
      public:
        explicit PerfectionController (juce::AudioProcessorValueTreeState& s) : state (s)
        {
            state.addParameterListener (params::perfection, this);
        }
        ~PerfectionController () override
        {
            state.removeParameterListener (params::perfection, this);
            cancelPendingUpdate ();
        }
        void cycle ()
        {
            setParameter (
                state,
                params::perfection,
                static_cast<float> (
                    (static_cast<int> (state.getRawParameterValue (params::perfection)->load ()) +
                     1) %
                    3));
            handleUpdateNowIfNeeded ();
        }
        // Message thread only: discard the preset snapshot before restoring defaults,
        // so a queued Perfection callback cannot restore the old custom settings.
        void resetToDefaults ()
        {
            jassert (juce::MessageManager::getInstance ()->isThisTheMessageThread ());
            cancelPendingUpdate ();
            saved.reset ();
            lastApplied = 0;
            restoredIndex.store (-1);
            for (const auto* id : {params::perfection,
                                   params::speed,
                                   params::reverbWet,
                                   params::gain,
                                   params::reverbOn,
                                   params::hz432})
            {
                auto* parameter = state.getParameter (id);
                setParameter (
                    state, id, parameter->convertFrom0to1 (parameter->getDefaultValue ()));
            }
            handleUpdateNowIfNeeded ();
        }
        // Can be called by a host state-restore thread. All snapshot mutations remain on the message thread.
        void beginStateRestore ()
        {
            restoring.store (true);
        }
        void syncAfterStateRestore ()
        {
            restoredIndex.store (
                static_cast<int> (state.getRawParameterValue (params::perfection)->load ()));
            restoring.store (false);
            triggerAsyncUpdate ();
        }

      private:
        void parameterChanged (const juce::String&, float) override
        {
            triggerAsyncUpdate ();
        }
        void handleAsyncUpdate () override
        {
            if (restoring.load ())
                return;
            const auto restored = restoredIndex.exchange (-1);
            if (restored >= 0)
            {
                lastApplied = restored;
                saved.reset ();
            }
            const int next =
                static_cast<int> (state.getRawParameterValue (params::perfection)->load ());
            if (next == lastApplied)
                return;
            Snapshot values;
            if (next == 0)
            {
                values = saved.value_or (Snapshot{});
                saved.reset ();
            }
            else
            {
                if (! saved)
                    saved = lastApplied == 0 ? snapshot (state) : Snapshot{};
                values = next == 1 ? Snapshot{0.91f, 0.36f, 0.6f, true, true}
                                   : Snapshot{1.16f, 0.36f, -1.5f, true, true};
            }
            lastApplied = next;
            setParameter (state, params::speed, values.speed);
            setParameter (state, params::reverbWet, values.wet);
            setParameter (state, params::gain, values.gainDb);
            setParameter (state, params::reverbOn, values.reverbOn ? 1.0f : 0.0f);
            setParameter (state, params::hz432, values.hz432 ? 1.0f : 0.0f);
        }
        juce::AudioProcessorValueTreeState& state;
        int lastApplied = 0;
        std::optional<Snapshot> saved;
        std::atomic<int> restoredIndex{-1};
        std::atomic<bool> restoring{false};
    };
}
