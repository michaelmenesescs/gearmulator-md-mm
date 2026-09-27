# Remote panel v3 — status, verified by the orchestrating agent

The agent that wrote the v3 code was killed by a usage limit mid-run (codex quota, 2026-09-26
~22:15) **during its own build step and before it wrote this document or its patch**. Everything
below was therefore produced and measured by a different agent, independently of the author's own
claims. The author's unit tests (`tools/remote-panel-v3/wire_test.cpp`) report PASS but were not
re-run or audited here; treat them as unverified.

Contract: `doc/remote-panel-v3.md`. Client: `/Users/michaelmeneses/gearmulator-ipad-panel/web/panel3.js`
(written against that contract, served by the plugin at the web root).

## What was verified, with the actual evidence

Build: the tree compiles clean from a fresh invocation — `[100%] Built target mdJucePlugin_Standalone`,
zero errors. The author's own build log stopped at 36%, so this was the first proof it builds at all.

Handshake and decode (MD standalone, host loopback, viewer in a browser):
- `Hello [01,03]` → `PanelSource available (#2/#3)` → PNG keyframe assembled from chunks → decoded →
  displayed → `PanelReady` → input unlocked. The client reported `LIVE md v3`.
- Geometry published: origin (0,0), panel dp/px **0.938**, context **1032x535**, raster **1032x535**
  (matches 1100x570 dp x 0.938).
- The decoded image is the real panel, not a flat or blank frame: **164 distinct sampled colours**,
  and skimming the raster shows skin chrome, labels and the live LCD.

Touch resolution — the failure that sank the previous client is gone. A touch resolves through the
plugin's own hit-testing of its laid-out document:
| touch (panel dp) | TouchAck |
|---|---|
| (791, 391) | `accepted slot 38` — that is Play |
| (69, 493) | `accepted slot 0` — that is Trigger1 |
| (969, 493) | `accepted slot 15` — that is Trigger16 |
| encoder A centre + drag | `accepted slot 64` — encoder 0, DataEntryA |

Multi-contact ownership, the hazard the earlier design would have tripped:
- two contacts on ONE key: refs **1 → 2 → 1 → 0**, every event `accepted`, irrespective of order.
- two different keys held at once: both held (contacts 2), then released; **0 orphan contacts**.
- while held, the displayed panel pixels **change** — canvas hash 2475383863 → 1550128302 — i.e. the
  real pressed styling comes from the plugin's own rendering, not from client drawing.

Latency (host-side, from TouchAck's u64 timestamps — differences only, never cross-clock):
- receive→resolved **med 22 µs**, receive→handled **med 25 µs**.
- p95 ~**9.9 ms**. That is consistent with input being sampled at the audio block boundary
  (512 frames @ 44.1 kHz ≈ 11.6 ms), not with a stall.

Idle cost, which is the check that separates a real diff stream from a video stream:
- with a viewer attached and nothing happening, over 30 s: `captureRequested` **1738 → 2105** but
  `panelBytes` **frozen at 926754** and `panelFrames` **frozen at 12** → **zero bytes sent while the
  panel is unchanged**.
- zero chunk drops and zero decode failures across all of the above.

## Measured cost, stated plainly

- CPU with a viewer attached: **~45%** of a core (RSS ~956 MB). Without any viewer: **~14%**.
  So a connected panel costs roughly **30 percentage points of one core even when completely idle**,
  because capture, PNG encode and comparison keep running at ~12 captures/s while only the *sending*
  is suppressed. Astra predicted exactly this ("zero wire bytes with full software rasterization is
  not a cheap idle implementation"). It needs an idle backoff or on-demand capture before this is
  something to leave running all day.
- The capture loop **stops entirely** when no v3 client is subscribed (`captureRequested` frozen at
  2463 over 20 s with `clients=0`), so an unused plugin is not paying for this.
- During active use the viewer saw ~1.5 panel frames/s and ~113 kB/s, i.e. the byte budget is doing
  its job; these are short-window figures, not a sustained-load measurement.

## Not verified — do not assume these

- **The real iPad.** Every number above is host-side loopback; no device touch, no Wi-Fi round trip,
  no iOS WebKit behaviour. The device test is the human's, and it is the next step.
- **Source loss and the honest-degradation path** (editor hidden / minimized / closed / reopened,
  screen sleep). The contract specifies the behaviour and the client implements it, but it was not
  exercised. Closing a standalone window may also end the process, so this needs care.
- **Audio under streaming.** No A/B of audio callback timing or xruns with the stream on vs off. The
  Mac's audio is the product's other half and this is an open risk.
- **MM**, Retina/density variants, browser display scales 0.75/1.5, letterboxed edge cases,
  simultaneous desktop + remote LED behaviour, and long-run memory/leaks.
- The author's own unit tests and its claimed 1.25 MB/s budget behaviour.

## Known state left behind

- Port moved: this instance listens on **7788** (the earlier one used 7789 because an older
  Gearmulator instance held 7788).
- `web/panel.js` (the v2 canvas client) is no longer referenced by `index.html`; `panel3.js` is.
- Closed-editor rendering is not implemented, as the contract states.