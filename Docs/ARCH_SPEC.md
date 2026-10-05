# ARCH_SPEC.md — SVERB Native Port: Technical Design Document

| Field | Value |
|---|---|
| Source prototype | `Prototypes/SVERB_Audio_FX_Plugin_Prototype.html` |
| Target | JUCE 9.0 Audio Plug-In template, **Standalone** format (`JucePlugin_Build_Standalone = 1`) |
| Language | C++17 minimum (C++20 allowed) |
| Files to implement | `Source/SVERB/Source/PluginProcessor.h/.cpp`, `Source/SVERB/Source/PluginEditor.h/.cpp`, plus new files listed in §0.2 |
| Class names (keep template names) | `SVERBAudioProcessor`, `SVERBAudioProcessorEditor` |
| Modules already enabled | juce_audio_basics, juce_audio_devices, juce_audio_formats, juce_audio_plugin_client, juce_audio_processors, juce_audio_processors_headless, juce_audio_utils, juce_core, juce_data_structures, juce_dsp, juce_events, juce_graphics, juce_gui_basics, juce_gui_extra |

This document is normative. Where it says **MUST**, the execution agent implements exactly that. Where the prototype has a bug, this spec says so and defines the corrected behaviour (§7).

---

## 0. Global Architecture

### 0.1 Signal flow

The prototype's Web Audio graph:

```
AudioBufferSourceNode(playbackRate) ─┬─> dryGainNode(1-wet) ───────────────┐
                                     └─> ConvolverNode(IR) ─> wetGainNode(wet) ┴─> gainNode(sum) ─> masterGainNode(dB) ─> destination
```

Native equivalent inside `SVERBAudioProcessor::processBlock`:

```
[Transport/VarispeedPlayer]  ← in-memory juce::AudioBuffer<float> (decoded file)
        │  writes stereo into the host output buffer (input is ignored)
        ▼
[juce::dsp::DryWetMixer<float>::pushDrySamples]   (captures dry copy)
        ▼
[juce::dsp::Reverb]  (100% wet, dryLevel = 0)
        ▼
[juce::dsp::DryWetMixer<float>::mixWetSamples]    (linear crossfade = effective wet)
        ▼
[juce::dsp::Gain<float>]  (master output gain, dB, 50 ms ramp)
        ▼
     output
```

### 0.2 New source files (add to the Projucer "Source" group, or `#include` from the existing translation units)

| File | Contents |
|---|---|
| `Source/SVERB/Source/Parameters.h` | Parameter ID constants, `createParameterLayout()`, speed range lambdas, perfection preset table (§2) |
| `Source/SVERB/Source/VarispeedPlayer.h/.cpp` | Audio-thread transport + fractional-position Hermite reader (§1.2) |
| `Source/SVERB/Source/AudioFileLoader.h/.cpp` | Background decode + validation + lock-guarded handoff (§1.1) |
| `Source/SVERB/Source/PerfectionController.h/.cpp` | Message-thread preset snapshot/apply/restore logic (§2.4) |
| `Source/SVERB/Source/OfflineExporter.h/.cpp` | Background offline render to 16-bit WAV (§1.6) |
| `Source/SVERB/Source/SverbLookAndFeel.h/.cpp` | Custom knob/LED/toggle drawing (§3.4) |

If adding files to the `.jucer` is undesirable, all of these MAY be header-only and included from `PluginProcessor.h` / `PluginEditor.h`. Behaviour is identical either way.

### 0.3 Threads

| Thread | Owns / does |
|---|---|
| **Audio thread** (`processBlock`) | Reads parameters via cached `std::atomic<float>*`; renders playback; runs DSP. No allocation, no locks that can block (only `SpinLock::ScopedTryLockType`), no file I/O, no `String` building. |
| **Message thread** | Editor, `FileChooser`, APVTS writes from UI, `PerfectionController`, status text, buffer swap (O(1) under spin lock), freeing old buffers. |
| **Loader thread** (`juce::ThreadPool`, 1 thread, owned by processor) | Opens/decodes audio files. |
| **Export thread** (same `ThreadPool`) | Offline render + WAV write. |

Cross-thread primitives used: `std::atomic<bool/int/double/float>`, `juce::SpinLock`, `juce::MessageManager::callAsync`, `juce::AsyncUpdater`, `juce::Timer` (editor, 30 Hz).

### 0.4 Bus layout (MUST)

The app is a file player; host/device input is not used. Replace the template's `BusesProperties` in the constructor:

```cpp
SVERBAudioProcessor::SVERBAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SVERB_STATE", sverb::params::createParameterLayout())
```

`isBusesLayoutSupported`: return `true` only if main output is mono or stereo and main input is disabled (`layouts.getMainInputChannelSet().isDisabled()`).

`acceptsMidi()` / `producesMidi()` / `isMidiEffect()` → `false`. `getTailLengthSeconds()` → `3.0`.

---

## 1. Audio Engine Mapping

### 1.1 File loading — `juce::AudioFormatManager` + `juce::AudioBuffer<float>`

**Prototype behaviour** (`loadAudio`, L613–702): rejects files > 150 MiB; estimates decoded size as `duration × ctxSampleRate × 2ch × 4 bytes` and rejects > 1.0 GiB; decodes whole file into an `AudioBuffer`; on success stops playback and enables Play/Stop/Loop/Export; on failure shows "Audio Error" and disables them.

**Native design:**

```cpp
// AudioFileLoader.h
namespace sverb
{
    struct LoadedAudio
    {
        juce::AudioBuffer<float> buffer;   // numChannels = min(fileChannels, 2), full length
        double sampleRate = 44100.0;       // file's native rate (NOT the device rate)
        juce::File sourceFile;
    };

    constexpr juce::int64 kMaxUploadBytes  = 150LL * 1024 * 1024;          // 150 MiB
    constexpr juce::int64 kMaxDecodedBytes = 1LL * 1024 * 1024 * 1024;     // 1.0 GiB
}
```

- `SVERBAudioProcessor` owns **one** `juce::AudioFormatManager formatManager;` and calls `formatManager.registerBasicFormats();` in its constructor.
- **MP3 support:** already available. JUCE 9 defaults `JUCE_USE_MP3AUDIOFORMAT` to `1` and the SVERB project does not override it, so `registerBasicFormats()` covers MP3, WAV, AIFF, FLAC and Ogg. No `.jucer` changes are needed.
- `FileChooser` wildcard: `"*.wav;*.mp3;*.aif;*.aiff;*.flac;*.ogg"`.

**Load sequence (`SVERBAudioProcessor::loadFileAsync (juce::File)`, called on message thread):**

1. If `file.getSize() > kMaxUploadBytes` → post error `("File Too Large", "Please choose a file smaller than 150 MB.")`, return.
2. Set `loaderState = LoaderState::Loading` (atomic int), status "Decoding audio...". Submit a job to `threadPool`.
3. **On loader thread:**
   - `std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));` — null → error `("Audio Error", "Could not decode the audio file. Please ensure it is a valid MP3 or WAV file.")`.
   - Exact decoded size (replaces prototype's estimate): `bytes = reader->lengthInSamples * juce::jmin ((int) reader->numChannels, 2) * sizeof (float)`. If `> kMaxDecodedBytes` → error `("File Too Large", "Decoded audio would require approx X.XX GiB memory which exceeds the allowed 1.0 GiB. Please choose a shorter file or reduce size.")`.
   - `auto data = std::make_unique<LoadedAudio>();` `data->buffer.setSize (numCh, (int) reader->lengthInSamples);` `reader->read (&data->buffer, 0, (int) reader->lengthInSamples, 0, true, numCh > 1);`
   - `data->sampleRate = reader->sampleRate; data->sourceFile = file;`
   - `juce::MessageManager::callAsync ([safeThis, d = std::move (data)]() mutable { safeThis->installLoadedAudio (std::move (d)); })` — wrap `this` in a `juce::WeakReference` or check a `std::atomic<bool> shuttingDown`; capture-by-move requires `std::shared_ptr` if the lambda must be copyable — use `std::shared_ptr<LoadedAudio>` for the hop and convert to `unique_ptr` on arrival, or keep `shared_ptr` throughout (it is never copied on the audio thread).
4. **`installLoadedAudio` (message thread):**
   ```cpp
   transport.requestStop();                       // see §1.2
   {
       const juce::SpinLock::ScopedLockType sl (audioLock);
       std::swap (currentAudio, incoming);        // O(1) pointer swap only
       transport.resetPositionUnsafe();           // readPos = 0; legal: audio thread is excluded by lock
   }
   incoming.reset();                              // old buffer freed HERE, on message thread, outside lock
   loaderState = LoaderState::Ready;
   sendChangeMessage();                           // processor is a juce::ChangeBroadcaster; editor listens
   ```
5. Audio thread access pattern (in `processBlock`):
   ```cpp
   const juce::SpinLock::ScopedTryLockType sl (audioLock);
   if (! sl.isLocked() || currentAudio == nullptr) { buffer.clear(); return; }  // one silent block max
   ```
   The audio thread never frees, allocates or resizes `currentAudio`.

Error reporting from the loader thread goes via `callAsync` to a processor method that stores `lastError` (title + message strings) and calls `sendChangeMessage()`; the editor shows it with `juce::AlertWindow::showAsync` (`MessageBoxIconType::WarningIcon`, button "OK"). On load error: `currentAudio` is left unchanged if one existed, otherwise transport controls stay disabled; load-button text resets to "Load Audio File".

Load-button text on success: first 20 characters of `file.getFileName()` followed by `"..."` (prototype L633). Status on success: `"Audio loaded and ready."`.

### 1.2 Speed / playback-rate mechanics — fractional interpolation (varispeed), NOT time-stretch

**Prototype semantics:** `AudioBufferSourceNode.playbackRate` = classic **varispeed**: pitch and tempo change together (rate 2.0 = one octave up, double tempo). There is no time-stretching/pitch preservation. The native port MUST reproduce varispeed.

**Decision:** Use **direct fractional-position reading with 4-point, 3rd-order Hermite interpolation** from the in-memory buffer. Do NOT use `juce::ResamplingAudioSource` / `juce::LagrangeInterpolator` (they are stream-oriented, stateful, and make loop wrap + seamless rate changes awkward). Do NOT pre-resample the whole file (would require re-rendering on every knob move).

Rationale over the prototype: the prototype restarts the source node on every speed change (`setSpeed` → `pausePlayback(); startPlayback();`), causing clicks and drift. Native reads a continuously-advancing `double readPos`, so rate changes are glitch-free and smoothed.

**`VarispeedPlayer` state (audio-thread owned unless marked atomic):**

```cpp
class VarispeedPlayer
{
public:
    void prepare (double deviceSampleRate, int maxBlockSize);
    // Message-thread API:
    void play();                    // playing = true
    void pause();                   // playing = false (position retained)
    void requestStop();             // playing = false; stopPending = true (audio thread zeroes position)
    void setLooping (bool);         // looping = b
    bool isPlaying() const;
    double getPositionSeconds() const;          // for UI/status
    bool consumeFinishedFlag();                 // exchange(false) — editor polls
    void resetPositionUnsafe();                 // only under audioLock from message thread

    // Audio thread:
    void render (const LoadedAudio& src, juce::AudioBuffer<float>& out, int numSamples, double effectiveRate);

private:
    std::atomic<bool> playing { false }, looping { false }, stopPending { false }, finished { false };
    std::atomic<double> positionSeconds { 0.0 };
    double readPos = 0.0;                                        // in SOURCE samples
    double deviceRate = 44100.0;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> rateSmoother; // 20 ms
    juce::SmoothedValue<float> fadeGain;                         // 0..1, 10 ms, click-free start/pause/stop
};
```

**Per-block algorithm (`render`):**

1. If `stopPending.exchange(false)` → begin fade-out (`fadeGain.setTargetValue(0)`), and mark that position must reset to 0 when fade reaches 0.
2. `fadeGain.setTargetValue (playing ? 1.0f : 0.0f)`. If `!playing && !fadeGain.isSmoothing() && fadeGain.getCurrentValue() == 0` → clear output, return (paused/stopped, `readPos` frozen).
3. `rateSmoother.setTargetValue (effectiveRate * (src.sampleRate / deviceRate))` — the `src.sampleRate / deviceRate` term corrects file-vs-device sample-rate mismatch (Web Audio did this implicitly by decoding to the context rate).
4. For each output sample `n`:
   ```cpp
   const double inc = rateSmoother.getNextValue();
   const float  g   = fadeGain.getNextValue();
   for (int ch = 0; ch < outChannels; ++ch)
   {
       const int srcCh = juce::jmin (ch, src.buffer.getNumChannels() - 1); // mono → both channels
       out.setSample (ch, n, g * hermiteRead (src.buffer, srcCh, readPos, looping));
   }
   readPos += inc;
   if (readPos >= len)
   {
       if (looping) readPos -= len;                 // seamless wrap (prototype: sourceNode.loop = true)
       else { zero remaining samples; playing = false; readPos = 0; finished = true; break; }
   }
   ```
5. `positionSeconds = readPos / src.sampleRate`.

**Hermite reader (exact formula):**

```cpp
static inline float hermiteRead (const juce::AudioBuffer<float>& b, int ch, double pos, bool wrap)
{
    const int    len = b.getNumSamples();
    const int    i   = (int) pos;
    const float  t   = (float) (pos - (double) i);
    auto at = [&] (int k) -> float
    {
        if (wrap) { k %= len; if (k < 0) k += len; return b.getSample (ch, k); }
        return (k < 0 || k >= len) ? 0.0f : b.getSample (ch, k);
    };
    const float x0 = at (i - 1), x1 = at (i), x2 = at (i + 1), x3 = at (i + 2);
    const float c0 = x1;
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}
```

(Use `getReadPointer(ch)` cached per block for speed; the lambda form above is the reference semantics.)

Known limitation (accepted for v1, matches prototype quality): at rates > 1 no anti-alias low-pass is applied before decimation.

**Transport → UI mapping:**

| Prototype function | Native call | Status text (exact strings) |
|---|---|---|
| `startPlayback()` | `transport.play()` | `"Playing... (Speed: 1.00x)"`; append `" / 432Hz Mode"` inside the parens when hz432 on; append `" [LOOP]"` after the `)` when looping |
| `pausePlayback()` | `transport.pause()` | `"Paused. (Offset: 12.34s)"` (`positionSeconds`, 2 dp) |
| `stopPlayback()` | `transport.requestStop()` | `"Audio loaded and ready."` or `"Ready to load audio."` if nothing loaded |
| `onended` (non-loop) | editor `Timer` sees `consumeFinishedFlag() == true` | `"Playback finished. Ready to restart."`, Play button shows play state |
| `toggleLoop()` | `transport.setLooping(b)` (NO restart needed) | refresh "Playing..." string if playing |

Speed/432 changes while playing do NOT restart playback; the editor simply refreshes the "Playing..." status string.

### 1.3 432 Hz mode — pitch offset math

Prototype: `PITCH_SHIFT_RATIO = 432 / 440`, applied multiplicatively to the playback rate (`getEffectivePlaybackRate`, L737–744). This is a varispeed detune (tempo also drops by the same ratio), **not** a pitch-preserving shift. Reproduce exactly.

```cpp
namespace sverb
{
    constexpr double kPitch432Ratio = 432.0 / 440.0;   // 0.98181818...
    // In cents: 1200 * log2(432/440) = -31.7667 cents  (≈ -1.82 % rate)

    inline double effectiveRate (double speed, bool hz432)
    {
        return speed * (hz432 ? kPitch432Ratio : 1.0);
    }
}
```

Effective rate range: `[0.5 × 0.981818, 2.0] = [0.490909, 2.0]`. Resulting increment per output sample = `effectiveRate × fileSR / deviceSR` (§1.2 step 3). The rate smoother (multiplicative, 20 ms) makes the 432 toggle a smooth glide rather than a click.

### 1.4 Reverb DSP — wetness → `juce::dsp::Reverb`

**Prototype:** `ConvolverNode` with a synthetic stereo IR (1.35 s decaying white noise, envelope `exp(-t/0.5)`, R channel ×0.8, overall ×0.65). Mix is a linear crossfade: `dry = 1 - wet`, `wet = wet`, 50 ms ramps. Reverb OFF ⇒ `dry = 1, wet = 0`.

**Native mapping (MUST):** run `juce::dsp::Reverb` **100 % wet** and perform the crossfade with `juce::dsp::DryWetMixer<float>`, which reproduces the prototype's linear law and handles latency-free dry capture and smoothing.

```cpp
// members
juce::dsp::Reverb reverb;
juce::dsp::DryWetMixer<float> mixer { 0 };   // maximumWetLatencyInSamples = 0

// prepareToPlay
juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) getTotalNumOutputChannels() };
reverb.prepare (spec);
mixer.prepare (spec);
mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);   // dry = 1-w, wet = w  (prototype law)
mixer.setWetMixProportion (0.5f);

juce::dsp::Reverb::Parameters rp;
rp.roomSize   = 0.60f;   // tuned to approximate the 1.35 s / tau 0.5 s noise IR
rp.damping    = 0.45f;
rp.width      = 1.00f;
rp.wetLevel   = 1.0f / 3.0f;  // JUCE internally multiplies wet by 3.0 → unity wet gain
rp.dryLevel   = 0.0f;         // dry is handled by DryWetMixer, never by Reverb
rp.freezeMode = 0.0f;
reverb.setParameters (rp);
```

Note on JUCE internals (verified in `juce_Reverb.h`, JUCE 9.0): `setParameters` applies `wetScaleFactor = 3.0` and `dryScaleFactor = 2.0`, so `wetLevel = 1/3` with `width = 1` yields a wet gain of exactly 1.0. `roomSize`/`damping` are fixed constants (not exposed as parameters in v1); `roomSize` and `damping` are tuning values and MAY be adjusted by ear without changing any interface.

**Per-block reverb logic:**

```cpp
const bool  reverbOn = reverbOnParam->load() >= 0.5f;
const float wet      = reverbWetParam->load();               // 0..1
mixer.setWetMixProportion (reverbOn ? wet : 0.0f);           // OFF ⇒ dry 1.0 / wet 0.0 (prototype toggleReverb)

juce::dsp::AudioBlock<float> block (buffer);
mixer.pushDrySamples (block);
reverb.process (juce::dsp::ProcessContextReplacing<float> (block));
mixer.mixWetSamples (block);
```

The reverb keeps running while OFF so re-enabling has no warm-up artefact and tails decay naturally through the 50 ms mixer ramp. `DryWetMixer` smooths internally (`SmoothedValue`, default 50 ms) — matches the prototype's `linearRampToValueAtTime(..., now + 0.05)`.

Mono output layout: `juce::dsp::Reverb::process` handles 1 or 2 channels automatically.

### 1.5 Output master gain

**Prototype:** `masterGainNode.gain = 10^(dB/20)`, dB range −6…+6, 50 ms linear ramp (`setGain`, L907–925).

```cpp
juce::dsp::Gain<float> masterGain;

// prepareToPlay
masterGain.prepare (spec);
masterGain.setRampDurationSeconds (0.05);
masterGain.setGainDecibels (0.0f);

// processBlock (after mixer)
masterGain.setGainDecibels (gainParam->load());   // dsp::Gain converts with Decibels::decibelsToGain
masterGain.process (juce::dsp::ProcessContextReplacing<float> (block));
```

### 1.6 Offline export ("Export Processed Audio (WAV)")

Prototype (`exportAudio`, L1508–1627): refuses while playing; renders source × effectiveRate through the same graph at the **file's** sample rate; length = `duration/effectiveRate + IR length`; writes 16-bit PCM WAV; filename `SVERB_SPEED{speed:0.00}x[_432Hz]_{originalBaseName}.wav`.

**Native design (`OfflineExporter`):**

1. Editor: if `transport.isPlaying()` → AlertWindow `("Playback Active", "Please stop or pause playback before starting the export process.")`, return.
2. Snapshot params on message thread into a POD: `{ speed, hz432, reverbOn, wet, gainDb }` and grab a `std::shared_ptr<const LoadedAudio>` (copy the shared pointer under `audioLock`).
3. `juce::FileChooser` (`launchAsync`, `saveMode | canSelectFiles | warnAboutOverwriting`), default file = source file's directory + generated filename. Abort if cancelled.
4. Disable Export button; status `"Rendering audio effects... Please wait."`; submit job to `threadPool`.
5. **Export thread** builds its OWN `VarispeedPlayer`, `dsp::Reverb`, `DryWetMixer`, `dsp::Gain` (never touches the realtime instances), prepared at `src.sampleRate` with block size 4096, same reverb parameters, with smoothers `setCurrentAndTargetValue` to the snapshot (no ramps at t=0). Render length in output samples: `ceil (src.numSamples / effectiveRate) + ceil (3.0 * src.sampleRate)` (3 s reverb tail; replaces IR length). Loop is forced OFF for export.
6. Write with `juce::WavAudioFormat`:
   ```cpp
   std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (dest);   // after dest.deleteFile()
   auto writer = juce::WavAudioFormat().createWriterFor (os,
                     juce::AudioFormatWriterOptions{}.withSampleRate (src.sampleRate)
                                                     .withNumChannels (numOutChannels)
                                                     .withBitsPerSample (16));
   writer->writeFromAudioSampleBuffer (block, 0, n);   // per 4096-sample block
   ```
   Output channels = `src.buffer.getNumChannels()` (1 or 2), matching prototype's `OfflineAudioContext(numberOfChannels, ...)`.
7. Completion via `callAsync`: status `"Export complete! File saved."` or AlertWindow `("Export Failed", "An error occurred during the audio rendering process: <msg>")` + status `"Export failed."`; re-enable Export.

Filename generation (exact):
```cpp
juce::String name = "SVERB_SPEED" + juce::String (speed, 2) + "x";
if (hz432) name << "_432Hz";
name << "_" << (src ? src->sourceFile.getFileNameWithoutExtension() : juce::String ("untitled")) << ".wav";
```

### 1.7 Full `processBlock` (reference order)

```cpp
void SVERBAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    {
        const juce::SpinLock::ScopedTryLockType sl (audioLock);
        if (! sl.isLocked() || currentAudio == nullptr) { buffer.clear(); /* still run DSP below so tails ring out */ }
        else
        {
            const double rate = sverb::effectiveRate (speedParam->load(), hz432Param->load() >= 0.5f);
            transport.render (*currentAudio, buffer, numSamples, rate);   // overwrites all output channels
        }
    }

    juce::dsp::AudioBlock<float> block (buffer);
    const bool reverbOn = reverbOnParam->load() >= 0.5f;
    mixer.setWetMixProportion (reverbOn ? reverbWetParam->load() : 0.0f);
    mixer.pushDrySamples (block);
    reverb.process (juce::dsp::ProcessContextReplacing<float> (block));
    mixer.mixWetSamples (block);

    masterGain.setGainDecibels (gainParam->load());
    masterGain.process (juce::dsp::ProcessContextReplacing<float> (block));
}
```

Cached raw parameter pointers (set once in constructor):
```cpp
std::atomic<float>* speedParam     = apvts.getRawParameterValue (sverb::params::speed);
std::atomic<float>* reverbOnParam  = apvts.getRawParameterValue (sverb::params::reverbOn);
std::atomic<float>* reverbWetParam = apvts.getRawParameterValue (sverb::params::reverbWet);
std::atomic<float>* gainParam      = apvts.getRawParameterValue (sverb::params::gain);
std::atomic<float>* hz432Param     = apvts.getRawParameterValue (sverb::params::hz432);
// perfection is NOT read on the audio thread
```

`prepareToPlay`: `transport.prepare(sr, block)`, then the reverb/mixer/gain prepares from §1.4–1.5. `releaseResources`: `reverb.reset(); mixer.reset(); masterGain.reset();`.

---

## 2. Parameter Architecture (APVTS)

### 2.1 Member

```cpp
// PluginProcessor.h (public, so the editor can attach)
juce::AudioProcessorValueTreeState apvts;
```

State tree type ID: `"SVERB_STATE"`. Undo manager: `nullptr`.

### 2.2 Parameter table (exact)

All IDs use version hint `1`: `juce::ParameterID { id, 1 }`.

| Constant (`sverb::params::`) | ID string | Name | Class | Range / choices | Default | Display (`stringFromValue`) | Prototype source |
|---|---|---|---|---|---|---|---|
| `speed` | `"speed"` | `"Speed"` | `AudioParameterFloat` | 0.5 … 2.0, **custom skew** (§2.3) | `1.0` | `String(v, 2) + "x"` → `"1.00x"` | `#speedKnob` min/max/initial |
| `reverbOn` | `"reverb_on"` | `"Reverb On"` | `AudioParameterBool` | off/on | `true` | `"ON"` / `"OFF"` | `#reverbToggle checked` |
| `reverbWet` | `"reverb_wet"` | `"Reverb Wetness"` | `AudioParameterFloat` | 0.0 … 1.0, linear, interval 0.0 (continuous) | `0.5` | `String(roundToInt(v*100)) + "%"` → `"50%"` | `#reverbKnob`, `INITIAL_WETNESS` |
| `gain` | `"gain"` | `"Gain"` | `AudioParameterFloat` | −6.0 … +6.0 dB, linear, interval 0.0 | `0.0` | `(v > 0 ? "+" : "") + String(v, 1) + "dB"` → `"+0.6dB"`, `"0.0dB"` | `#gainKnob`, `INITIAL_GAIN` |
| `hz432` | `"hz432"` | `"432Hz Mode"` | `AudioParameterBool` | off/on | `false` | `"ON"` / `"OFF"` | `is432HzMode` |
| `perfection` | `"perfection"` | `"Perfection"` | `AudioParameterChoice` | `{"Off", "Slow", "Fast"}` (index 0,1,2) | `0` (Off) | choice name | `perfectionState` |

`valueFromString` for each float: parse the first numeric token (`text.retainCharacters ("-0123456789.").getFloatValue()`); for `reverbWet` divide by 100. Out-of-range input is clamped by the parameter (prototype showed a "Value Clamped" box — v1 silently clamps; optional).

Loop state is **not** a parameter (transport-only, `std::atomic<bool>` in `VarispeedPlayer`), matching the prototype where loop is UI/transport state.

### 2.3 Speed non-linear range (exact port of `getSpeedValueFromNormalizedPosition` + its inverse)

The knob is linear in *position* N∈[0,1], centre N=0.5 ⇒ 1.00x, with a square-root curve on each half. Encode this in the `NormalisableRange` so slider position, host automation and the parameter's normalised value all agree:

```cpp
inline juce::NormalisableRange<float> makeSpeedRange()
{
    auto from0to1 = [] (float, float, float n)        // normalised → value
    {
        n = juce::jlimit (0.0f, 1.0f, n);
        if (n <= 0.5f) return 0.5f + 0.5f * (1.0f - std::sqrt (1.0f - n / 0.5f));   // 0.5x … 1.0x
        return 1.0f + 1.0f * std::sqrt ((n - 0.5f) / 0.5f);                          // 1.0x … 2.0x
    };
    auto to0to1 = [] (float, float, float v)          // value → normalised
    {
        v = juce::jlimit (0.5f, 2.0f, v);
        if (v <= 1.0f) { const float d = (v - 0.5f) / 0.5f; return (1.0f - (1.0f - d) * (1.0f - d)) * 0.5f; }
        const float a = (v - 1.0f) / 1.0f; return 0.5f + a * a * 0.5f;
    };
    auto snap = [] (float, float, float v) { return juce::jlimit (0.5f, 2.0f, v); };
    return { 0.5f, 2.0f, from0to1, to0to1, snap };
}
```

Check values (MUST hold, use as a unit sanity assert in debug): N=0 → 0.5; N=0.5 → 1.0; N=1 → 2.0; v=0.91 → N≈0.4838; v=1.16 → N≈0.5128.

### 2.4 `createParameterLayout()`

```cpp
namespace sverb::params
{
    inline constexpr const char* speed      = "speed";
    inline constexpr const char* reverbOn   = "reverb_on";
    inline constexpr const char* reverbWet  = "reverb_wet";
    inline constexpr const char* gain       = "gain";
    inline constexpr const char* hz432      = "hz432";
    inline constexpr const char* perfection = "perfection";

    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { speed, 1 }, "Speed", makeSpeedRange(), 1.0f,
            AudioParameterFloatAttributes()
                .withLabel ("x")
                .withStringFromValueFunction ([] (float v, int) { return String (v, 2) + "x"; })
                .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("-0123456789.").getFloatValue(); })));

        layout.add (std::make_unique<AudioParameterBool> (ParameterID { reverbOn, 1 }, "Reverb On", true));

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { reverbWet, 1 }, "Reverb Wetness",
            NormalisableRange<float> (0.0f, 1.0f), 0.5f,
            AudioParameterFloatAttributes()
                .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; })
                .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("-0123456789.").getFloatValue() / 100.0f; })));

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { gain, 1 }, "Gain",
            NormalisableRange<float> (-6.0f, 6.0f), 0.0f,
            AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + String (v, 1) + "dB"; })
                .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("-0123456789.").getFloatValue(); })));

        layout.add (std::make_unique<AudioParameterBool> (ParameterID { hz432, 1 }, "432Hz Mode", false));

        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { perfection, 1 }, "Perfection",
            StringArray { "Off", "Slow", "Fast" }, 0));

        return layout;
    }
}
```

### 2.5 Perfection preset logic (`PerfectionController`)

Prototype (`applyPerfection`, L991–1124) semantics, to reproduce exactly:

| Target state | Action |
|---|---|
| Off → Slow/Fast, or Slow ↔ Fast | If no snapshot is held, **save** current `{speed, reverbWet, gain, reverbOn, hz432}` (only once per activation — Slow→Fast keeps the original snapshot). Then apply preset values below. |
| Slow/Fast → Off | **Restore** the snapshot (all five values), then discard it. |

Preset table:

| Preset | speed | reverb_wet | gain (dB) | reverb_on | hz432 |
|---|---|---|---|---|---|
| Slow | 0.91 | 0.36 | +0.6 | true | true |
| Fast | 1.16 | 0.36 | −1.5 | true | true |

Cycle order on button click: Off → Slow → Fast → Off.

**Threading (MUST):** parameters that change other parameters must only do so on the message thread.

```cpp
class PerfectionController : private juce::AudioProcessorValueTreeState::Listener,
                             private juce::AsyncUpdater
{
public:
    explicit PerfectionController (juce::AudioProcessorValueTreeState&);   // addParameterListener (perfection, this)
    ~PerfectionController() override;                                       // removeParameterListener; cancelPendingUpdate
    void cycle();   // message thread: set perfection = (index + 1) % 3 via setValueNotifyingHost
private:
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }  // any thread
    void handleAsyncUpdate() override;   // message thread: compare new index vs lastAppliedIndex, save/apply/restore
    void setParam (const char* id, float plainValue);  // p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(v)); p->endChangeGesture();

    juce::AudioProcessorValueTreeState& apvts;
    int lastAppliedIndex = 0;
    std::optional<Snapshot> saved;
    bool applying = false;   // re-entrancy guard while writing other params
};
```

Owned by the processor (`PerfectionController perfection { apvts };`, declared after `apvts`). On `setStateInformation`: set `lastAppliedIndex` to the restored perfection index and `saved.reset()` **without** applying (state already holds the preset values); if the user later returns to Off with no snapshot, restore to factory defaults (`1.0, 0.5, 0.0 dB, true, false`).

Manual knob moves while a preset is active do NOT exit the preset (prototype behaviour).

### 2.6 State persistence

```cpp
void getStateInformation (juce::MemoryBlock& dest) override
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}
void setStateInformation (const void* data, int size) override
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    perfection.syncAfterStateRestore();
}
```

The loaded audio file is NOT persisted in v1.

---

## 3. UI Layout Map

### 3.1 Window

- `setSize (550, 730); setResizable (false, false);` (prototype `.plugin-body` is fixed 550 px wide).
- Editor inherits `juce::AudioProcessorEditor, private juce::Timer, private juce::ChangeListener`.
- `startTimerHz (30)` — polls `transport.consumeFinishedFlag()`, refreshes status/transport button states.
- `processor.addChangeListener (this)` for load-complete / error / export-complete notifications; remove in destructor.
- Editor owns `SverbLookAndFeel lnf;` and calls `setLookAndFeel (&lnf)` in constructor, `setLookAndFeel (nullptr)` in destructor (before members are destroyed).

### 3.2 Element mapping (HTML → JUCE)

| HTML element (id) | JUCE member | Type | Configuration | Binding |
|---|---|---|---|---|
| `h1` "SVERB" | `titleLabel` | `juce::Label` | text "SVERB", font 30 px bold, letter-spaced (kerning 0.3), centred, white | — |
| `p` subtitle | `subtitleLabel` | `juce::Label` | "Speed & Reverb Audio Effect", 13 px, `#9ca3af`, centred | — |
| `#audioFile` + `label.file-input-label` / `#loadText` | `loadButton` | `juce::TextButton` | text "Load Audio File"; yellow outline style (§3.4) | `onClick` → `FileChooser::launchAsync` → `processor.loadFileAsync(file)` |
| `#loopButton` | `loopButton` | `juce::TextButton` | text "LOOP", `setClickingTogglesState (true)`, disabled until audio loaded; toggled-on colour `#ffcc00` bg / `#111111` text | `onClick` → `transport.setLooping (loopButton.getToggleState())` |
| `#playButton` (`#playIcon`/`#pauseIcon`) | `playButton` | `juce::TextButton` | text "PLAY" ↔ "PAUSE" by transport state; base colour `#16a34a`; disabled until loaded | `onClick` → play/pause |
| `#stopButton` | `stopButton` | `juce::TextButton` | text "STOP"; base colour `#dc2626`; disabled until loaded | `onClick` → `transport.requestStop()` |
| `#statusText` (+ "Status:" prefix) | `statusLabel` | `juce::Label` | text `"Status: " + msg`, 13 px, centred; colour `#facc15` | set by editor |
| `#hz432Button` + `.hz-button-light` | `hz432Button` | `juce::ToggleButton` | text "" ; drawn as round LED (§3.4): off `#550000`, on `#ff0000` + glow | `ButtonAttachment` → `"hz432"` |
| `#hz432Status` | `hz432Label` | `juce::Label` | "432Hz" (off, `#9ca3af`) / "432Hz Active" (on, `#f87171`), 11 px | updated from button `onStateChange` |
| `label` "SPEED" | `speedTitle` | `juce::Label` | "SPEED", 18 px, white, centred | — |
| `#speedKnob` + `#speedIndicator` + `#speedValue` | `speedSlider` | `juce::Slider` | `RotaryVerticalDrag`, `TextBoxBelow` (editable, 80×22), component ID `"speed"` | `SliderAttachment` → `"speed"` |
| "0.5x (SLOW)" / "2.0x (FAST)" | `speedMinLabel`, `speedMaxLabel` | `juce::Label` | 11 px semibold; `#f87171` left-justified / `#4ade80` right-justified | — |
| `#perfectionButton` + `#perfectionLight` | `perfectionButton` | `juce::TextButton` | text ""; drawn as round LED: Off `#550000`, Slow `#ffcc00` glow, Fast `#10b981` glow | `onClick` → `processor.perfection.cycle()`; LED colour driven by a `juce::ParameterAttachment` on `"perfection"` (choice params cannot use `ButtonAttachment`) |
| `#perfectionStatus` | `perfectionLabel` | `juce::Label` | "Perfection: OFF" / "Perfection: SLOW" / "Perfection: FAST", 11 px | same `ParameterAttachment` callback |
| `label` "REVERB ON" | `reverbOnTitle` | `juce::Label` | 13 px, white | — |
| `#reverbToggle` (`.toggle-switch`) | `reverbToggle` | `juce::ToggleButton` | drawn as 50×25 pill switch (§3.4): off `#444444`, on `#5D80E5` + glow | `ButtonAttachment` → `"reverb_on"` |
| `label` "WETNESS" | `wetTitle` | `juce::Label` | 13 px, white | — |
| `#reverbKnob` + `#reverbIndicator` + `#reverbValue` | `wetSlider` | `juce::Slider` | `RotaryVerticalDrag`, `TextBoxBelow` (editable, 80×22), component ID `"wet"` | `SliderAttachment` → `"reverb_wet"` |
| `label` "GAIN" | `gainTitle` | `juce::Label` | 13 px, white | — |
| `#gainKnob` + `#gainIndicator` + `#gainValue` | `gainSlider` | `juce::Slider` | `RotaryVerticalDrag`, `TextBoxBelow` (editable, 80×22), component ID `"gain"` | `SliderAttachment` → `"gain"` |
| `#exportButton` | `exportButton` | `juce::TextButton` | "Export Processed Audio (WAV)", 13 px bold, colour `#5D80E5`; disabled until loaded and during export | `onClick` → §1.6 |
| `#message-box` | — | `juce::AlertWindow::showAsync` | WarningIcon, single "OK" button | — |

Attachment members MUST be declared **after** the components they attach to (destroyed first):

```cpp
using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
std::unique_ptr<SliderAtt> speedAtt, wetAtt, gainAtt;
std::unique_ptr<ButtonAtt> hz432Att, reverbOnAtt;
std::unique_ptr<juce::ParameterAttachment> perfectionAtt;
```

### 3.3 Slider behaviour (all three rotaries)

| Prototype behaviour | JUCE configuration |
|---|---|
| 306° sweep, 7:30 → 4:30 (`START_ANGLE = -153`, `MAX_ANGLE_RANGE = 306`) | `setRotaryParameters (juce::degreesToRadians (207.0f), juce::degreesToRadians (513.0f), true);` (−153° expressed as +207° because JUCE asserts angles ≥ 0; 207 + 306 = 513 < 720) |
| Vertical drag, 200 px = full sweep | `setSliderStyle (juce::Slider::RotaryVerticalDrag); setMouseDragSensitivity (200);` |
| Double-click knob resets to initial value | `setDoubleClickReturnValue (true, defaultValue);` defaults: speed 1.0, wet 0.5, gain 0.0 |
| Double-click value to type a number | `setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 22); setTextBoxIsEditable (true);` (text formatting comes from the parameter's string functions via the attachment) |
| Moving the WETNESS knob while reverb is OFF turns reverb ON (`setReverbWetness`, L899–903) | `wetSlider.onDragStart = [this] { if (! reverbToggle.getToggleState()) reverbToggle.setToggleState (true, juce::sendNotificationSync); };` |
| Wetness value shows "OFF" when reverb off (`toggleReverb`, L948) | Cosmetic only. When `reverb_on` is false, set `wetSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xff6b7280))`; restore white when true. Drive this from `reverbToggle.onStateChange`. Do not override the attachment's text formatting. |

### 3.4 `SverbLookAndFeel` (extends `juce::LookAndFeel_V4`)

**Palette (exact hex, from prototype CSS `:root`):**

| Token | Hex | Use |
|---|---|---|
| `pluginBg` | `#282828` | editor background |
| `panelBg` | `#1e1e1e` | main control panel fill |
| `border` | `#333333` | panel/editor 1 px border |
| `fontColour` | `#e0e0e0` | default label text |
| `highlightYellow` | `#ffcc00` | load button outline/text, loop-on, perfection Slow |
| `divider` | `#ffffff` @ 10 % alpha | horizontal rules |
| `knobBodyTop/Mid/Bottom` | `#444444` / `#777777` / `#555555` | knob body linear gradient (145°, stops 0 / 0.4 / 1.0) |
| `marker` | `#000000` | knob pointer line |
| `buttonBase` | `#4a4a4a` | generic TextButton |

**`drawRotarySlider`** — pick arc stops by `slider.getComponentID()`:

| ID | Ring diameter : body diameter | Arc colour stops (fraction of 306° → colour) | Marker (w × h) |
|---|---|---|---|
| `"speed"` | 160 : 120 (body = 0.75 × ring) | 0.000 `#ff4500`, 0.196 `#ff9900`, 0.392 `#ffcc00`, 0.784 `#00ff00`, 1.000 `#00ff00` | 4 × 20 px |
| `"wet"` | 90 : 60 (body = 0.667 × ring) | 0.000 `#7D26CD`, 0.327 `#5D80E5`, 0.654 `#00CCFF`, 1.000 `#55C48A` | 4 × 10 px |
| `"gain"` | 90 : 60 (body = 0.667 × ring) | 0.000 `#00CC99`, 0.500 `#10b981`, 1.000 `#34d399` | 4 × 10 px |

Drawing steps:
1. Ring: draw the filled portion only (from `rotaryStartAngle` to `rotaryStartAngle + sliderPosProportional × (end − start)`), ring thickness = `(ringDiameter − ringDiameter*0.5)/2` (CSS mask cut at 50 % radius). JUCE has no conic gradient: approximate by drawing the arc as **64 segments**, each a `Path::addCentredArc` stroke coloured by linearly interpolating the stop table at the segment's fraction of the full 306°.
2. Glow: `juce::DropShadow` / `Graphics::setColour(glow.withAlpha(a))` + stroke slightly wider; speed glow `#ffcc00` α = `0.1 + 0.5 × pos`, wet `#5D80E5` α = `0.6 × pos`, gain `#10b981` α = `0.6 × pos`.
3. Body: filled circle with `ColourGradient` (top-left `#444444` → `#777777` at 0.4 → bottom-right `#555555`), plus a 5 px black drop shadow at 60 % alpha.
4. Marker: rounded rect (corner 2), inset 5 px from body top, rotated by the slider angle about the body centre.

**`drawToggleButton`:**
- If `button.getComponentID() == "hz432"`: 60×60 outer circle `#333333`, 2 px border `#555555`; inner 40×40 light: off `#550000`, on `#ff0000` with 15 px glow `#ff0000`.
- If `"reverbToggle"`: 50×25 pill (corner 12.5), off `#444444`, on `#5D80E5` + glow `rgba(93,128,229,0.6)`; white 21 px thumb, left when off / right when on.

**`drawButtonBackground` for `perfectionButton`** (component ID `"perfection"`): same LED geometry as hz432; light colour read from `button.getProperties()["ledColour"]` (set by the perfection `ParameterAttachment` callback): Off `#550000`, Slow `#ffcc00` + glow, Fast `#10b981` + glow.

**`loadButton`** (component ID `"load"`): transparent fill, 1 px `#ffcc00` outline, corner 8, text `#ffcc00` bold; hover fill `#ffcc00` @ 10 %.

**Other TextButtons**: corner 8, fill from `TextButton::buttonColourId`, 4 px darker bottom "lip" (`colour.darker (0.5f)`), disabled alpha 0.5.

### 3.5 `resized()` — exact bounds (pixels, editor-local, window 550 × 730)

Use these literal rectangles (they encode the prototype's Tailwind layout). `juce::Rectangle<int> (x, y, w, h)`:

| Component | x | y | w | h | Notes |
|---|---|---|---|---|---|
| `titleLabel` | 24 | 24 | 502 | 36 | |
| `subtitleLabel` | 24 | 60 | 502 | 18 | |
| `loadButton` | 24 | 94 | 192 | 40 | `w-48` = 192 |
| `loopButton` | 326 | 94 | 56 | 40 | gap 16 |
| `playButton` | 398 | 94 | 56 | 40 | |
| `stopButton` | 470 | 94 | 56 | 40 | right edge 526 |
| `statusLabel` | 24 | 142 | 502 | 20 | |
| *divider 1* | 24 | 174 | 502 | 1 | painted in `paint()` |
| *main panel* | 24 | 190 | 502 | 448 | painted: fill `#1e1e1e`, border `#333333`, corner 12 |
| `hz432Button` | 91 | 292 | 60 | 60 | column 1 centre x = 121, vertically centred on speed knob centre y = 322 |
| `hz432Label` | 66 | 358 | 110 | 16 | |
| `speedTitle` | 195 | 210 | 160 | 24 | column 2 centre x = 275 |
| `speedSlider` | 195 | 242 | 160 | 190 | 160 knob area + 8 gap + 22 text box |
| `speedMinLabel` | 195 | 436 | 80 | 16 | left half under knob |
| `speedMaxLabel` | 275 | 436 | 80 | 16 | right half under knob |
| `perfectionButton` | 399 | 292 | 60 | 60 | column 3 centre x = 429 |
| `perfectionLabel` | 374 | 358 | 110 | 16 | |
| *divider 2 (in panel)* | 44 | 468 | 462 | 1 | painted |
| `reverbOnTitle` | 44 | 484 | 154 | 18 | column 1 (x 44–198) |
| `reverbToggle` | 96 | 538 | 50 | 25 | centred in a 90×90 cell at (76, 506) |
| `wetTitle` | 198 | 484 | 154 | 18 | column 2 (x 198–352) |
| `wetSlider` | 230 | 506 | 90 | 112 | 90 knob + 22 text box |
| `gainTitle` | 352 | 484 | 154 | 18 | column 3 (x 352–506) |
| `gainSlider` | 384 | 506 | 90 | 112 | |
| *divider 3* | 24 | 654 | 502 | 1 | painted |
| `exportButton` | 155 | 670 | 240 | 36 | centred; bottom margin 24 |

`paint()`: fill `#282828`, 1 px `#333333` border around the full editor, the three dividers, and the main panel (rounded rect, corner 12, fill `#1e1e1e`, 1 px `#333333` border, inner shadow optional).

### 3.6 Control enable/disable rules (from `enableControls` / `disableControls`)

| Condition | play | stop | loop | export |
|---|---|---|---|---|
| No audio loaded / load failed | off | off | off | off |
| Audio loaded | on | on | on | on |
| Export in progress | on | on | on | **off** |

Knobs, 432, Perfection and Reverb toggle are always enabled (prototype allows tweaking before load).

---

## 4. Processor public interface (for the editor)

```cpp
class SVERBAudioProcessor : public juce::AudioProcessor, public juce::ChangeBroadcaster
{
public:
    juce::AudioProcessorValueTreeState apvts;
    sverb::PerfectionController perfection;      // declared after apvts
    sverb::VarispeedPlayer transport;

    void loadFileAsync (const juce::File&);
    void exportAsync (const juce::File& destination);       // snapshots params internally
    bool hasAudio() const;                                  // message thread
    bool isExporting() const;                               // atomic
    juce::String getLoadedFileName() const;                 // message thread
    std::optional<std::pair<juce::String, juce::String>> takePendingError();   // title, message
    juce::String takePendingStatus();                       // e.g. "Export complete! File saved."
    // ...template overrides...
private:
    juce::AudioFormatManager formatManager;
    juce::ThreadPool threadPool { 1 };
    juce::SpinLock audioLock;
    std::shared_ptr<sverb::LoadedAudio> currentAudio;       // guarded by audioLock
    juce::dsp::Reverb reverb;
    juce::dsp::DryWetMixer<float> mixer { 0 };
    juce::dsp::Gain<float> masterGain;
    std::atomic<float>* speedParam {}, * reverbOnParam {}, * reverbWetParam {}, * gainParam {}, * hz432Param {};
};
```

Destructor: `threadPool.removeAllJobs (true, 5000);` before members are destroyed.

---

## 5. Acceptance checklist (execution agent self-test)

1. Builds the `SVERB_StandalonePlugin` target in Debug and Release with zero errors.
2. Launching shows a 550 × 730 non-resizable window laid out per §3.5.
3. Loading a WAV and an MP3 both succeed; >150 MB file is rejected with the specified message.
4. Play/Pause/Stop/Loop behave per §1.2; turning Speed while playing changes pitch+tempo smoothly with no clicks or restarts.
5. Speed knob centre = 1.00x; full CCW = 0.50x; full CW = 2.00x; typing `0.91` positions the knob at ≈ 48.4 % of travel.
6. 432 toggle lowers pitch by ≈ 31.8 cents smoothly.
7. Wetness 0 % = dry only; 100 % = reverb only; Reverb OFF = dry only; moving Wetness while OFF switches Reverb ON.
8. Gain −6…+6 dB, display `"+0.6dB"` style.
9. Perfection cycles Off→Slow→Fast→Off; Slow sets 0.91x / 36 % / +0.6 dB / Reverb ON / 432 ON; returning to Off restores the pre-preset values.
10. Export produces `SVERB_SPEED0.91x_432Hz_<name>.wav`, 16-bit, at the file's sample rate, length = source/rate + 3 s.
11. No allocations, locks (other than try-lock) or logging in `processBlock`.

---

## 6. Out of scope for v1

Waveform/scrub timeline, time-stretch (pitch-independent speed), user IR loading, resizable UI, persisting the loaded file, Rive/video features (see `Docs/FUTURE_SPEC.md`).

---

## 7. Prototype defects — corrected behaviour (do NOT port these)

| # | Prototype location | Defect | Native behaviour |
|---|---|---|---|
| 1 | `setSpeed`, `toggle432Hz`, `toggleLoop` | Restart `AudioBufferSourceNode` on every change → clicks, offset drift | Continuous `readPos`, smoothed rate (§1.2) |
| 2 | `startPlayback` L775 | Compares `startOffset` (source seconds) with `duration / effectiveRate` (output seconds) — unit mismatch | Wrap/stop is decided on `readPos` in source samples |
| 3 | `pausePlayback` L819 | Offset uses the *current* rate for the whole elapsed time, wrong if rate changed mid-play | Position is integrated per sample |
| 4 | `handleValueEditEnd` L1401–1404 | Revert path uses a different (wrong) inverse speed formula than the rest of the file | Single range definition (§2.3) |
| 5 | `loadAudio` L653–655 | Decoded size is an estimate assuming stereo at context rate | Exact size from `AudioFormatReader` metadata |
| 6 | `exportAudio` L1538–1545 | Tail = IR length (1.35 s), which truncates real reverb decay | Fixed 3.0 s tail |
| 7 | `applyPerfection` restore L1032–1033 | Restores wet/gain using the *preset's* stored normalised position, so knob visuals desync | Attachments derive visuals from the value automatically |
