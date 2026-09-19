# Machinedrum standalone iOS app

Status (2026-09-19): full Xcode is installed; the iOS port builds with Unix Makefiles and a signed-device Xcode project has been generated. Development signing identity is valid. Xcode platform setup and device provisioning are being completed; physical-device audio and multitouch qualification remain pending. See `hermes-ios-handoff.md` for current evidence and commands.

## Product goal

Build a Machinedrum-only standalone iPhone app for two-handed touch performance. The first release has stereo output, the existing Machinedrum panel, Files-based ROM import, persistent state, and multitouch gestures. Monomachine, AUv3, MIDI learn, external audio input, and desktop patch-management workflows are deferred.

The app must not ship a ROM. The user imports a supported Machinedrum OS image from Files; the app validates it, copies it into Application Support, and keeps the previous working image if validation or initialization fails.

## Architecture

```text
JUCE iOS app
  ├─ iOS services: Files importer, app lifecycle, audio-session policy
  ├─ touch/panel layer: RmlUi + Metal + per-finger capture
  ├─ MdEngine: bounded commands, panel snapshots, state handoff
  └─ mdLib: existing Machinedrum device, DSP, firmware, resampler
```

Thread ownership:

- Audio callback: process the active machine and consume bounded commands; no file I/O or allocation.
- Worker: validate ROMs, prepare engine/state replacements, and retire old instances.
- Main thread: UIKit, RmlUi updates, Files picker, and touch routing.

The iOS target should force interpreter execution initially. The desktop JIT path is not a safe assumption for an App Store-style iOS build and must be removed from the iOS execution path, not merely bypassed at dispatch time.

## Build phases

### 0. Toolchain and interpreter gate

Install full Xcode, select it with `xcode-select`, and verify `xcodebuild`, `simctl`, and `devicectl`. Add a small iOS/device engine benchmark. Prove a signed Release build boots the imported ROM and produces audio without runtime executable-memory generation.

### 1. Portable engine boundary

Add `MdEngine` around `mdLib` and `ResamplerInOut`. It owns prepared buffers, ROM identity, bounded panel/MIDI commands, panel snapshots, and asynchronous state replacement. Keep the desktop plugin path unchanged.

### 2. iOS target

Add an Xcode-generator CMake preset and a Machinedrum-only `MachinedrumIOS` target. Disable all plugin formats and unrelated synths. Keep existing desktop targets valid. Link only the JUCE iOS app, Metal, audio, and required Machinedrum dependencies.

Likely files: `CMakePresets.json`, `source/juce.cmake`, `source/CMakeLists.txt`, `source/elektron/md/mdJucePlugin/CMakeLists.txt`, plus a new `mdIOS/` target directory.

### 3. ROM import and persistence

Use `UIDocumentPickerViewController` through Objective-C++. Stage the selected file, require the exact supported 8 MiB Machinedrum image, then atomically install it under `Application Support/Machinedrum/ROMs/<digest>/`. Persist session state and ROM identity separately from the ROM. The app must relaunch offline after the original Files provider is unavailable.

Likely files: `mdIOS/RomImportService.mm`, `mdIOS/AppPaths.mm`, `mdEngine/RomRepository.*`, and the existing `mdromloader.*` integration.

### 4. Audio and lifecycle

Use JUCE's audio device backend with output-only stereo for the first milestone. Request a preferred rate/buffer, then honor the activated hardware values. Keep the emulator at 44.1 kHz and use the existing resampler for iPhone routes such as 48 kHz. Handle interruptions, route changes, media-services reset, suspension, and foreground return without touching the audio callback from UI/file code.

### 5. Touch UI

Reuse the existing RmlUi documents, skins, LCD model, and Metal renderer, replacing desktop-only view assumptions with an iOS-compatible Metal host. Add per-finger capture and cancellation handling for simultaneous trigs, Function chords, trig-plus-encoder gestures, independent encoder drags, and encoder press/turn. A released finger must not release a control still held by another finger.

Likely files: `source/juceRmlUi/MetalContext.mm`, `RmlUi_Renderer_Metal.mm`, `rmlMouseInput.cpp`, `mdEditor.cpp`, and a new `mdIOS/TouchRouter.*`.

### 6. Qualification

Run existing firmware/state/audio/front-panel/rendering tests, then add import rollback, malformed state, touch cancellation, queue saturation, sample-rate, interruption, and lifecycle tests. Qualify on a physical iPhone: dense patterns, two-hour endurance, no underruns, no stuck controls, and stable thermal behavior.

## Acceptance gates

1. Full Xcode is installed and device/simulator targets configure cleanly.
2. A signed Release build launches on the iPhone without a debugger.
3. The imported Machinedrum ROM validates and survives relaunch/offline use.
4. Audio remains stable at the actual device sample rate and supported buffer sizes.
5. Two-handed multitouch gestures work, including cancellation and independent finger ownership.
6. Existing macOS plugin builds and archives remain unchanged.

## Current blocker

Full Xcode 26.3 is selected at `/Applications/Xcode.app/Contents/Developer`. The iPhone is paired with Developer Mode and DDI services enabled. The Apple Development identity and WWDR G3 chain are valid. Xcode's scheme destination currently reports its iOS platform unavailable; direct target provisioning reports no registered team devices. Platform setup is in progress. These replace the earlier missing-Xcode blocker. This explicitly authorized personal test build includes the user's local ROM; distributable builds must not bundle it.
