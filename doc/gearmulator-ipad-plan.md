# Gearmulator on the iPad — plan, state, and what to do next

Written 2026-09-26 ~17:55 EDT, while Michael is away. Nothing here is committed; nothing was pushed.

## Goal

Michael wants Machinedrum (MD) and Monomachine (MM) playable on his iPad with two hands, because the
panel was designed for two hands and is painful with a mouse. Self-signed/local only — no App Store.

Target device: **iPad 7th gen, iPad7,11 — Apple A10 Fusion, 3 GB RAM, iPadOS 18.7.8**,
UDID `6144a2ca75cc25bdf3f412ee18ebaee139d1e4dd`, CoreDevice `752F19AC-9469-52FD-BFB1-C0678CB02CBB`,
Developer Mode on, developer profile trusted.

## The hard truth we established today

| Path | Status |
|---|---|
| MD/MM as **native apps on the iPad** | Built, signed, installed, launchable — but performance is the blocker |
| Interpreter-only (the only mode iOS allowed so far) | **Measured 3.01 (MD) / 3.28 (MM)** real-time ratio on an Apple M4 at 512 frames. Over budget; an A10 is several times slower again |
| Same code **with JIT on the M4** | 0.75 (MD) / 0.96 (MM) — i.e. the JIT is the whole difference |
| JIT on the iPad | **Undetermined — being probed right now.** gpt-6-astra predicts, as an assumption-based prediction, that even with JIT the A10 stays above real time |
| Idle-loop elision as a rescue | No. Ceiling is ~1.5–2.2×, against a ≥3× gap. Not started, deliberately |

Michael reported on 2026-09-26 evening: **"doesnt work"** — the installed `Gearmulator MD` on the iPad
did not work for him. What exactly failed is **not yet known**; no crash report has synced to this Mac
(`~/Library/Logs/CrashReporter/MobileDevice` is empty). Do not guess between "crash on launch",
"no audio", and "audio broken up" — ask him, then reproduce.

A strong hypothesis, not a finding: `Jit` is a by-value member of `DSP` (`dsp.h` ~107) constructed
unconditionally (`dsp.cpp` ~93), so trampoline codegen and an executable `mmap` happen even in
interpreter builds. On iOS without a JIT grant that allocation can fail or the process can be killed —
which would look exactly like "doesn't work" regardless of audio. The in-flight probe is testing this.

## The chosen fallback (Michael picked this)

The iPad becomes a **two-handed touch panel driving the emulator on the Mac mini; audio comes from the
Mac.** The iPad only draws the panel, streams the LCD, and captures multitouch. This needs no JIT and no
A10 horsepower, so it is not a consolation prize — if the JIT probe fails, it is the product.

## Work in flight (started 17:41, after the Claude session limit reset)

1. **JIT probe** — Claude Code, Opus 5.5, in the repo. Created `iosjitprobe.{h,cpp}`, touched
   `jit.cpp`, `jittrampoline.cpp`, `synthLib/callbackCapture.h`, several CMakeLists, and a JUCE
   standalone header. Mission: does a development-signed app get executable memory on iPadOS 18.7.8,
   via debugger-attached launch (`devicectl … --start-stopped`) or entitlements? Then measure MD/MM on
   the device. Status doc: `doc/ios-ipad-status.md`.
2. **iPad panel** — Claude Code, Opus 5.5, in `/Users/michaelmeneses/gearmulator-ipad-panel` (outside
   the repo on purpose). First proving: drive the headless engine, read LCD back, measure transport
   latency. Then the two-handed panel. Status: `gearmulator-ipad-panel/STATUS.md`.
3. **gpt-6-astra** — design analysis to `/tmp/astra-jit2.md`, written incrementally (a previous attempt
   produced nothing by buffering its whole answer — the fix worked).

## Quota economics — read this before dispatching anything

- **Claude/Opus is the scarce resource.** It is a shared session limit, and Michael's unrelated
  `dryfire v2` project (Opus, unattended, in `/Volumes/ACEXR-Dev/acexr-clone`) competes for it. It
  already killed two runs today with "You've hit your session limit · resets 5:40pm".
- **kills are clean, not crashes.** A run that hits the limit exits immediately; nothing is corrupted.
  Both briefs require a status doc, so a killed run leaves evidence.
- **gpt-6-astra (codex) is a separate quota** and is cheap to use for reasoning. Prefer it for design
  and analysis while Opus is contended.
- Disk: internal ~13 GB free (was 2 GB — the 5.6 GB iPhone debug-symbol cache was deleted with
  Michael's approval). Build on `/Volumes/Crucial X9`, and **build inside a sparse APFS disk image**,
  because that volume is exFAT and exFAT corrupts code signatures (it already broke desktop VST3
  signing and left `._*` files inside the app code seals).

## Rules that still apply

- Never commit without Michael's explicit approval; no pushes; no `reset --hard` / `checkout -- .` /
  `stash` / `clean`; no `Co-authored-by` trailers.
- ROMs are private and copyrighted: MD
  `/Users/michaelmeneses/Documents/Gearmulator Preview/Machinedrum/roms/elektron_sps1-1uw_os1.63.bin`,
  MM `/Users/michaelmeneses/Documents/Gearmulator Preview/Monomachine/roms/elektron_sfx6-60_os1.32b.bin`.
  Never commit them, never copy them into a repo, never upload them.
- Free Apple account: the provisioning profile expires **2026-10-03** and both apps stop launching
  until re-signed. Re-signing is scriptable (~1 minute) but needs a connected, unlocked iPad.

## When Michael is back — in this order

1. **Ask what "doesn't work" means**, exactly: did it fail to launch, crash, show the panel but no
   sound, or sound but broken up? Reproduce it with console capture:
   ```sh
   xcrun devicectl device process launch --device 752F19AC-9469-52FD-BFB1-C0678CB02CBB --console \
     --terminate-existing com.michaelmeneses.mzz78tc6yl.GearmulatorMD
   ```
2. **Read the JIT verdict** from `doc/ios-ipad-status.md`.
   - JIT possible → measure MD/MM on the A10 and decide with real numbers whether the standalone app
     is viable at 512/1024 frames.
   - JIT impossible, or still above real time → **stop the standalone path** and finish the panel.
3. **Judge the touch layer by hand.** No measurement substitutes for this: put two hands on the panel
   and say whether simultaneous trigs, Function chords and encoder press-and-turn feel like the real
   machine. This feeds the panel build directly.
4. Decide the panel's remaining scope: which model first (MD or MM), LCD fidelity, and whether it
   eventually also drives the real hardware over USB-MIDI.

---

# gpt-6-astra analysis, 2026-09-26 — `/tmp/astra-jit2.md` (full text there; this is the digest)

## The JIT question

- **Best route: launch suspended, attach Apple's device debugserver via Xcode/LLDB, THEN continue.**
  `devicectl … --start-stopped` only suspends the launch — it grants nothing by itself. The attach must
  happen **before** the DSP/`Jit` constructors run, because that is when the executable trampolines are
  allocated. A plain local `lldb attach -p <pid>` is not sufficient; it must be a properly connected
  remote device session.
- **Does tracing grant executable memory? Qualified yes** — an authorised debug session can mark the task
  `CS_DEBUGGED` and relax W^X, but `cs_allow_invalid()` still consults the MAC policy first and can deny.
  `get-task-allow` permits debugging; it is *not* an executable-memory entitlement.
- **Entitlement routes are dead ends:** `com.apple.security.cs.allow-jit` is a macOS Hardened Runtime
  exception, not a Personal Team iOS capability; `dynamic-codesigning` is restricted Apple-only.
  A profile is Apple-signed and cannot be expanded locally. Do not burn time here.
- **Community routes** (StikDebug, SideJITServer, AltJIT) support 17.4–18.x and are *the same permission*
  via more setup. Same race warning: this app allocates JIT during construction, so an enabler that
  launches first and attaches after may lose.

## Performance prediction — this is the sobering part

**Even with a working JIT, an A10 is predicted to land at MD ≈1.6–2.9× and MM ≈2.0–3.6× the real-time
budget** (low-to-medium confidence, explicitly a prediction). Reasoning: JIT removes interpreter decode
but does not make one emulated DSP instruction one cheap ARM instruction; the M4 sits at 0.75/0.96, so
there is almost no headroom for a core 2.0–3.6× slower. **Permission success must not be reported as
feasibility success.** The measurement that settles it: a warm 512-native-frame `Hardware::advance`
timed loop on the A10, same source/ROM/state as the M4 benchmark.

## Two probe bugs to verify before trusting any JIT result

1. `csFlags()` returns `0xffffffff` when `csops` **fails**, and the existing bit test then treats that
   sentinel as "DEBUGGED" — a **false positive** that could make the probe claim JIT permission it does
   not have. Verify the sentinel is handled before believing a positive.
2. "SIGKILL right after this line = code-signing kill" is stronger than the evidence. A real code-signing
   kill shows as `.ips` `Namespace CODESIGNING, Code 2, Invalid Page`; jetsam and watchdog kills are
   alternatives. Confirm the termination reason.
   Also: `exec skipped` in the probe output is **inconclusive**, not success or failure.

Failure observables, in short: `mmap` denied → `MAP_FAILED` + capture `errno` at the call (asmjit maps
`EACCES`/`EPERM` to an opaque `kErrorInvalidState`, losing the distinction); `mprotect` denied → `-1` +
errno; AMFI execution kill → SIGKILL with a CODESIGNING invalid-page reason; silent interpreter
fallback → `g_useJIT=false` and no compiled-block execution. **There is no automatic runtime fallback:**
a JIT build whose allocations fail is likely to crash rather than quietly run slow.

Build-side facts: the tree already has an additive opt-in gate `DSP56K_IOS_ALLOW_JIT` (default OFF); a
JIT iOS build needs **both** `-DDSP56K_IOS_ALLOW_JIT=ON -DDSP56K_FORCE_INTERPRETER=OFF`. Note
`dspconfig.h` tests macro **presence**, so defining `DSP56K_FORCE_INTERPRETER=0` still disables the JIT —
the macro must be absent. asmjit's aarch64 backend is present and selected; its iOS allocator path uses
ordinary POSIX exec mappings rather than the macOS MAP_JIT API, and first attempt should leave it
unchanged. Also: the probe currently logs allocation errors and then keeps null trampoline pointers —
unsafe init; all three trampoline allocations *plus* actually executing generated code must succeed
before anything claims success.

## The fallback, concretely (better than expected)

- **Do not use MIDI for the panel.** MD note 36–51 NoteOn becomes an immediate panel **press+release
  pulse**, not a held press, so it cannot implement trig-hold parameter locks or Function chords.
  MCP is also insufficient (single RmlUi cursor, no independent touch IDs, screenshots are Mac file
  paths not LCD bytes). Lua is insufficient (no networking; sandbox excludes `io`/`os`/`package`).
- **Recommended: a small additive remote-panel adapter** reusing what already exists —
  `md::panelPacket()`, `panelEncoderPressPacket()`, `panelEncoderCommand()`,
  `Device::sendPanelEvent()`, and `FrontPanelPublisher`. Only the network endpoint, contact/session
  state and serialisation are new; no synthesis or firmware changes.
- **Input model:** `button(control, down, contactId)`, `encoderPress(encoder, down, contactId)`,
  `encoderDelta(encoder, signedDetents)`, plus `releaseAll` per session. Keep per-contact ownership and
  refcounts, then OR active bits into each scan-row mask — note `PanelRowState` is **not refcounted**,
  and desktop plus remote row writers can release each other's held bits, so the remote session needs
  one shared ownership/merge point or exclusivity. Preserve the verified ±1 encoder packet encoding
  (`0x01`/`0xff`); rate-limit bursts with a bounded remainder.
- **LCD is cheap:** `getLcdVram()` is 2×8×64 = **1,024 bytes**; adding the 14 raw LED banks (0x20–0x2d)
  and a written-bank mask gives 1,040 B, so **~1,064 B/frame** with a small header. That is **~32 kB/s
  at 30 Hz**, ~64 kB/s at 60 Hz, per instrument. No codec needed. Send only changed snapshots, always a
  full frame on (re)connect. Do **not** add a competing consumer to the LED transition ring (SPSC,
  already drained by the editor).
- **Transport:** one persistent WebSocket per instrument/session. Targets: ping/ack **median ≤5 ms,
  p95 ≤10 ms**; finger-event→Mac-audio **≤15–20 ms** (ideally ~10 ms); event→LCD update **≤35–50 ms**.
- **What breaks the feel:** Wi-Fi loss/retransmit, TCP backlog, per-event HTTP reconnects, waiting for
  an ack before sending the next finger's edge, background throttling, dropped releases, encoder-pulse
  queue overflow, and routing input through the editor's **33 ms `servicePanelQueue` timer** — physical
  button down/up already go straight to panel ingress, so preserve that path. Also: a bigger Mac audio
  buffer fixes underruns but adds touch-to-sound latency (512/48k is already 10.67 ms).

---

# OUTCOME, evening 2026-09-26 — the JIT question is settled, and the standalone path is closed

Full write-up: `doc/ios-ipad-status.md` §Phase 5. Verified independently by the orchestrator, not just
self-reported.

## Why "doesn't work" — reproduced

Launching the installed `Gearmulator MD` on the iPad (verified by hand, 18:19) aborts before any audio:

```
Jit@156: No profiler detected
libc++abi: terminating due to uncaught exception of type std::bad_alloc: std::bad_alloc
App terminated due to signal 6.
```

Uncaught `std::bad_alloc` in `vector<DSP::OpcodeCacheEntry>::__append` ← `DSP::clearOpcodeCache()` ←
`DSP::DSP()`: the dense interpreter opcode table is 0x800000 P words × 48 B = **384 MiB per DSP, ×2
DSPs**, on a 3 GB device. The app shows a UI and logs HDI08 init first, which is why it looked like it
launched. **This is the memory problem the first analysis predicted, now confirmed.** Before dying, the
ColdFire clock ran ~13× slower than real time. The interpreter build never reaches steady state on this
device, so there is no steady-state interpreter number — the 3.01/3.28 figures stay desktop-only.

## JIT on iPadOS 18.7.8: it works, and it still isn't enough

Real device evidence (probe log `Documents/jitprobe.txt`, `/Volumes/gmIosJit/results/`):

| | no debugger | after LLDB attach + detach |
|---|---|---|
| `csops` flags | `0x22003305` DEBUGGED=0 | `0x32003004` **DEBUGGED=1** |
| `mmap(RWX)` | returns a pointer | returns a pointer |
| execute that page | **SIGBUS KERN_PROTECTION_FAILURE** — iOS silently strips X | **SIGBUS** (same) |
| `mmap(RWX\|MAP_JIT)` | `EINVAL` (22) | `EINVAL` (22) |
| RW → `mprotect(RX)`, execute | signal 10 | **returns 42 ✔** |
| `vm_remap` mirror + `mprotect(RX)` | **SIGKILL CODESIGNING "Invalid Page"** | returns 42 ✔; RW-view patch visible as 7 through the RX view (coherent) |

So: **W^X works via `vm_remap`, `MAP_JIT` is unavailable and not needed, and `CS_DEBUGGED` persists after
LLDB detaches.** The personal-team profile carried `get-task-allow` only — no entitlement changes, no
third-party tools, no jailbreak. Astra's mechanism ranking was correct.

**But measured on-device with the JIT running (foreground, idle workload):**

```
              MD JIT            MM JIT
ratio mean    4.28              4.27
ratio p95     6.91              5.69
over budget   100 % of callbacks
audio/wall    0.23–0.25  (i.e. ~23–25 % of real time)
footprint     719 MB            654 MB
```

iOS/JUCE **ignored** a 1024-frame request — actual stayed 257 frames at 44.1 kHz. It doesn't matter:
`R(B) = r + hF/B` cannot fall below `r` ≈ 4, so **no buffer size passes.**

**Why no amount of DSP-side work fixes this.** Time Profiler, 20 s, MD JIT: `schedStep` 32.5 %,
**JIT-generated code only 14.2 %**, `processUC` (68K) 12.8 %, `HDI08::exec` 10.6 %, ESSI/DMA/peripherals
~13 %. Amdahl: making generated code infinitely fast buys **1.16×**; even deleting all of `schedStep`'s
self time is 1.5×. Getting under 1.0 needs a **≥4.3× whole-engine speedup** (≥5.3× for margin) on the
*idle* workload. Nothing scoped — idle elision, peripheral coalescing — is remotely that large.

A10 vs M4 with JIT: **~5.7× slower (MD) / ~4.4× (MM)**. And any JIT build on iOS needs a Mac (or a
StikDebug-style helper) to attach a debugger at **every launch**, so it was never shippable anyway.

## Conclusion, and what is now closed

- The **standalone-app path is dead on this iPad**, in both modes: interpreter (won't boot — memory) and
  JIT (boots, ~4× too slow, and needs a tethered debugger).
- The chosen fallback — **iPad as a two-handed touch panel, engine and audio on the Mac mini** — is the
  product, not a consolation prize. Astra's concrete design for it is digested above.
- The opcode-table memory fix (sparse instead of dense) is still worth doing so the interpreter apps
  *boot* on iOS, but it cannot make them real-time — do not let it become a rescue plan.

## Housekeeping left behind

- Nothing committed or pushed. Patches: `ipad-patches/ipad-phase5-jit-{root,dsp56300,asmjit,juce}-20260926-1817.patch`.
- JIT builds live in an APFS sparse image: `gearmulator-build-archive/gm-ios-jit.sparseimage`, mounted at
  `/Volumes/gmIosJit` (`hdiutil attach`). Results and trace: `/Volumes/gmIosJit/results/`.
- Installed on the iPad: `Gearmulator MD`, `Gearmulator MM`, `Gearmulator MM JIT`. The **free profile
  allows only 3 apps**, so `Gearmulator MD JIT` was uninstalled to make room; reinstall it from
  `/Volumes/gmIosJit/ios-jit/products/Release/Standalone/` after removing something else.
- Untested: the new measurement code (`synthLib/callbackCapture.*`) compiles for iOS but no desktop build
  was run to check it. It only activates behind environment variables.
- Astra's predicted probe false-positive (`csFlags()` returning `0xffffffff` on `csops` failure treated as
  DEBUGGED) **did not occur** — the recorded flags are real values, and the functional proof (executing
  generated code and getting 42) is stronger evidence than the bit anyway.
- Profile expires **2026-10-03**; the three installed apps stop launching then until re-signed.

## The panel client — written by hand, verified end to end on the Mac (2026-09-26 late)

The iPad client is no longer a stub. Nothing was committed; the repo source is untouched by this
pass (the Mac-side `mdRemotePanel*` code was already in the tree from the earlier agent run).

**Files** (`/Users/michaelmeneses/gearmulator-ipad-panel/`):
- `web/index.html`, `web/panel.js` — the client (multitouch panel, LCD, LEDs).
- `web/layout-md.json`, `web/layout-mm.json` — generated geometry.
- `tools/gen_layout.py` — generates that geometry from the machine's own skin documents.
- `tools/wsprobe.py` — stdlib protocol probe (LCD, ping, press, ownership, trigprobe).

**Where the panel geometry comes from (nothing guessed).** `mdEditor.cpp` holds the id ->
`md::PanelControl` / `md::PanelEncoder` table, and slots are ordinal, so `PanelControl(N)` is the
client's button index N. The rects come from the skin RML/RCSS with a small layout pass: absolute
rects from inline style, flex-row sequencing for the containers the skins use
(`.elektronStepStrip` 16 x `.elektronStepCell` 60x89dp containing an absolute `.elektronStepKey`
at 4,20 52x52). Verified against hand-computed values: `trigKey0` = (43,467,52,52),
`trigKey15` = (943,467,52,52) = 39 + 15x60 + 4. LED banks are real too: MD layout from
`mdLib/mdfrontpanel.h` (0x20/0x21 step, 0x22 status, 0x23 mode, 0x24/0x25 drum, active-low), MM
table from `mdEditor.cpp::createLeds()`.

**Measured, on the Mac (real evidence, not estimates):**
- Both `Gearmulator MD` on the host and the generated client work: 37 keys + 10 encoders (MD),
 44 + 9 (MM), 47 LEDs (MD), all rects inside the 1100x570 panel; a centre-pixel sample of every
  control confirmed 46/47 painted and the 47th (`encLevel`) has 1,277 fill pixels in its rect.
- Protocol loopback RTT: **p50 0.35 ms, p95 0.92 ms** (20 pings).
- Two-contact ownership: press c1 + press c2 + release c1 => control stays held; duplicate release
  `ignored`; release c2 `accepted`. Astra's release hazard behaves correctly.
- Synthetic press on PLAY through the client's own hit-test: server ack `accepted` x2.
- **Audio: striking a trig moved the rendered peak from -138.5 dBFS to -13.0 dBFS.** The engine
  makes sound; input reaches it.
- LCD decodes correctly (ASCII dump shows the Machinedrum boot screen).
- Host-side `recv->enqueued` measured **5.5 ms** on one PLAY press — over astra's <=5 ms median
  target, worth watching on the iPad.

**Server** auto-starts with the plugin unless `GM_REMOTE_PANEL=0`; it is listening on **7789** for
MD here (7788 is held by an older Gearmulator instance), web root defaults to
`~/gearmulator-ipad-panel/web`. Client URL on the LAN: **http://192.168.1.170:7789/** (`en1`;
`en0` is a link-local 169.254 address, not the LAN). Firewall is off.

**Build note:** the desktop build dir was reconfigured with `-DGEARMULATOR_MDMM_APPLE_PGO_MODE=none`
because the PGO profile lived in `/tmp/mdmm-perf/mdmm.profdata` and is gone. This build is therefore
~10% slower than a PGO build; flip the mode back (and retrain the profile) for real use.

**Unverified / honest gaps:** the panel draws a clean schematic of the machine (true geometry, real
labels, live LCD, live LEDs), *not* the photographic skin; on-device touch feel and Wi-Fi latency are
unmeasured; whether sequencer playback (PLAY) produces audio was not independently confirmed — only a
single trig was measured.