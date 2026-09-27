# Hermes status — MD/MM performance work, 2026-09-20 (afternoon)

Supersedes the state in `hermes-perf-handoff.md` for everything below. Read §1–§3 first.

Repo: `/Users/michaelmeneses/gearmulator-md-mm`, branch `release/md-mm-alpha`.
`E` = `source/elektron/md`, `D` = `source/dsp56300/source/dsp56kEmu`.

---

## 1. Where this stands vs the goal

Goal: run **Machinedrum AND Monomachine simultaneously** as AU/VST3 at low latency.
(Steam Deck secondary; iOS device work explicitly parked by the user this session.)

**Both engines now fit the real-time budget on the mean, running concurrently**, which was
not true before this session — MM alone was over budget at every block size.

Recommended setting right now: **512-frame buffer, 48 kHz, MD and MM on separate tracks.**

---

## 2. What landed (all measured, all gated)

| Change | Evidence |
|---|---|
| Step 2: benchmark comparability (`--model/--block/--seconds`, one fresh machine per invocation, `MEASURE_BEGIN`) | Gate 21/21 |
| Step 3: batch MD's UC→DSP catch-up via `execUntilCycles` (`mdhardware.cpp` `schedCatchUpDsp`) | MD mean −2.15/−3.13/−2.80/−0.42 % at 128/256/512/1024; MM unchanged (control) |
| Step 4: PGO (`-fprofile-instr-generate` → `llvm-profdata merge` → `-fprofile-instr-use`) | MD mean −9.0…−10.5 %, p95 −15.3…−16.2 %; MM mean −8.3…−10.4 %, p95 −9.1…−14.2 %. Every cell non-overlapping → decisive |
| Step 8 (partial): `analyze.py` reports `warm_mean_ms` / `warm_p95_ms` | `test_analysis.py` 4/4 pass |

Step 3 was **kept** on evidence (unanimous mean improvement, no MM regression). PGO **kept**.

### Cumulative (clean baseline → now), real-time ratio, lower is better

```
             before            now (step3+PGO)
MD  128   0.838 -> 0.740     MM  128  1.050 -> 0.963
MD  256   0.846 -> 0.741     MM  256  1.051 -> 0.961
MD  512   0.844 -> 0.746     MM  512  1.057 -> 0.958
MD 1024   0.835 -> 0.745     MM 1024  1.066 -> 0.956
```

### Plugin level, MD and MM running **concurrently** (separate processes/cores)

Pure render-time ratio (render_ms ÷ block period) is the meaningful cost signal.
See §4 for why "over budget" counts from this host are not trustworthy as-is.

```
config       MD ratio   MM ratio   MM callbacks over budget
48k/128       0.772      0.933     14.3 %
48k/256       0.784      0.938      5.6 %
48k/512       0.775      0.928      0.7 %
44.1k/128     0.736      0.883      6.6 %
```

### AU validation

Both AUs build and pass Apple's validator:

```
auval -v aumu Tmdr GmPv   # Machinedrum -> AU VALIDATION SUCCEEDED
auval -v aumu Tmno GmPv   # Monomachine -> AU VALIDATION SUCCEEDED
```

### Installed for the user

- `~/Library/Audio/Plug-Ins/VST3/Gearmulator {MD,MM}.vst3` — replaced with the optimized
  builds (13:41). Previously installed copies (2026-09-18, i.e. pre-optimization) are in
  `~/Library/Audio/Plug-Ins/VST3-backup-20260920/`.
- `~/Library/Audio/Plug-Ins/Components/Gearmulator {MD,MM}.component` — installed new.
  (The Components dir was empty before; no AU had ever been installed.)
- Both installed VST3s re-loaded and rendered correct audio after the install (verified).

---

## 3. Corrections to earlier claims (do not inherit)

1. `mdIdleSchedulerFirmwareTest` still does **not** exercise the DSPs. The DSP-affecting
   evidence for step 3 is instead: the 21/21 gate (MD 192 + MM 192 scheduler comparisons,
   MD/MM audio tests) **plus** the broker-level check in §5.
2. A DO loop **retires atomically inside a single JIT block**. Measured: a `do #$fff` loop
   (4096 iterations) reaches 8195 cycles and LC=0 in one block, so `SR_LF`/`LC` are never
   observable mid-loop and must not be asserted. Two consequences: the 256-frame "anomaly"
   is not explained, and any NOP-loop fast-forward proposal must be redesigned around this.
3. The old benchmark numbers were taken with an unrelated process pegged at ~124 % CPU
   for 12 h. Clean re-baseline moved MD 128 from 2622 µs → 2432 µs (≈8 % inflation).
   Old figures in `hermes-perf-handoff.md` §3 are therefore pessimistic.

---

## 4. Measurement caveats that matter

- `latency_host` is a paced console app, **not** a real-time thread. In captures,
  `late_ms` exceeded 50 ms in 223 callbacks for MD while MM stayed punctual. Its
  "over budget" counts therefore mix host scheduling noise with render cost; use
  `warm_mean_ms ÷ period` and use the same host for both sides of any comparison.
- `run.py` **hard-requires** a `.vst3` (`--plugin must identify the exact VST3 bundle`), and
  the host is built with `JUCE_PLUGINHOST_AU=0`. AU is not measurable by this harness as-is;
  it was validated with `auval` instead. Measuring AU performance needs a deliberate change.
- Building plugins puts products in `build/macos-md-arm64/products/Release/{VST3,AU}/`,
  **not** `bin/plugins/Release/` (those are empty shells with only an `Info.plist`).

### New finding: MD stalls ~285 ms twice around t≈16 s (plugin-level only)

Every MD capture shows two render spikes of 280–298 ms, ~0.6 s apart, near 15.7–16.8 s,
with a one-time total of ~575 ms. It does **not** recur in a 60 s capture, and the core
benchmark's worst block was 5.5 ms — so it is introduced by the plugin/wrapper path, not
the engine. `host.log` shows `[MD] factory flash preparation complete; rebooted in process`,
consistent with a one-time init rather than a recurring dropout source. **Unconfirmed in a
real DAW** — treat as open.

---

## 5. Test-harness work (this session)

`D/jitunittests.cpp` `JitUnittests::boundedDispatch` now has a DO-loop differential:
five cycle targets spanning a peripheral deadline (4097), comparing bounded dispatch
against repeated dispatch over registers, counters, cycles, timer and peripheral
observations. It passes and is gated (21/21, no skips).

Getting there fixed three real defects in the version left behind:
bad narrowing casts (didn't compile); a mid-loop `SR_LF` assertion that cannot hold
(§3.2); and the loop body ending in a self-branch plus only 0x1000 words of program
memory, which parked the PC and hung the suite. Program memory is now 0x8000 (zero words
execute as NOPs) and the guard `getInstructionCounter() >= 0x1000` stops the test passing
if the loop never runs.

---

## 6. Remaining work, ranked

1. **MM tail.** MM still exceeds budget on p95 at 128 (1.107) and 256 (1.030); it is the
   only engine that does. Prime suspects, both MM-specific and both currently unoptimised:
   MM's background quantum is **30 µs vs MD's 125 µs** (`mdtransportpolicy.h:27`), i.e. its
   scheduler slice runs ~4× more often, and MM sets `exactEssiCycleDeadlines = true`.
   MM also gets **none** of the dispatcher batching: both `schedCatchUpDsp` and
   `schedCatchUpDspToDsp` are gated off by the host-backlog check for MM.
   Any quantum change is a behaviour change — needs event-level equivalence, not just a
   timing improvement.
2. **MD's ~285 ms init stall** (§4) — confirm/deny in a real DAW host.
3. **PGO provenance.** The current profile is trained on the core benchmark, not plugin
   workloads, so it does **not** satisfy `scripts/macos/write_mdmm_receipt.py` (§849–867).
   Retrain on the plugin workloads before any release claim. Must retrain after source change.
4. **JIT wall-time attribution** (handoff step 5) — still not measured. Note §3.2 when
   designing any NOP-loop elision.
5. **Steam Deck** — not measured. Do not quote macOS-scaled predictions.
6. **AU performance measurement** — needs a host change (§4).

---

## 7. Rules observed

No commits were made (CLAUDE.md requires explicit approval). No pushes. No
`reset --hard` / `checkout -- .` / `stash` / `clean`. No `Co-authored-by` trailers.
No ROM committed, copied into the repo, or redistributed. Internal disk kept clear
(3.5 → 13 GiB free during this session).
