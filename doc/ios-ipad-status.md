# iPad MD/MM status — 2026-09-26

Brief: `~/.hermes/cache/scratch/gearmulator-ipad-brief.md`. Nothing committed or pushed.
Target (confirmed via devicectl): iPad (7th generation), iPad7,11, **iPadOS 18.7.8**,
UDID `6144a2ca75cc25bdf3f412ee18ebaee139d1e4dd`, CoreDevice `752F19AC-9469-52FD-BFB1-C0678CB02CBB`,
paired, wired, Developer Mode on, DDI available.

Build dirs (all external): `/Volumes/Crucial X9/gearmulator-build-archive/`
- `ios-ipad-xcode/` — signed iOS Xcode project; apps in `products/Release/Standalone/`
- `macos-touch-test/` — desktop build + ctest
- `macos-interp-bench/` — desktop interpreter-only `mdPerfBenchmark`
- Rollback patch: `ipad-patches/ipad-phase1-2-final-20260926-1718.patch` (includes new files)

## Blocked right now

**Both apps are installed on the iPad but can't launch until the user trusts the developer profile on the iPad:**
Settings → General → VPN & Device Management → *Apple Development: …* → Trust.
`devicectl` launch error: *"its profile has not been explicitly trusted by the user"* (log:
`ipad-md-console-2.log` in the archive dir). Only a person at the iPad can do this step.

Then run:
```sh
xcrun devicectl device process launch --device 752F19AC-9469-52FD-BFB1-C0678CB02CBB --console \
  --terminate-existing --environment-variables '{"GEARMULATOR_RT_INSTRUMENTATION":"1"}' \
  com.michaelmeneses.mzz78tc6yl.GearmulatorMD
# the perf capture JSONL lands in the app container: Documents/Gearmulator Preview/<product>/logs/
xcrun devicectl device copy from --device 752F19AC-9469-52FD-BFB1-C0678CB02CBB \
  --domain-type appDataContainer --domain-identifier com.michaelmeneses.mzz78tc6yl.GearmulatorMD \
  --source Documents --destination "/Volumes/Crucial X9/gearmulator-build-archive/ipad-md-docs"
```
(Same for `…GearmulatorMM`.)

## Phase 1: builds (done, verified)

Changes:
- `source/elektron/md/mdJucePlugin/CMakeLists.txt`: new cache vars `GEARMULATOR_IOS_ROM_MD` /
  `GEARMULATOR_IOS_ROM_MM`. Each product embeds only its own ROM (`machinedrum.bin` / `monomachine.bin`).
  Legacy `GEARMULATOR_IOS_ROM` still maps to MD.
- `source/juce.cmake`: ROM copy uses the per-target `iosRomFile`/`iosRomName`. iPad orientations are now
  landscape only, plus `REQUIRES_FULL_SCREEN TRUE`. Portrait would fit the 1252×648 skin at about 65% of the
  landscape scale; landscape on 1080×810 pt gives about 0.86×. All changes are iOS-only; desktop is unchanged.

Evidence (both apps built with `-destination id=<iPad UDID> -allowProvisioningDeviceRegistration`, `BUILD SUCCEEDED`):

| | MD | MM |
|---|---|---|
| Bundle ID | `com.michaelmeneses.mzz78tc6yl.GearmulatorMD` | `…GearmulatorMM` |
| Display name | Gearmulator MD | Gearmulator MM |
| TeamIdentifier | MZZ78TC6YL | MZZ78TC6YL |
| `codesign --verify --deep --strict` | valid | valid |
| Profile ProvisionedDevices | iPad UDID | iPad UDID |
| Profile expiry (Personal Team) | 2026-10-03 | 2026-10-03 |
| Embedded ROM SHA-256 = source ROM | `68542e30…c8` ✔ | `36984917…7e` ✔ |
| Size | 27 MB | 26 MB |
| UIDeviceFamily | 1,2 | 1,2 |
| Installed (`devicectl device info apps`) | yes | yes |

Also verified: configure forces `DSP56K_FORCE_INTERPRETER=ON` for iOS.

Notes:
- The Crucial X9 volume is **exFAT**. Bundles built there pick up `._*` AppleDouble files that end up
  inside the code seal, 10 per app. They're harmless, and stripping them would break the signature. The
  desktop ad-hoc signing of `.app`/`.vst3` fails on exFAT for the same reason. Fix: use an APFS
  volume, or an APFS disk image on the X9, for bundle output.
- The iOS Release flags include `-Ofast`, i.e. fast-math. This was already there before my changes and I left it alone.

## Phase 2: two-handed multitouch (implemented; verified host-side only)

Design: `source/juceRmlUi/rmlTouchRouter.{h,cpp}` (new).
- A finger landing on a *touch-capture* element owns that element until it lifts. The router sends
  Mousedown / Drag / Mouseup (+ Click if lifted inside) straight to that element, with a `touch_id`
  parameter. It never sends Mouseout and never moves the RmlUi mouse, so a new finger can't unhover or
  release another finger's control.
- Several fingers on one control: only the first sends Mousedown/Drag, and the Mouseup goes out only
  when the **last** finger lifts.
- Fingers on non-capture UI (menus, settings) still use the RmlUi context, one at a time.
- Cancellation: panel hidden → `cancelAll` (Mouseup with `touch_cancel`, no Click). Teardown clears
  finger state silently. A lost up (reused finger id) cancels the old gesture first.
- Mouse input on desktop takes the unchanged legacy path. Only `source.isTouch()/isPen()` gets routed.

Wiring:
- `juceRmlComponent.{h,cpp}`: routes touch down/drag/up. Touch enter/move/exit no longer touch the context.
- `rmlElemKnob.cpp`: every knob is touch-capture.
- `mdEditor.{h,cpp}`: panel switches, LCD, and label/LED affordances are touch-capture. Encoder press and
  the LCD drag remember their owning finger, so another finger's Mouseup (which bubbles to the document)
  doesn't end them. **Touch encoder press:** holding an encoder still for 400 ms (10 px slop) presses it;
  keep holding and drag for press+turn; lifting releases. The desktop Alt-click path is unchanged.

Coverage of the brief's gestures: simultaneous trigs, Function chords (physical buttons), trig + encoder,
independent encoder drags, and encoder press/turn.
Known limits:
- Tapping a label/LED shortcut still ends other held buttons. That's the existing "labels own the whole
  gesture" rule, and it matters because `PanelRowState` is a bitmask, not refcounted.
- Only one encoder press can be held at a time.
- A second finger on the same knob holds it but doesn't turn it.

Tests:
- New `juceRmlTouchRouterTest` covers independent holds, a shared control, independent drags, context
  finger coexistence, cancel, lost up, a removed element, and modifier forwarding. It passes. I checked it
  catches regressions by removing the shared-control guard: the test failed, and passed again once restored.
- `juceRmlMouseInputTest` still passes.
- Full desktop ctest with both ROM env vars (`macos-touch-test/ctest.log`): 77/88 pass. None of the failures
  involve code I touched:
  - `synthLibMidiClockTimingTest`, plus `synthLibAudioTest` (Not Run because it depends on it): known pre-existing.
  - 6× `*_VST3*` tests: the bundles weren't produced because of exFAT codesign.
  - `mdStateTest`: 1 subtest, "immutable factory cache is created exclusively". This is a file-exclusivity
    check running on the exFAT build dir; it's `mdLib` code I didn't touch.
  - `mmSineFirmwareTest`: "GND SIN produced silence". **Pre-existing:** the Sep 20 HEAD binary in
    `build/macos-md-arm64` fails the same way. It only links `mdLib`.
  - `mmDigiproFirmwareTest`: also `mdLib`-only. I haven't compared it against the old binary (the run takes about 5.5 min).
  - `mdSysexLifecycleTest` was **Skipped** ("explicit firmware fixtures required"). It needs fixture env
    vars beyond the two ROM paths, so it isn't a valid gate result.
- **Not verified:** real multitouch on the iPad. That needs the Trust step.

## Performance: the most important finding

Desktop interpreter-only (`DSP56K_FORCE_INTERPRETER=ON`, same code path as iOS), Apple M4, block 512,
15 s, one fresh machine per run (`macos-interp-bench`, `mdPerfBenchmark --model X --block 512 --seconds 15`):

```
MD 512: mean 34954us  p95 41170us  max 74138us  budget 11610us  ratio 3.01
MM 512: mean 38119us  p95 38753us  max 49362us  budget 11610us  ratio 3.28
```
With JIT the same machine runs MD at 0.75 and MM at 0.96. **Even an M4 in interpreter mode runs about 3×
slower than real time.** The A10 is several times slower per core than the M4. I haven't measured it yet
(blocked on Trust), but a ratio under 1.0 on the iPad is not plausible without a very large change.

Implication for Phase 4: eliding DSP2's NOP loop and Port C spin (DSP2 is about 96% idle) can at most
remove DSP2's share of the cost. The other DSP and the 68K remain. It can't close a ≥3× gap on an M4, let
alone on an A10. I did **not** start the elision work: the brief gates it on Phase 3 measurements, and the
numbers above show it isn't sufficient on its own. The only lever of the right size is JIT on the device.
These builds already carry `get-task-allow`, and on iPadOS 18 a debugger-attached process may be allowed
executable memory (not verified on this iPad). That would take a deliberate change to the iOS JIT path (currently compiled out),
so it needs a decision from the user.

## Next steps
1. User taps Trust on the iPad (see above). Then launch MD and MM with console capture, confirm audio and
   multitouch, and pull the perf JSONL for actual sample rate, block size, and mean/p95 ratio.
2. Decide on direction given the ≥3× interpreter gap: JIT-under-debugger on iPadOS 18 vs. reduced
   fidelity vs. dropping the iPad goal. NOP elision alone won't make it real-time.

---

# Phase 5: JIT on the iPad (2026-09-26, evening)

Brief: `~/.hermes/cache/scratch/gearmulator-jit-brief.md`. Nothing committed. The user had tapped Trust, so device launch works.
Build: `/Volumes/gmIosJit/ios-jit` (60 GB sparse **APFS** image `gearmulator-build-archive/gm-ios-jit.sparseimage` on the X9; mount it
with `hdiutil attach`). Results, crash logs and the time profile are in `/Volumes/gmIosJit/results/`. Rollback patches are in
`ipad-patches/ipad-phase5-jit-{root,dsp56300,asmjit,juce}-20260926-1817.patch`.

## Answer

**JIT is achievable on iPadOS 18.7.8 / A10 for this development-signed app, but only while the process is `CS_DEBUGGED`.**
Launch it with `devicectl … --start-stopped`, attach LLDB, then detach. **It does not make MD or MM real-time.** With the JIT running,
both models run about **4× slower than real time**, every callback overruns at every buffer size, and **no buffer size passes**.

## Verified on the device

### 1. The interpreter builds (the installed "Gearmulator MD"/"MM") don't survive launch
Every launch of the baseline MD (5 crash logs, 17:42–17:51) and MM (18:16) ends in `SIGABRT` from an uncaught
`std::bad_alloc` in `vector<DSP::OpcodeCacheEntry>::__append` ← `DSP::clearOpcodeCache()` ← `DSP::DSP()`. That's the dense
interpreter opcode table (0x800000 P words × 48 B = 384 MiB per DSP, ×2 DSPs). The A10-plan memory concern is confirmed.
The app shows a UI and logs HDI08 init first, which is why "Launched application" looked like success. Before dying, the ColdFire clock
advanced about 3.1 M cycles per wall second against 40 M nominal (≈13× slower than real time, boot phase, startup log).
There's no steady-state interpreter number on the device because the interpreter never reaches steady state.

What the interpreter build still does with the JIT: `Jit` is constructed and its trampolines are generated (`Jit@156: No profiler
detected` in the console), and asmjit allocates executable memory. The code is never run, so there's no kill. The JIT build's probe
shows the allocation *succeeds* without a debugger (details below). `MmuHelper: shm_open failed, err 1` and
`mmap failed to reserve address range` also appear in both builds. The sandbox denies them and they fall back.

### 2. Executable-memory probe (`dsp56kEmu/iosjitprobe.cpp`, runs before the first `Jit`; log in `Documents/jitprobe.txt`)

| | no debugger | after LLDB attach + detach |
|---|---|---|
| `csops` flags | `0x22003305` VALID HARD KILL, DEBUGGED=0 | `0x32003004` VALID=0 HARD=0 KILL=0 **DEBUGGED=1** |
| `mmap(RWX, MAP_PRIVATE\|MAP_ANON)` | returns a pointer | returns a pointer |
| execute that RWX page | **SIGBUS KERN_PROTECTION_FAILURE** (crash log 17:55:49) | **SIGBUS KERN_PROTECTION_FAILURE** (17:56:39). iOS silently strips X from W+X |
| `mmap(RWX\|MAP_JIT)` | `EINVAL` (22) | `EINVAL` (22) |
| RW → `mprotect(RX)` | returns 0 | returns 0, **call returns 42 ✔** |
| execute RW→RX page | **terminated, signal 10** (17:55) | ✔ |
| `vm_remap` mirror + `mprotect(RX)` | returns 0, then **SIGKILL CODESIGNING "Invalid Page"** on first touch (`sys_icache_invalidate`, crash 17:54:29) | returns 42 ✔; patching through the RW view → the RX view returns 7 (**coherent**) ✔ |

The detach is enough: `CS_DEBUGGED` stays set after LLDB detaches, and the session never kept a debugger attached.
The personal-team profile carries `get-task-allow` only. `MAP_JIT`/`allow-jit`/`dynamic-codesigning` aren't available and weren't needed.
No third-party tools were used.

### 3. JIT variant (separate products: `com.michaelmeneses.mzz78tc6yl.jit.GearmulatorMD` / `…jit.GearmulatorMM`, names "Gearmulator MD JIT" / "MM JIT")
Code changes, all opt-in or iOS-only:
- `dsp56300/source/CMakeLists.txt`: `DSP56K_IOS_ALLOW_JIT` (default OFF) skips the forced interpreter on iOS.
- asmjit `virtmem.cpp` (iOS only): treat the runtime as hardened (the RWX probe is a false positive on iOS), and add a dual mapping
  via `vm_allocate` plus a `vm_remap` RX mirror (the sandbox denies the stock `shm_open`/tmp-file dual mapping).
- `jit.cpp`: on iOS, skip the per-DSP 8M-entry dispatch template. With it, each DSP mode chain got a 64 MiB table.
  After about 10 modes (footprint around 640 MB) `new` failed. The throw was caught in LLDB at
  `MmuArray::ensureBlockForIndex` ← `JitBlockChain::JitBlockChain` ← `Jit::checkModeChange` → `std::terminate` (crash 17:59:03).
  Without the template, the table grows only to the highest P address actually used. `checkModeChange` now logs such an exception
  before rethrowing it.
- Probe logging of `runtime.add` results (trampolines: all OK).
- Measurement: `synthLib/callbackCapture.*` (`GEARMULATOR_RT_FULL_CAPTURE=1`) records **every** callback into a preallocated buffer
  (unbiased, unlike the sampled RT report) and samples `phys_footprint` every 2 s. The JUCE standalone takes a
  `GEARMULATOR_AUDIO_BUFFER` / `GEARMULATOR_AUDIO_SAMPLERATE` request and logs requested vs actual to `Documents/audiosetup.txt`.
  The analysis scripts are `/Volumes/gmIosJit/analyze_rtcapture.py` and `windows.py`.
- Build flags are the same as the interpreter build (Release, `-Ofast -DNDEBUG`, no PGO/ThinLTO). The only differences are the bundle ID prefix,
  the display-name suffix and the JIT.

Run: `/tmp/run-measure.sh` = `devicectl process launch --start-stopped` → `lldb device process attach` → `process detach`, app in the
foreground with the UI up, idle after boot (no pattern playing, so this is the *light* workload). The first 20 s are skipped.

| | MD JIT (100 s) | MD JIT, 1024 requested (75 s) | MM JIT (100 s) |
|---|---|---|---|
| actual AVAudioSession | 44100 Hz, **257** frames | requested 1024 → **actual 257**, 44100 Hz | 44100 Hz, 257 frames |
| budget | 5828 µs | 5828 µs | 5828 µs |
| render mean / p50 / p95 / p99 / max | 24.9 / 21.3 / 40.3 / 45.0 / 126 ms | 23.0 / 20.7 / 36.4 / 38.9 / 48.7 ms | 24.9 / 23.7 / 33.1 / 46.3 / 197 ms |
| **ratio mean** / p95 / max | **4.28** / 6.91 / 21.7 | **3.94** / 6.25 / 8.35 | **4.27** / 5.69 / 33.7 |
| steady 10 s windows | 3.6–4.4 (spikes to 6.2) | – | 4.0–4.7 |
| over-budget callbacks | 100 % | 100 % | 100 % |
| audio delivered / wall | 0.23 | 0.25 | 0.23 |
| phys_footprint | 719 MB | 719 MB | 654 MB |

The app is stable under the JIT: no crash over 100 s, no jetsam. iOS logged a CPU-resource event for MM (94 % CPU over 96 s, "action
taken: none"). The engine saturates one core.

**Buffer size:** iOS/JUCE ignored the 1024 request and the actual stayed 257 frames. It doesn't matter: with a mean ratio of about 4 the engine
produces only about 25 % of real-time audio, and `R(B) = r + hF/B` can't drop below `r` ≈ 4 at any buffer. **No buffer size passes.**

### 4. Where the time goes (Time Profiler, 20 s, 20 244 samples, MD JIT, `results/md-jit.trace`)
95 % on the audio thread. Self time: `md::Hardware::schedStep` 32.5 %, **JIT-generated code 14.2 %**, `Hardware::processUC` 12.8 %
(68K + UC peripherals, mostly inlined), `HDI08::exec` 10.6 %, ESSI ring buffers about 5 %, `execPeripherals`/ESSI/DMA/EsxiClock about 8 %.
On the A10 the DSP instructions themselves are a minor cost. The scheduler, peripheral emulation and 68K C++ dominate.

## Inference (not measured)
- A10 vs M4 with JIT: MD 4.28 / 0.75 ≈ **5.7×**, MM 4.27 / 0.96 ≈ **4.4×** slower per core.
- To get below 1.0, MD needs a whole-engine speedup of ≥ 4.3× (≥ 5.3× for a 0.8 margin), and MM the same. This is the idle workload;
  a dense pattern will be worse. Amdahl: making the JIT code infinitely fast removes at most 14 %, a 1.16× speedup. Even removing
  `schedStep`'s entire self time is only 1.5×. Reaching 4–5× would need cutting roughly 80 % of the scheduler/HDI08/68K/peripheral
  work *and* keeping the JIT. Nothing in the current scoped work (idle elision, peripheral tick coalescing) is that big.
- Any JIT build on iOS needs a Mac (or a StikDebug-style on-device helper) to attach a debugger at every launch. It isn't shippable.

## Conclusion
JIT on the iPad works (debugger-attach route, W^X via `vm_remap`), but the A10 is about 4× short of real time for both MD and MM even
with it. Neither the interpreter nor the JIT build is viable on this device. The evidence supports redirecting the iPad to the
**touch-GUI-for-a-desktop-engine fallback**. The installed interpreter apps also need the opcode-table memory fix even to boot.

Device state left: baseline `Gearmulator MD` and `Gearmulator MM` are still installed (unchanged). `Gearmulator MM JIT` is installed.
`Gearmulator MD JIT` was uninstalled to make room (the free profile allows at most 3 apps). Reinstall it from
`/Volumes/gmIosJit/ios-jit/products/Release/Standalone/Gearmulator MD.app` after removing another one.
