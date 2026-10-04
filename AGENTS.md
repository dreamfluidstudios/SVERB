# Agent Instructions: SVERB Standalone Audio App

## Environment and Framework
- Framework: JUCE (Basic Plug-In Template, configured for Standalone format)
- Language: C++17 or C++20
- Required Modules: juce_core, juce_audio_processors, juce_audio_utils, juce_dsp

## Directory Layout
- Master Root: SVERB/
- Prototype source: Prototypes/index.html
- Specifications: Docs/ARCH_SPEC.md
- Source code: Source/ (contains the .jucer file and Source/ subfolder with C++ files)
- Build outputs: Application/

## Core Execution Tasks
1. Read Docs/ARCH_SPEC.md and Prototypes/index.html for architectural mapping and DSP/UI behavior.
2. Implement the playback speed alteration and the one-knob `juce::dsp::Reverb` inside the `processBlock` method in Source/PluginProcessor.cpp.
3. Expose parameters via `juce::AudioProcessorValueTreeState` (APVTS).
4. Implement UI controls in Source/PluginEditor.cpp matching the functional requirements of the prototype.
5. Keep changes isolated to the C++ source files inside the Source/ directory. Do not alter the top-level .jucer file configuration unless adding a required module path.
