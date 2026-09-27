# Remote panel v3 wire contract

This implements the **editor-open capture path**, not an offscreen renderer. The Mac owns
synthesis and audio. A v3 viewer displays pixels from the actual Rml component, including its
LCD, LEDs, skin, overlays and pressed states. There is no browser reconstruction of the skin.
Implementation: `mdRemotePanelV3.cpp`, `mdRemotePanelWire.h`, and the existing v2 server.
Runtime qualification is recorded separately in `remote-panel-v3-status.md`.

## Connection and compatibility

Connect a binary RFC 6455 WebSocket to the instance's logged TCP port, any URL path. Default
port search is 7788–7795; use the actual log, not a hard-coded 7789. This is the existing
plain LAN endpoint, with no TLS/authentication added by v3. Client frames must be masked.
All integers below are **little-endian**; signed integers use two's complement. Floats are
IEEE-754 binary32 or binary64, also little-endian. Offsets count from the message type byte.
One application message is one complete WebSocket message. Text messages are JSON diagnostics.

The server still sends `Info = [81, model, 02]` (hex). `02` deliberately remains the legacy
protocol version so every v2 byte layout stays intact. Send **`Hello = [01,03]`** to opt into
v3. `Hello [01,02]` keeps the original LCD-only service. There is no negotiation timeout.
A new viewer must require `PanelSource`; an old server will silently ignore v3 extensions.
Unknown binary types are ignored. A subscribed v3 client continues receiving v2 LCD frames,
acks, stats and pongs, which are useful as independent firmware-state observations.

Sequence: connect → Hello 3 → PanelSource available → collect/decode complete PNG keyframe →
display that keyframe → PanelReady → permit Touch. **Available alone never enables input.**
PanelReady has no response. WebSocket ordering puts subsequent Touch after PanelReady.
Keep sending contacts without waiting for each acknowledgement. Send ReleaseAll on page hide,
loss of focus, renderer reset or a pointer-cancel storm. Use a persistent contact id for each
pointer, and cancel/up its captured owner even if the pointer leaves the displayed image.

## All binary messages

| ID (hex) | Direction | Length | Meaning |
|---|---|---:|---|
| 01 | client → server | 2 | Hello: offset 1 u8 requested protocol, 3 enables capture |
| 10 | client → server | 18 | Button, layout below |
| 11 | client → server | 18 | EncoderDelta, layout below |
| 12 | client → server | 18 | EncoderPress, layout below |
| 13 | client → server | 1 | ReleaseAll: release this session's remote holds and contacts |
| 14 | client → server | 20 | Touch, layout below |
| 15 | client → server | 13 | PanelReady, layout below |
| 20 | client → server | 9 | Ping: offsets 1–8 opaque bytes |
| 81 | server → client | 3 | Info: offset 1 u8 model (0 MD, 1 MM), offset 2 u8 legacy version (2) |
| 82 | server → client | 27 + 64n | Legacy LCD Frame, layout below |
| 83 | server → client | 14 | Legacy Ack, layout below |
| 84 | server → client | 43 | PanelSource, layout below |
| 85 | server → client | 41 + n | PanelChunk, n ≤ 16384 |
| 86 | server → client | 43 | TouchAck, layout below |
| A0 | server → client | 17 | Pong: offsets 1–8 echoed opaque bytes; offset 9 u64 host steady-clock µs |

Legacy inputs with fewer than 18 bytes are ignored; extra bytes retain the old ignored-tail
behavior. Touch requires exactly 20 bytes and acknowledges malformed inputs. For a short
Touch without all sequence bytes, its malformed acknowledgement uses sequence 0.

### Button / EncoderDelta / EncoderPress (10 / 11 / 12)

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u8 | message id |
| 1 | u8 | control or encoder index |
| 2 | u8 / i8 | Button/Press: zero up, nonzero down; Delta: signed detents |
| 3 | u8 | flags; bit 0 arms legacy audio onset probe on Button down; other bits ignored |
| 4 | u16 | contact id, scoped to this connection |
| 6 | u32 | client sequence echoed in Ack |
| 10 | f64 | client milliseconds; currently opaque/unused by server |

Button indices, in order (zero-based):

```
 0–15 Trigger1–Trigger16
16–21 Track1–Track6
22 Tempo              23 SynthesisEffectsRouting   24 Function
25 Kit                26 Enter                     27 Exit
28 Up                 29 Down                      30 Left
31 Right              32 BankGroup                 33 BankA
34 BankB              35 BankC                     36 BankD
37 Record             38 Play                      39 Stop
40 DataPageForward    41 DataPageBackward          42 Scale
43 PatternSong        44 TrigSelect                45 SongEnable
46 ClassicExtended
```

Encoder indices: 0–7 DataEntryA–H, 8 Level, 9 SoundSelection. Other reserved indices or
model-unsupported mappings return Unsupported. Encoder detents use the existing bounded
±1 ingress: burst 8, pending remainder at most 24, reversal cancels pending old direction.
Use small signed deltas; no artificial 33 ms navigation queue is used.

Legacy Ack: offset 1 u32 sequence; 5 u8 status; 6 u32 inputEpoch; 10 u32 receive-to-handled
microseconds. **inputEpoch is ordering metadata, not firmware-consumption or pixel proof.**

### Touch (14)

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u8 | 14 hex |
| 1 | u8 | phase: 0 down, 1 move, 2 up, 3 cancel |
| 2 | u16 | contact id, unique among active contacts in this connection |
| 4 | u32 | client sequence |
| 8 | u32 | geometry generation from the displayed keyframe |
| 12 | f32 | panel-local x in dp |
| 16 | f32 | panel-local y in dp |

NaN, infinity, coordinates outside ±100000, invalid phases, and wrong lengths are malformed.
Down outside the panel is a miss, never clamped. Moves/up/cancel may be outside the panel.
A valid down hit-tests the laid-out Rml document, then walks ancestors to an editor-bound
`remote-slot`. Text/sprite children therefore resolve to their parent control. Only the
existing physical buttons and encoders are mapped; master volume, settings and LCD gesture
shortcuts are not Touch targets. The server retains that owner until up/cancel. Duplicate
down and unmatched move/up/cancel are ignored. A live geometry change cancels all contacts.

Touch buttons use **raw momentary hardware semantics**, including MM bank keys. They do not
invoke the desktop's Shift or MM bank latching convenience. Each (connection, contact)
owns a separate slot reference; the shared scan row is ORed with desktop ownership. Two
contacts on one button produce refs 1 → 2 → 1 → 0, irrespective of release order. No synthetic
single mouse is fed to Rml; doing so would break independent holds.

Encoder movement follows the real knob's configured range and speed:
`valueDelta = (contextDx - contextDy) * range / speed` (modifier = 1; no modifier field).
Accumulate fractional deltas per driving contact; truncate toward zero to whole detents.
Only the first contact on one knob drives motion; additional contacts hold passively and
are not promoted to drivers when it lifts. Independent knobs have independent accumulators.
A stationary hold presses after 400 ms, serviced at 5–20 ms timer opportunities. Moving more than
10 **context units of Manhattan distance** before that disables the pending hold. A held
encoder may subsequently turn while pressed. A quick tap does not press. Passive contacts
share the group's hold decision and keep its press owned until their releases. An unsupported
encoder switch has no physical press. Knob sprite values are updated without dispatching a
second firmware Change event. Large moves use the existing remote detent clamp/remainder;
exact desktop behavior for a huge one-event endless wrap is not claimed.

At most 256 active Touch contacts and 1024 pending value events are allowed globally.
Overflow of the event queue disconnects the flooding session, releasing its holds. Network
threads only parse/enqueue values; hit testing/DOM/gesture state run on the JUCE message
thread under Rml access. ReleaseAll supersedes older queued downs. Do not mix legacy and
Touch ownership using the same contact ids within one connection.

### TouchAck (86)

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u8 | 86 hex |
| 1 | u32 | client sequence |
| 5 | u8 | status |
| 6 | u8 | resolved slot, FF if none; button 0–63, encoder 64 + encoder index |
| 7 | u32 | current geometry generation |
| 11 | u64 | host receive µs |
| 19 | u64 | host resolved µs |
| 27 | u64 | host handled/enqueued µs |
| 35 | u32 | total remote hold references for this slot after handling, all sessions; excludes desktop |
| 39 | i32 | signed drag detents requested by this move (before ingress clamp) |

Timestamps share the host steady-clock origin. Subtraction measures ingress, never compare
absolute host time with an iPad clock. For non-down events, resolved means the owner-thread
handling point, not a fresh hit test. Enqueued is queue handling completion; a registered
encoder down with owners=0 has not pressed the hardware. For rejected/miss/unavailable events
these times do not imply enqueue. Acks do not prove firmware LCD changes.

Statuses for both Ack formats: 0 Rejected (row recovery can remain pending); 1 Accepted;
2 Ignored (duplicate/unmatched/cancelled old event); 3 NoDevice; 4 Unsupported; 5 Clamped;
6 SourceUnavailable/not ready; 7 StaleGeometry; 8 HitMiss; 9 Malformed.

### PanelSource (84)

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u8 | 84 hex |
| 1 | u8 | 0 unavailable, 1 available but requires a decoded keyframe |
| 2 | u8 | reason: 0 live, 1 no editor/detached, 2 hidden/minimized, 3 capture stale, 4 geometry changed/initializing, 5 encoding failure/oversize |
| 3 | u32 | generation |
| 7 | f32 | panel origin x in context pixels |
| 11 | f32 | panel origin y in context pixels |
| 15 | f32 | panel width in dp |
| 19 | f32 | panel height in dp |
| 23 | f32 | context pixels per panel dp |
| 27 | u32 | context viewport width in pixels |
| 31 | u32 | context viewport height in pixels |
| 35 | u32 | encoded raster width in pixels |
| 39 | u32 | encoded raster height in pixels |

Unavailable messages have zero geometry fields; retain no actionable view from them.
The raster contains the **entire Rml viewport**, not the JUCE outer window and not a cropped
1100×570 rectangle. Root size and origin come from the laid-out document; component/window,
panel dp, context and raster dimensions must never be substituted for one another. At most
2560×1600 capture pixels are handed off; larger or dimension-mismatched captures are ignored
and become unavailable via the freshness deadline if previously live. Encoding scales widths
over 1100 down to 1100, with aspect ratio preserved and integer-height truncation. No upscale.
PNG is lossless relative to this final raster; downscaling itself is resampling.

For an image displayed with `object-fit: contain` inside a CSS box `(Bx,By,Bw,Bh)`, calculate:

```
s = min(Bw / rasterW, Bh / rasterH)
L = Bx + (Bw - s*rasterW)/2
T = By + (Bh - s*rasterH)/2
rx = (cssX-L)/s;                 ry = (cssY-T)/s
cx = rx*contextW/rasterW;        cy = ry*contextH/rasterH
panelX = (cx-originX)/dpScale;   panelY = (cy-originY)/dpScale
```

Reject touches in CSS letterboxing. The inverse is:
`cssX = L + s*(originX + panelX*dpScale)*rasterW/contextW`, likewise y.
Keep fractional coordinates. CSS pixels are not device pixels: do not multiply by the iPad's
`devicePixelRatio`. The same formulas work for browser scales 0.75, 1, 1.5 and Retina capture.

### PanelChunk (85) and PanelReady (15)

| Offset | Type | PanelChunk meaning |
|---:|---|---|
| 0 | u8 | 85 hex |
| 1 | u32 | geometry generation |
| 5 | u64 | capture completion sequence; gaps are expected |
| 13 | u64 | firmware LED publication sequence sampled by the editor at capture request |
| 21 | u64 | screenshot callback completion time, host steady-clock µs |
| 29 | u32 | total PNG byte count, ≤ 2097152 |
| 33 | u32 | byte offset in that PNG |
| 37 | u32 | bytes in this chunk, ≤ 16384 |
| 41 | byte[n] | PNG bytes |

Within one keyframe, offsets are contiguous from zero; all headers describe the same frame.
The sequence at 13 is a **sample**, not proof that readback pixels incorporate that firmware
publication. Record v2 snapshots and targeted changed pixels separately for causal evidence.
The completion timestamp is callback delivery, not GPU presentation time. All fields are
explicitly serialized, never a native C++ struct.

Validate limits before allocation. Buffer one incomplete PNG, decode only on exact total
completion, check decoded dimensions, then atomically replace the displayed image. Clear an
incomplete frame on source/generation changes; discard every stale-generation chunk, even if
it arrives after the unavailable notification. Keep the old live image while a next same-
generation frame is incomplete, but visibly disable/blank it as soon as the source is lost.
Every emitted image is an independent keyframe; there are no PNG byte deltas or tile baselines.

After **decoding and displaying** a complete current-generation keyframe, send PanelReady:
offset 0 u8 15 hex, 1 u32 generation, 5 u64 capture sequence. The sequence must be nonzero and
no newer than a fully sent frame for this session and generation. Acknowledge each displayed
keyframe; only the first is needed to unlock interaction in that generation.

Capture cadence is adaptive (see `remote-panel-v3-latency.md`). Idle: one capture per 200 ms
on a 20 ms timer. Fast (a Touch is down, a Touch was just handled, LCD/LED contents changed, or
the last capture's pixels differed; held for 600 ms after the last such signal): at most one
per 33 ms on a 5 ms timer, additionally paced so that a capture is only requested once the
per-client budget below could send the previous keyframe whole. `captureRequested`,
`captureAccepted`, `captureCompleted` are separate cumulative counters. The callback only
bounds/checks/copies pixels into one newest pending capture and wakes the worker. PNG
compression runs on that dedicated worker, separate from input ingress and encoder remainder
draining. A capture whose pixels equal the live keyframe's is neither encoded nor sent. The
PNG is ordinary and lossless (RGB8 when every pixel is opaque, else RGBA8; Up-filtered rows,
zlib level 3); any PNG decoder applies. These are not measured frame-rate or CPU promises.

Each client retains one in-progress immutable PNG plus access to the global newest PNG;
intermediate frames coalesce. A token bucket limits PanelChunk payloads to 1,250,000 B/s
with at most 131,072 B burst credit (one whole keyframe), initially 16,384 B. One ≤16,425 B message is sent between
control-queue drains. Control traffic, source messages, WebSocket/TCP headers and legacy LCD
frames are outside this budget. A two-second socket send timeout bounds a stalled write;
chunks still cannot preempt bytes already in TCP, so zero ack latency is **not** promised.
`panelBytes`/`panelFrames` count built chunks/keyframes, not confirmed browser delivery; the
probe measures bytes actually received. V2 `framesSent/s` and `KB/s` retain their v2 scope.

## Legacy LCD Frame (82)

Offset 1 u32 snapshot sequence; 5 u32 inputEpoch; 9 u16 LED-written mask; 11 u16 tile mask;
13–26 fourteen raw active-low LED bytes for banks 0x20–0x2D; offset 27 onward, each set tile
bit in ascending order contributes 64 LCD bytes. Tile index = half*8+page. Pixel (x,y),
x=0..127, y=0..63, uses byte `((x>>6)*8+(y>>3))*64+(x&63)`, bit `y&7` (LSB at top).
First frame on connection contains all 16 tiles. The sequence increments on observed display
changes, not every firmware cycle. V2 remains available to legacy clients with no open editor;
v3 subscribers' legacy Button/Encoder inputs are also gated while their captured view is dead.

## JSON diagnostics

Existing text `{"t":"stats",...}` remains unchanged (framesPerSec is the v2 rate).
New `{"t":"panelControls","generation":N,"controls":[...]}` contains `slot`, `id`,
`x`, `y`, `w`, `h` for each bound control, derived from actual laid-out border rectangles
in panel dp. It is emitted after Hello 3 and geometry changes, and can arrive before a
keyframe. This is for centre-hit testing/diagnostics, **not a client-side hit-testing map**.
Other keys/types may be added; unknown text diagnostics must be ignored. Probe samples for
labels/LCD/LED are MD-specific and must be visually checked before interpreting MM results.

## Source loss and recovery

Detach/hidden/minimized detection releases all remote rows and encoder remainder ownership.
If capture callback completion is older than 750 ms, an independent worker marks source
unavailable, even if the UI thread is stalled. DOM cleanup runs when its owner next executes;
firmware holds are released without waiting for it. Stale/outdated touches are acknowledged
as unavailable or stale and do not reach firmware. No competing LED transition consumer is
introduced: the desktop remains the sole drain and its composed pixels are captured.

Availability and chunks are serialized by the same per-client writer, avoiding source-state
ordering races. On recovery: new generation → available → complete fresh keyframe → PanelReady.
A connection loss is itself unavailable; the viewer must release its pointer bookkeeping and
wait for a new keyframe after reconnecting. A whole-process suspension cannot send a state
message: the viewer should ping periodically (e.g. every 500 ms) and disable the view after
1.5 s without a pong. An unchanged image is **not** a timeout by itself, because idle frames
are suppressed. No host-clock subtraction across machines is needed for that watchdog.

Closed-editor rendering is not implemented. A standalone exit also ends audio/the endpoint;
no claim is made that closing its last window keeps the processor alive. Test open, obscured,
minimized, sleep, closed and reopened individually; see the status document for what was run.
