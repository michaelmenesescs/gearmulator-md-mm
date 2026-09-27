# Hermes handoff — MD/MM performance work, 2026-09-20

You are orchestrating. You can drive both Claude Code and Codex; use them as described in §7.

Repo: `/Users/michaelmeneses/gearmulator-md-mm`, branch `release/md-mm-alpha`.
Read `CLAUDE.md` before anything. Shorthand below: `E` = `source/elektron/md`, `D` = `source/dsp56300/source/dsp56kEmu`.

---

## 1. The goal

Run **Machinedrum AND Monomachine simultaneously** as AU/VST plugins in a DAW at low latency, alongside dawless hardware. That is the user's actual need. Everything else is subordinate.

Secondary: Steam Deck (Linux x86-64). Deprioritised by the user: iOS/iPhone.

Machinedrum = two DSP56300s (~101.6MHz each) + a ColdFire/68K, running authentic firmware. Monomachine is the sibling. JIT via asmjit.

---

## 2. Current state (all committed, worktrees clean)

| Commit | Repo | What |
|---|---|---|
| `08edbfda` | parent (HEAD) | `mdPerfBenchmark` + `nopLoopMicrobench` harnesses |
| `3698be28` | parent | Batch `schedCatchUpDspToDsp` via `execUntilCycles` — **measured ~3–5%** |
| `a725bd2f` | `source/dsp56300` | **MOVEP peripheral-routing bug fix** + ESSI/HDI08 logging gated |

The MOVEP fix is a genuine emulation bug, not a perf tweak: `op_Movep_ppea` used plain `memRead`/`memWrite` for the effective-address operand, so when that operand named another peripheral (HORX → DMA destination/count during a host-command transfer) the access bypassed the peripheral path entirely — FIFO pops and status-flag updates never happened.

`ESSI` logging sat behind a stray `#if 1`, doing a synchronous `fputs` to stdout on **every ESSI register access in Release builds**, reachable from the audio thread on any HDI08/ESSI reconfiguration. Now behind `LOG_DIAGNOSTIC`.

Backups: `/Volumes/Crucial X9/gearmulator-build-archive/` and `s3://citizen-science-ableton-sessions/gearmulator/2026-09-19-repo-backup/`. A pre-work snapshot is at `/Volumes/Crucial X9/gearmulator-build-archive/perf-20260920-105230`.

Submodule note: `source/dsp56300` sits on a **detached HEAD**; branch `ios-md-mm-work` is kept pointing at the work so a stray `git submodule update` cannot orphan it.

---

## 3. Measured baseline (independently verified, non-PGO, Apple M4)

```
cmake --build build/macos-md-arm64 --target mdPerfBenchmark -j 8
export GEARMULATOR_MD_FIRMWARE_BIN="/Users/michaelmeneses/Documents/Gearmulator Preview/Machinedrum/roms/elektron_sps1-1uw_os1.63.bin"
./build/macos-md-arm64/source/elektron/md/mdLibTest/mdPerfBenchmark
```
`realtimeRatio` = CPU used ÷ budget. **Lower is better. >1.0 = dropouts.**

```
128:  mean 2402us  p95 3451us   max 4804us   budget 2902us   ratio 0.828   18768 ns/sample
256:  mean 6030us  p95 7579us   max 11118us  budget 5805us   ratio 1.039   23556 ns/sample  <-- OVER
512:  mean 9128us  p95 10052us  max 12824us  budget 11610us  ratio 0.786   17828 ns/sample
1024: mean 18490us p95 20187us  max 21894us  budget 23220us  ratio 0.796   18056 ns/sample
```

**The mean fits; the tail does not.** At 128, p95 = 119% and max = 166% of budget. Dropouts come from tail spikes, not average load. 1024 is the only size where even the max fits. **Treat p95/max as first-class targets, not afterthoughts.**

### Profile (`sample`, 6s steady playback)
`md::Hardware::advance` 489, `EsxiClock::exec` 292, `dspExecPeripherals<...>` 241, `schedCatchUpDspToDsp` 234, `HDI08::exec` 142, `m68k_execute_one` 125, `Sim::exec` 106, `DmaChannel::execTransfer` 104.
Recursive chain `DSP::execPeripherals → EsxiClock::exec → Essi::execTX → lambda → schedCatchUpDspToDsp → …` ≈ **20–25%**, independent of block size.
~27% is unresolved `<unknown binary>` — JIT-generated DSP code, no symbols.

### Idle measurement (interpreter retirement histogram)
- **DSP1 (Mixer): not idle** — real interrupt dispatch + audio MAC loop.
- **DSP2 (Producer): ~96% idle** — ~83.7% a fixed-count NOP delay loop (`do #$32`, 50 NOPs), ~12.3% a Port C busy-wait (`move x:<<M_PDRC,b / and #2,b / cmp b,a / beq`).

---

## 4. CORRECTIONS — read these, they overturn earlier claims

Prior analysis in this project asserted things that were **wrong**. Do not inherit them:

1. **`mdIdleSchedulerFirmwareTest` does NOT validate DSP changes.** At `E/mdLibTest/idleSchedulerFirmwareTest.cpp:93` **neither DSP runs**. It was repeatedly cited as the correctness gate for DSP-affecting work. It is not. It is also 192 comparisons per model, not 384.
2. **Fast-forwarding the fixed-count NOP loop is NOT automatically timing-safe.** It was claimed to be "bit-identical" because the cycle count is known. Wrong — interrupts and peripheral events can still fire mid-loop.
3. **`mdRamAudioOracleTest` is a synthetic oracle**, not firmware-backed.
4. **The "2–5× headroom" and Steam Deck scaling figures are hypotheses**, derived from cycle arithmetic, not measured bounds.
5. **The 256-block anomaly may be a measurement artifact.** `mdPerfBenchmark` sweeps block sizes sequentially in one process, so each size measures a *different musical moment*. Fix the harness before drawing conclusions.
6. `D/dsp.cpp:53` calls the peripheral callback, **not** nested interrupt dispatch. Leave it alone.

---

## 5. CRITICAL testing gotcha

`ctest` **silently SKIPS** the firmware-backed tests unless the ROM env vars are exported. A previous agent reported these as passing when they had been skipped.

```sh
export GEARMULATOR_MD_FIRMWARE_BIN="/Users/michaelmeneses/Documents/Gearmulator Preview/Machinedrum/roms/elektron_sps1-1uw_os1.63.bin"
export GEARMULATOR_MM_FIRMWARE_BIN="/Users/michaelmeneses/Documents/Gearmulator Preview/Monomachine/roms/elektron_sfx6-60_os1.32b.bin"
```
**Always grep the ctest output for `***Skipped` and treat any skip in the gate set as a failure.**

Known pre-existing failures, unrelated to this work: `synthLibMidiClockTimingTest` (untouched `synthLib` code) and four AU tests that cascade from it plus missing plugin bundles.

---

## 6. Remaining work, ordered

### SAFE — suitable for a competent implementer
1. **Fix benchmark comparability.** `E/mdLibTest/mdPerfBenchmark.cpp` — add `--model MD|MM`, `--block N`, `--seconds N`; one fresh machine per invocation, identical boot/warm-up, measure a single block size. Print `MEASURE_BEGIN` before measuring. Until this lands, the 256 result is not trustworthy.
2. **Batch the remaining UC→DSP dispatcher loop.** `E/mdLib/mdhardware.cpp:1526–1530` `schedCatchUpDsp`: when `m_schedBoundedJit && !s_mmBp`, use `d.dsp().execUntilCycles(std::min(targetCyc, clampStop))`; otherwise leave the loop unchanged. Mirror the structure already at `schedCatchUpDspToDsp:1596`. Preserve target arithmetic, clamp, diagnostics, MM backlog checks. Revert if MD mean/p95 doesn't improve or MM regresses.
3. **PGO.** Plumbing exists at `E/optimization.cmake:45–79`. `GEARMULATOR_MDMM_APPLE_PGO_MODE=generate` → train on the benchmark → `llvm-profdata merge` → `=use`. Expected ~8–12%, no semantic risk. Must retrain after source changes. Do NOT add compiler flags elsewhere or enable fast-math.

### ESCALATE — needs a strong model plus differential verification
4. **NOP-loop cost then elision.** First measure real JIT wall-time attribution (`mdNopLoopMicrobench` measures only a two-NOP body — validate the firmware body PCs `0x100095–0x100096` by disassembly before extrapolating). Any skipping must preserve interrupt/peripheral checkpoints, scheduler deadlines, LC/LA/SR/stack state and program-memory invalidation. Require differential event/audio/state traces against unmodified execution. **Reject gain claims based on instruction-count percentages alone.**
5. **Port C busy-wait.** Harder than the NOP loop: duration depends on the other DSP's progress. The `esaiFrameSyncSpinloopBra` precedent at `D/jitops_jmp.cpp:49` recognises ESAI frame-sync branches but **is not a drop-in solution** — its helper mixes instruction and cycle accounting.
6. **Recursive `schedCatchUpDspToDsp` ↔ `execPeripherals` re-entrancy.** Still the largest single profile item; only partially addressed. Preserve the reentrancy guard at `mdhardware.cpp:1559` and delivery ordering.
7. **Peripheral tick coalescing.** `EsxiClock::exec` is the #2 hot symbol. Are peripherals ticked more often than their clock dividers require?

### FINAL VALIDATION — required before claiming success
8. Build the real plugins (`mdJucePlugin_VST3`, `mmJucePlugin_VST3`, `mdJucePlugin_AU`, `mmJucePlugin_AU`). Use `source/pluginTester/latency/` capture commands for both models at 128/256/512/1024 and 44.1/48kHz. Then run **both instruments simultaneously on separate DAW tracks** with hardware MIDI/audio for a documented 10-minute workload; require zero underruns; report the smallest passing buffer. Core-only benchmarks do not prove plugin-level success.

### Steam Deck
`build_linux.sh` exists, CI builds `ubuntu-latest`, VST3/CLAP/LV2 supported, and the `jitops_*_x64.cpp` backend is **more mature** than aarch64. JIT works on Linux — no iOS-style restriction. Shared dispatcher/JIT/peripheral wins carry over. **Do not quote macOS-scaled predictions**; measure natively on the Deck.

---

## 7. Tooling — read before dispatching

- **Codex** (`codex exec -m gpt-6-astra -s workspace-write -c model_reasoning_effort="high"`): works, has credits. **Its sandbox only permits `workdir` and `/tmp`** — it cannot write to `/Volumes/Crucial X9` and will hard-block if a plan requires that. Give it `/tmp` paths and copy results out yourself. Usage limits have repeatedly interrupted long runs; prefer several scoped runs over one large one.
- **Claude Code**: good for verification, measurement and review. Session rate limits have interrupted long agent runs; keep tasks scoped.
- **OpenCode**: currently unusable headless — free tier is TUI-only (`"OpenCode's free tier can only be used from within OpenCode"`), and its Google credential returns `API key not valid`. Needs re-auth to be useful.

**Division that works:** Codex for deep emulator reasoning and planning (it found the DO-loop/Long-Interrupt deadlock and the MOVEP bug); Claude Code for independent verification of numbers and for catching over-claims.

---

## 8. Non-negotiable rules

- **Correctness gates speed.** Deterministic emulator; a faster MD that sequences differently is worthless.
- **Measure before AND after every change.** Report mean *and* p95/max. **Revert anything that doesn't help** — no speculative edits left in the tree.
- **Never add a `Co-authored-by` trailer** (CLAUDE.md forbids it in this repo).
- Never push to a remote. Never `git reset --hard` / `checkout -- .` / `stash` / `clean`.
- ROMs are private — never commit, copy into the repo, or redistribute.
- Internal disk ~5GB free; `/Volumes/Crucial X9` has ~490GB. Do not fill the internal volume — a full disk already caused one misleading build failure.

---

## 9. Current user-facing advice

Until the work lands: **buffer 1024, MD and MM on separate DAW tracks** (separate tracks let the host schedule them on different worker threads; the real-time budget is per core), and **avoid 256**.
