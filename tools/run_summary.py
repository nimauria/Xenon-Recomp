#!/usr/bin/env python3
"""Summarise one Xenon run from its logs, and check it against a baseline.

    py tools/run_summary.py <run directory> [--baseline base.json] [--save-baseline base.json]
                            [--budget metric=percent ...] [--json] [--skip-frames N]
                            [--mixer-thread REGEX]

The run directory is searched recursively (the probe directory is usually
<run>/session/errors/<stamp>/probes); where a log exists more than once the
newest is used. Logs read:

  frame_timeline.log   per-frame intervals     -> frame-time p50/p99, 1% low
  perf_frame.log       one line a second       -> per-draw cost, dedup uploads/reuses
                                                  (falls back for frame times)
  [audio-rate] lines   (any *.txt / *.log)     -> audio callbacks/s
  guest_profile.log    guest thread samples    -> mixer thread running ms
  wait_profile.log     blocked guest waits     -> wait totals
  hitches.log, slow_events.log, bottlenecks.log

Metrics missing from a run are reported as "n/a" and skipped in comparisons.

Baseline check: each metric has a direction (lower or higher is better) and a
default budget of 10%; --budget name=percent overrides one. A metric that is
worse than the baseline by more than its budget is a regression and makes the
exit status 1.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

# name -> (label, unit, better, default budget percent)
METRICS = {
    "frame_p50_ms": ("frame time p50", "ms", "lower", 10.0),
    "frame_p99_ms": ("frame time p99", "ms", "lower", 10.0),
    "frame_1pct_low_fps": ("1% low", "fps", "higher", 10.0),
    "frames": ("presented frames", "", "info", 0.0),
    "audio_callbacks_per_s": ("audio callbacks/s", "/s", "higher", 5.0),
    "mixer_running_ms": ("mixer thread running", "ms", "lower", 15.0),
    "frontend_us_per_draw": ("frontend (ring+IR) per draw", "us", "lower", 10.0),
    "backend_us_per_draw": ("backend per draw", "us", "lower", 10.0),
    "constant_uploads": ("constant slots uploaded", "", "info", 0.0),
    "constant_reuses": ("constant slots reused", "", "info", 0.0),
    "binds_set": ("state sets recorded", "", "info", 0.0),
    "binds_skipped": ("state sets skipped", "", "info", 0.0),
    "imload_hits": ("IM_LOAD cache hits", "", "info", 0.0),
    "imload_misses": ("IM_LOAD cache misses", "", "info", 0.0),
    "constant_reuse_pct": ("constant reuse", "%", "higher", 10.0),
    "wait_total_ms": ("guest wait total (all threads)", "ms", "info", 0.0),
    "hitches": ("hitch frames", "", "lower", 25.0),
    "slow_event_ms": ("slow kernel events total", "ms", "lower", 25.0),
}


def newest(root: Path, name: str):
    found = [p for p in root.rglob(name) if p.is_file()]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def read_lines(path):
    if path is None:
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def percentile(sorted_values, fraction):
    if not sorted_values:
        return None
    index = min(len(sorted_values) - 1, max(0, math.ceil(fraction * len(sorted_values)) - 1))
    return sorted_values[index]


def frame_stats(intervals):
    """p50, p99 and the 1% low (average fps of the slowest 1% of frames)."""
    if not intervals:
        return {}
    ordered = sorted(intervals)
    worst = ordered[-max(1, len(ordered) // 100):]
    mean_worst = sum(worst) / len(worst)
    return {
        "frame_p50_ms": percentile(ordered, 0.50),
        "frame_p99_ms": percentile(ordered, 0.99),
        "frame_1pct_low_fps": 1000.0 / mean_worst if mean_worst > 0 else None,
        "frames": len(intervals),
    }


def parse_frame_timeline(lines, skip):
    intervals = []
    for line in lines:
        m = re.match(r"frame (\d+) at [\d.]+ ms: interval ([\d.]+) ms", line)
        if m and int(m.group(1)) >= skip:
            intervals.append(float(m.group(2)))
    return intervals


def num(pattern, line, default=0.0):
    m = re.search(pattern, line)
    return float(m.group(1)) if m else default


def parse_perf_frame(lines):
    """Per-second perf_frame.log lines -> totals and per-draw costs."""
    out = {}
    draws = frontend_ms = backend_ms = 0.0
    sums = dict(constant_uploads=0, constant_reuses=0, binds_set=0, binds_skipped=0,
                imload_hits=0, imload_misses=0)
    p50s, p99s, seen = [], [], 0
    for line in lines:
        if not line.startswith("frames "):
            continue
        seen += 1
        rate = num(r"draws ([\d.]+)/s", line)
        if rate >= 100.0:  # a second of real rendering, not a menu or a load
            draws += rate
            frontend_ms += num(r"\| ring ([\d.]+) ms", line) + num(r" ir ([\d.]+) ms", line)
            backend_ms += num(r"\| backend ([\d.]+) ms", line)
        p50s.append(num(r" p50 ([\d.]+)", line))
        p99s.append(num(r" p99 ([\d.]+)", line))
        for key, pattern in (("constant_uploads", r"constants up (\d+)"),
                             ("constant_reuses", r"reuse (\d+) binds"),
                             ("binds_set", r"binds set (\d+)"),
                             ("binds_skipped", r"skip (\d+) texmark"),
                             ("imload_hits", r"imload hit (\d+)"),
                             ("imload_misses", r"miss (\d+) mirror")):
            sums[key] += int(num(pattern, line))
    if draws > 0:
        out["frontend_us_per_draw"] = 1000.0 * frontend_ms / draws
        out["backend_us_per_draw"] = 1000.0 * backend_ms / draws
    if seen:
        out.update(sums)
        total = sums["constant_uploads"] + sums["constant_reuses"]
        if total:
            out["constant_reuse_pct"] = 100.0 * sums["constant_reuses"] / total
        out["_perf_p50"] = sorted(p50s)[len(p50s) // 2]
        out["_perf_p99"] = max(p99s)
    return out


def parse_audio_rate(texts):
    rates = []
    for text in texts:
        for m in re.finditer(r"\[audio-rate\] [\d.]+s: callbacks ([\d.]+)/s", text):
            rates.append(float(m.group(1)))
    # The first window runs while the title is still starting up.
    if len(rates) > 2:
        rates = rates[1:]
    return sum(rates) / len(rates) if rates else None


def parse_mixer_ms(lines, thread_regex):
    """Running samples (a guest address, not a kernel export) of the threads
    whose name matches: the guest profile samples each thread every ms."""
    pattern = re.compile(thread_regex, re.I)
    total = 0
    active = False
    for line in lines:
        m = re.match(r"== \d+s t\d+(?: \((.*)\))?: \d+ samples", line)
        if m:
            active = bool(m.group(1) and pattern.search(m.group(1)))
            continue
        if active:
            m = re.match(r"\s+(\d+)\s+[\d.]+%\s+([0-9A-F]{8})\s*$", line)
            if m:
                total += int(m.group(1))
    return total if lines else None


def parse_wait_profile(lines):
    """Total blocked ms over all guest threads, and the three longest."""
    threads = []
    for line in lines:
        m = re.match(r"thread (\d+): ([\d.]+) ms blocked", line)
        if m:
            threads.append((float(m.group(2)), int(m.group(1))))
    if not lines:
        return None, []
    return sum(t[0] for t in threads), sorted(threads, reverse=True)[:3]


def parse_hitches(lines):
    return sum(1 for line in lines if re.match(r"\+[\d.]+s frame took", line))


def parse_slow_events(lines):
    return sum(float(m.group(1)) for line in lines
               for m in [re.search(r"total\s+([\d.]+) ms", line)] if m)


def collect(root: Path, skip: int, mixer_regex: str):
    metrics = {}
    notes = []
    timeline = newest(root, "frame_timeline.log")
    perf = newest(root, "perf_frame.log")
    perf_data = parse_perf_frame(read_lines(perf))
    intervals = parse_frame_timeline(read_lines(timeline), skip)
    if intervals:
        metrics.update(frame_stats(intervals))
    elif "_perf_p50" in perf_data:
        metrics["frame_p50_ms"] = perf_data["_perf_p50"]
        metrics["frame_p99_ms"] = perf_data["_perf_p99"]
        notes.append("frame times from perf_frame.log (no frame_timeline.log): "
                     "p99 is the worst second's p99; no 1% low")
    for key, value in perf_data.items():
        if not key.startswith("_"):
            metrics[key] = value
    texts = []
    for pattern in ("*.txt", "*.log"):
        for path in root.rglob(pattern):
            if path.is_file() and path.stat().st_size < 64 * 1024 * 1024:
                texts.append(path.read_text(encoding="utf-8", errors="replace"))
    audio = parse_audio_rate(texts)
    if audio is not None:
        metrics["audio_callbacks_per_s"] = audio
    guest = newest(root, "guest_profile.log")
    mixer = parse_mixer_ms(read_lines(guest), mixer_regex)
    if mixer is not None:
        metrics["mixer_running_ms"] = float(mixer)
        notes.append(f"mixer thread = guest threads named /{mixer_regex}/; ms is the "
                     "guest profile's running samples (top 8 locations per second)")
    waits, top = parse_wait_profile(read_lines(newest(root, "wait_profile.log")))
    if waits is not None:
        metrics["wait_total_ms"] = waits
        if top:
            notes.append("longest waiting threads: " +
                         ", ".join(f"t{t} {ms:.0f} ms" for ms, t in top))
    hitches = newest(root, "hitches.log")
    if hitches:
        metrics["hitches"] = parse_hitches(read_lines(hitches))
    slow = newest(root, "slow_events.log")
    if slow:
        metrics["slow_event_ms"] = parse_slow_events(read_lines(slow))
    return metrics, notes


def fmt(name, value):
    if value is None:
        return "n/a"
    unit = METRICS[name][1]
    if isinstance(value, float) and not float(value).is_integer():
        return f"{value:.2f} {unit}".strip()
    return f"{int(value)} {unit}".strip()


def compare(metrics, baseline, budgets):
    """Rows of (name, base, now, change percent, verdict)."""
    rows = []
    for name, (label, unit, better, default_budget) in METRICS.items():
        if better == "info":
            continue
        now, base = metrics.get(name), baseline.get(name)
        if now is None or base is None:
            continue
        budget = budgets.get(name, default_budget)
        change = 0.0 if base == 0 and now == 0 else (
            math.inf if base == 0 else 100.0 * (now - base) / abs(base))
        worse = change > budget if better == "lower" else -change > budget
        rows.append((name, base, now, change, "REGRESSION" if worse else "ok"))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("run", type=Path)
    parser.add_argument("--baseline", type=Path, help="baseline JSON to compare against")
    parser.add_argument("--save-baseline", type=Path, help="write this run's metrics as JSON")
    parser.add_argument("--budget", action="append", default=[], metavar="NAME=PERCENT")
    parser.add_argument("--skip-frames", type=int, default=30,
                        help="frames at the start of frame_timeline.log to ignore (default 30)")
    parser.add_argument("--mixer-thread", default=r"mix|audio|xaudio|sound",
                        help="regex for the guest mixer thread's name")
    parser.add_argument("--json", action="store_true", help="print metrics as JSON")
    args = parser.parse_args(argv)

    if not args.run.is_dir():
        print(f"run_summary: {args.run} is not a directory", file=sys.stderr)
        return 2
    budgets = {}
    for item in args.budget:
        name, _, percent = item.partition("=")
        if name not in METRICS or not percent:
            print(f"run_summary: bad --budget {item!r}", file=sys.stderr)
            return 2
        budgets[name] = float(percent)

    metrics, notes = collect(args.run, args.skip_frames, args.mixer_thread)
    if args.json:
        print(json.dumps({"metrics": metrics}, indent=2, sort_keys=True))
    else:
        print(f"run summary: {args.run}")
        for name, (label, unit, better, budget) in METRICS.items():
            print(f"  {label:<34} {fmt(name, metrics.get(name))}")
        for note in notes:
            print(f"  note: {note}")
    if args.save_baseline:
        args.save_baseline.write_text(json.dumps({"metrics": metrics}, indent=2, sort_keys=True))
        print(f"baseline written to {args.save_baseline}")
    if args.baseline:
        baseline = json.loads(args.baseline.read_text())["metrics"]
        rows = compare(metrics, baseline, budgets)
        print(f"comparison with {args.baseline}:")
        bad = 0
        for name, base, now, change, verdict in rows:
            bad += verdict != "ok"
            print(f"  {METRICS[name][0]:<34} {fmt(name, base):>14} -> {fmt(name, now):>14} "
                  f"({change:+.1f}%, budget {budgets.get(name, METRICS[name][3]):.0f}%) {verdict}")
        if not rows:
            print("  (no metric in common with the baseline)")
        if bad:
            print(f"{bad} metric(s) over budget")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
