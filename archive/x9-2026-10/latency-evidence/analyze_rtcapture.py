#!/usr/bin/env python3
"""Offline analysis of Gearmulator realtime callback capture.

Reads ~/Documents/rtcapture-<pid>.bin (24-byte records, written by synthLib/callbackCapture) and
reports the true deadline distribution, which the sampled RealtimeInstrumentation report cannot
give because it biases towards slow/interesting callbacks.

Record: uint64 startNs, uint32 durationNs, uint32 frames, uint32 sampleRate, uint32 reserved.
"""
import glob
import statistics
import struct
import sys

REC = struct.Struct("<QIIII")
assert REC.size == 24


def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = max(0, min(len(s) - 1, int(round((p / 100.0) * (len(s) - 1)))))
    return s[k]


def main(path):
    raw = open(path, "rb").read()
    whole = len(raw) - (len(raw) % REC.size)
    recs = [REC.unpack_from(raw, off) for off in range(0, whole, REC.size)]

    dur_ms, ratio, gaps_ms, budgets = [], [], [], []
    over = 0
    longest_late = cur_late = 0
    rates, frames_seen = set(), set()

    prev_start = None
    for start_ns, dur_ns, frames, rate, _ in recs:
        if frames == 0 or rate == 0:
            continue
        rates.add(rate)
        frames_seen.add(frames)
        budget_ms = (frames / rate) * 1000.0
        d = dur_ns / 1e6
        dur_ms.append(d)
        budgets.append(budget_ms)
        ratio.append(d / budget_ms)
        if d > budget_ms:
            over += 1
            cur_late += 1
            longest_late = max(longest_late, cur_late)
        else:
            cur_late = 0
        if prev_start is not None and start_ns > prev_start:
            gaps_ms.append((start_ns - prev_start) / 1e6)
        prev_start = start_ns

    print(f"file            {path}")
    print(f"complete records{len(recs):>14}   (ignored {len(raw) - whole} trailing bytes)")
    if not dur_ms:
        print("no usable records")
        return
    info = path.rsplit(".", 1)[0] + ".txt"
    try:
        print("--- companion info ---")
        print(open(info).read().strip()[:800])
    except OSError:
        pass
    print(f"sample rates    {sorted(rates)}   block sizes {sorted(frames_seen)}")
    budget = statistics.median(budgets)
    span_s = (recs[-1][0] - recs[0][0]) / 1e9 if len(recs) > 1 else 0.0
    print(f"budget          {budget:.3f} ms   span {span_s:.1f} s   "
          f"avg {len(dur_ms) / span_s:.0f} callbacks/s" if span_s else "")
    print()
    print("                 p50       p95       p99       max")
    print(f"duration ms   {pct(dur_ms,50):9.3f} {pct(dur_ms,95):9.3f} "
          f"{pct(dur_ms,99):9.3f} {max(dur_ms):9.3f}")
    print(f"budget ratio  {pct(ratio,50):9.3f} {pct(ratio,95):9.3f} "
          f"{pct(ratio,99):9.3f} {max(ratio):9.3f}")
    if gaps_ms:
        print(f"start gap ms  {pct(gaps_ms,50):9.3f} {pct(gaps_ms,95):9.3f} "
              f"{pct(gaps_ms,99):9.3f} {max(gaps_ms):9.3f}")
    print()
    print(f"callbacks over their own budget : {over} / {len(dur_ms)} "
          f"({100.0 * over / len(dur_ms):.2f}%)")
    print(f"longest consecutive late run    : {longest_late}")
    print()
    if over == 0 and pct(ratio, 99) < 0.80:
        print("VERDICT: no deadline misses observed, p99 inside the 80% margin.")
    elif over == 0:
        print("VERDICT: no deadline misses observed, but p99 is inside 80% of budget only "
              "marginally - treat the margin as thin.")
    else:
        print("VERDICT: callbacks exceeded their deadline in this window -> this block size "
              "is not safe for playing regardless of reported output latency.")


if __name__ == "__main__":
    targets = sys.argv[1:] or sorted(glob.glob("/Users/michaelmeneses/Documents/rtcapture-*.bin"))
    if not targets:
        print("no rtcapture-*.bin found (was the app launched with GEARMULATOR_RT_FULL_CAPTURE=1?)")
    for t in targets:
        main(t)
        print()
