# Project Guidelines for Claude Code

## Overview
SVERB is a native JUCE-based standalone desktop application porting an HTML/JS Web Audio API prototype.

## Project Paths
- Root: SVERB/
- C++ Source Files: Source/
- Documentation and Specs: Docs/
- Build Target: Standalone Application

## Coding Standards
- Maintain strict thread separation: perform all heavy non-audio tasks (like UI updates or future file/network workers) outside of the real-time `AudioProcessor::processBlock` thread.
- Ensure thread-safe parameter handling using atomic operations or APVTS attachments.
- Keep implementation clean, standard modern C++, relying on JUCE native classes and the `juce_dsp` module.

## Workflow Rules
- Read Docs/ARCH_SPEC.md and CHANGES.log for context synchronization when debugging or troubleshooting compiler errors.
- Output precise, drop-in C++ fixes or refactors when resolving build failures.
