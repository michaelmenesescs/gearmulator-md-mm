# Remote panel v3: latency work (2026-09-27)

Goal: reduce felt latency of the v3 streamed panel without touching audio, without changing
the wire byte layout, and without giving up "idle viewer ⇒ zero PanelChunk bytes".

Constraint during this work: a live host (PID 91725) was in use from the iPad. It was never
killed, restarted or signalled. **No live end-to-end number in this document was measured on
the new code; every end-to-end figure is DEFERRED until the user restarts the host.**

## What the code actually cost (read before choosing)

Per capture, `mdRemotePanelV3.cpp` did: Rml render + GPU readback + swizzle (message thread),
`createCopy` in the callback, then on the worker `juce::PNGImageFormat` (libpng defaults: zlib
level 6, adaptive filters, RGBA with a per-pixel unpremultiply) on **every** capture, and only
then compared PNG bytes to drop duplicates. Captures ran every 66.7 ms on a 20 ms timer, i.e.
every fourth tick = 80 ms = 12.5/s, which matches the measured ~12 frames/s. The worker also
slept 5 ms between polls. Separately, the per-client `PanelBudget` (1.25 MB/s, 32 KiB burst)
needs ~37 ms to release one ~79 kB keyframe even on a quiet link.

## What changed

1. **Pixel dedup before encode** (`samePixels`, worker). A capture equal to the live keyframe
   is dropped after a ~0.1 ms compare instead of a ~33 ms encode. Same "no bytes when equal"
   behaviour as before, now decided on pixels rather than on PNG bytes.
2. **Panel PNG writer** (`mdRemotePanelPng.{h,cpp}`, JUCE's zlib, no new dependency). Ordinary
   lossless PNG: RGB8 when every pixel is opaque (no unpremultiply), else RGBA8 unpremultiplied
   exactly like JUCE; Up-filtered rows; zlib level 3. Decoded pixels are identical to the JUCE
   writer's round trip (tested on three real keyframes, a translucent and an RGB-format image).
   Chosen over a smaller raster (visible quality loss on the iPad) and over a wire change
   (dirty rectangles would need a new client contract). Level 3 instead of 1: +0.8 ms encode
   for −3 kB per keyframe.
3. **Adaptive cadence** (`mdRemotePanelCapturePolicy.h`, message thread). Idle: one capture
   per 200 ms on a 20 ms timer (inside the 750 ms freshness deadline with margin). Fast: ≤ one
   per 33 ms on a 5 ms timer while a Touch is down, after a handled Touch (the timer is switched
   to 5 ms immediately), after an LCD/LED *content* change seen by the pump, or after the
   worker published changed pixels; decays to idle 600 ms after the last signal. The editor's
   LED publication sequence is **not** used: it counts LED bank writes, not changes.
4. **Budget-aware pacing + burst.** At 30 captures/s × ~81 kB the unchanged 1.25 MB/s budget
   would make every frame trickle out (the model showed frame age *rising* 72 → 88 ms). So:
   the policy mirrors the budget and only requests a fast capture once the previous
   keyframe's size could leave in one go; and the burst cap is raised 32 KiB → 128 KiB (one
   whole keyframe). **The sustained rate is unchanged at 1.25 MB/s**, so the average Wi-Fi load
   ceiling is the one already verified with the iPad.
5. The worker is woken by a condition variable from the screenshot callback instead of
   polling every 5 ms.

Threads: capture/encode stay off the audio thread (message thread + worker, as before). Touch
handling does not wait on capture: it only records activity and restarts the timer.

Wire: **no byte layout change.** `panel3.js` decodes any PNG via `createImageBitmap`/`Image`
and was not modified. `doc/remote-panel-v3.md` text updated for cadence, PNG form and burst.

## Measured (standalone harness, real keyframes, host untouched)

`mdRemotePanelPngTest` on three 1032×535 keyframes captured passively from the live host
(receive-only viewer: Hello 3, no PanelReady, no Touch; `tools/remote-panel-v3/passive_grab.py`).
Full output: `tools/remote-panel-v3/latency-evidence/bench-final.txt`. Frame00, ms median / p95:

| Worker work per capture | Before | After |
|---|---:|---:|
| Changed pixels (encode) | 32.6 / 44.8 (78 654 B) | 5.8 compare+encode (80 999 B) |
| Unchanged pixels | 32.6 (encode, then dropped) | 0.09 (compare only) |
| Callback `createCopy` (unchanged) | 0.19 | 0.19 |

Other frames agree within ~1 ms. Idle-bytes check (policy + dedup over 30 s of identical
captures): 150 captures, 0 encodes, **0 PanelChunk bytes**.

## Modelled, not measured end-to-end

The harness composes the measured encode medians with the real `CapturePolicy`, the old
cadence rule and the token bucket. It **excludes** render/readback, Wi-Fi and iPad decode.

| Host-side change → last PanelChunk byte released (ms, median / p95) | Before | After |
|---|---:|---:|
| Touch (pressed state), single change after quiet | 111 / 148 | 11 / 31 |
| — of which change → capture request | 39 / 76 | 5 / 25 |
| Firmware LCD/LED change, single | 111 / 148 | 17 / 31 |
| Sustained change: keyframes/s, frame age | 12.5/s, 72 | 15.5/s, 8 |

Under sustained change the 1.25 MB/s budget, not the 30/s capture target, bounds delivered
keyframes to ~15/s at ~81 kB each. 25–30/s delivered would need ~2.4 MB/s; raising the
sustained rate is the next lever and needs a Wi-Fi measurement with the iPad first.

## Build

`cmake --build build/macos-md-arm64 --target mdJucePlugin_Standalone -j 8` (run under
`taskpolicy -b nice -n 20` so it could not compete with the live audio) → `[100%] Built target
mdJucePlugin_Standalone`, exit 0. Before building, the running host's executable (inode
79599516) was **renamed** to `build/macos-md-arm64/live-host-91725-Gearmulator-MD.bin` and a
copy put in the bundle, so the linker/codesign could never write the mapped file; `lsof`
after the build shows PID 91725 still on inode 79599516. Delete that file after the host exits.

## Unverified / deferred

- Live press-to-displayed-frame on the iPad with the new build (needs a host restart).
- Live CPU at block 128/512 with a viewer. Expected lower at idle (5 vs 12.5 captures/s, no
  idle encodes) and in fast mode per frame, but render + GPU readback per capture was not
  measured (needs the live GPU context) and fast mode can request up to ~28 captures/s.
- The 30 s zero-bytes property on the live host (the harness proves the logic only).
- Whether the 128 KiB burst causes ack delay on a weak Wi-Fi link (sustained rate unchanged).
- Render wait between a capture request and the screenshot callback (≤ one Rml frame).
