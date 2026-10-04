# FUTURE_SPEC.md
# Future Phase Vision: The Video-Enhanced MP3 Player Target

This document serves as the permanent architectural marker to transition the standalone desktop application from a basic utility into a full feature-rich media platform in a future session.

---

## 🎯 Product Concept Summary
The future iteration will transform the application into a **Video-Enhanced MP3 Player**. It will retain the core real-time DSP manipulation engine (Speed, Pitch, and Reverb controls) while introducing a fully dynamic, audio-reactive visual playback loop. 

Instead of looking like a traditional audio plug-in, the interface will mirror a sleek, modern desktop media player. When a user loads an audio track, they can search for, pick, and loop highly stylized visual assets that render inside the UI in perfect lockstep with the playback controls and altered audio speeds.

---

## 🧱 Core Architecture Additions

### 1. The Visual Loop Engine (Klipy API Integration)
* **The Workflow:** The user types a keyword into a UI search bar. The application queries the **Klipy API**, parses the JSON response, and downloads the chosen visual looping asset asynchronously.
* **Threading Bounds:** The network request and file-download streams will live entirely within a dedicated background thread (`juce::Thread`). This ensures zero interference with the real-time audio thread (`juce::AudioProcessor`) to eliminate clicks or pops.
* **Rendering Choice:** The application will utilize the **`juce_video`** module's **`juce::VideoComponent`** to natively render downloaded high-efficiency MP4 video formats. This ensures smooth hardware acceleration without overloading CPU cycles.

### 2. Advanced Interactive UI Controls (Rive.js / Rive C++)
* **The Component Strategy:** To achieve professional UI animations (fluid button states, morphing playback toggles, and smooth knob physics), the interface will be designed using **Rive**.
* **Native C++ Pipeline:** The pipeline will bypass raw HTML canvas wrappers and integrate the **Rive C++ Runtime library** straight into the custom JUCE graphics pipeline. The UI components will pull vector coordinate parameters directly from a compiled `.riv` asset file, drawn inside the `juce::Component::paint()` function.
* **Control Layout Extensions:** The layout will expand to include full standard transport controls: Play, Pause, Stop, a scrubbable timeline tracker (`juce::Slider`), and a Visuals Asset Search Input field.

---

## 🗂️ How to Use This Context File

When you begin the next sprint, initialize the active AI model (Claude Code or Codex) by providing this file alongside the working V1 repository. 

**Kick-off Prompt Example:**
> *"Read `FUTURE_SPEC.md`. We are now transitioning the working audio codebase into this new target architecture. Review our existing `PluginProcessor.cpp` and `PluginEditor.cpp` layout, and give me a step-by-step implementation blueprint to safely integrate the Klipy API network thread without introducing thread safety issues to our real-time processing loop."*

---

## 🔗 Reference API Resources
Use these official channels to pull endpoint schemas and API implementation logic for the video workflow:
- **API Documentation:** [Klipy Getting Started Guide](https://docs.klipy.com/getting-started)
- **API Functional Overview:** [Klipy API Overview & Endpoints](https://klipy.com/api-overview#overview)
- **Official Open-Source Implementations:** [Klipy GitHub Repository](https://github.com/KLIPY-com/Klipy-GIF-API)