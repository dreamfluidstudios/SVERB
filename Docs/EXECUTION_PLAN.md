# 24-Hour Implementation Blueprint: Claude & Codex Split

This document establishes the exact execution roadmap, prompts, and tool boundaries to port the HTML/JS audio prototype into a native JUCE Standalone Application within a 24-hour window, maximizing your \$20 subscription tiers.

---

## 🛠️ Step 1: Pre-Flight File Checklist (Do This First)
Before writing a single line of code, create the following empty files in the root folder of your Projucer project directory:
*   `CLAUDE.md` — The global rulebook for Claude Code.
*   `AGENTS.md` — The execution instruction guide for Codex.
*   `CHANGES.log` — The shared handoff log to pass context between models.
*   `ARCH_SPEC.md` — The core technical blueprint (generated in Phase 1).

*Ensure your original prototype `index.html` file is also dropped directly into this root folder.*

---

## 📋 Phase 1: High-Level Architecture (Hour 0 - Hour 1)
**Model to Use:** Claude 3.5/5.5 (Browser Chat or clean Claude Code session)
**Goal:** Extract the mathematical logic from the HTML code and build a bulletproof C++ design plan.

### The Kick-Off Prompt for Claude
Copy and paste this exact block into Claude along with your complete HTML/JS prototype code:

"""
Act as a Principal C++ Audio Architect specializing in the JUCE framework. I am porting an HTML/JS Web Audio API prototype into a native desktop application using the JUCE "Audio Plug-In" template configured for a Standalone target.

I need you to write a highly technical, exact Technical Design Document (`ARCH_SPEC.md`). Do not generate full C++ files yet. Instead, analyze the provided HTML/JS code and define the following structural mappings:

1. AUDIO ENGINE MAPPING:
   - File loading orchestration via `juce::AudioFormatManager` and `juce::AudioBuffer`.
   - The Speed/Playback rate alteration mechanics (resampling vs. fractional interpolation).
   - The 432Hz mode pitch offset math.
   - The Reverb DSP mapping linking the wetness parameter to the native `juce::dsp::Reverb` class.
   - The Output Master Gain node translation.

2. PARAMETER ARCHITECTURE:
   - Explicitly define the `juce::AudioProcessorValueTreeState` (APVTS) layout with precise parameter IDs for: Speed, Reverb Toggle, Reverb Wetness, Gain, 432Hz Toggle, and Perfection Toggle.

3. UI LAYOUT MAP:
   - Map the HTML elements to their native visual counterparts (`juce::Slider`, `juce::ToggleButton`, `juce::TextButton`).

Output the results cleanly so an execution agent can write the files blindly based on your specification.
"""

*Take Claude's output and save it entirely inside `ARCH_SPEC.md`.*

---

## ⚙️ Phase 2: Local Code Generation & Build (Hour 1 - Hour 6)
**Model to Use:** Codex (Desktop App / IDE Extension with Local Workspace Permissions Enabled)
**Goal:** Generate the C++ code blocks directly inside the source files and compile the project.

### Step 2A: Configure `AGENTS.md`
Paste this exact layout into your local `AGENTS.md` file so Codex understands its structural environment:
"""
# Project configuration
Framework: JUCE (Basic Plug-In Template configured for Standalone format)
Compiler Target: Standalone Application
Required Modules: juce_core, juce_audio_processors, juce_dsp

## Project Files to Modify
- `Source/PluginProcessor.h` & `PluginProcessor.cpp` (DSP Engine)
- `Source/PluginEditor.h` & `PluginEditor.cpp` (Visual Interface)
"""

### Step 2B: The Codex Execution Prompt
Open the Codex composer bar in your local project workspace and execute this prompt:

"""
Read `ARCH_SPEC.md` and the `index.html` prototype located in the root directory. 
Modify the implementation files inside the `Source/` folder directly to fully build out the native C++ application. 

Constraints:
- Implement the speed alteration and one-knob reverb inside the `processBlock` loop using thread-safe parameters.
- Expose all controls via an APVTS structure.
- Map the layout inside the Editor's `resized()` function using clean bounding boxes.
- Once all source files are written, execute the project compiler to verify that the application successfully compiles into a working Standalone target without errors.
"""

---

## 🔄 Phase 3: The Handoff & Troubleshooting Cycle (Hour 6+)
If Codex runs into a complex C++ audio engine error or linking conflict it cannot resolve after 2-3 compilation attempts, do not waste its context tokens. Perform a handoff:

1. **Document the Error:** Let Codex write the failing compiler logs into `CHANGES.log`.
2. **Switch to Claude Code:** Open your local terminal, type `claude`, and execute this prompt:
   > *"Look at `CHANGES.log`, read the compilation error, and review the files in `Source/`. Provide the exact code fix to correct this build block, write it to the files, and run the compiler target again."*
3. **Pass Back to Codex:** Once the error is fixed, clear the log, and let Codex continue adding structural layout features.

---

## 🚨 Critical Token-Saving Rules for Tomorrow
1. **Context Separation:** Do not let single chat threads run for hours. Once a major component (like the DSP processor) is compiling successfully, update your `CLAUDE.md` metadata, close the chat session, and open a fresh thread referencing the saved markdown files.
2. **Do Not Push Visuals:** Do not upload heavy graphic design layouts or UI mockups to the models tomorrow. Rely strictly on text coordinate dimensions for layouts. You will handle the visual polish on a future session.
