"""tools/run_summary.py: reads a synthetic run directory, computes the
metrics, and flags a regression against a baseline (and not a non-regression)."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "run_summary.py"
sys.path.insert(0, str(ROOT / "tools"))
import run_summary  # noqa: E402


def perf_line(draws, ring, ir, backend, up, reuse):
    return (f"frames 30.0/s ms p50 33.3 p95 33.6 p99 34.0 max 36.0 | swaps 30.0/s | ring {ring} ms "
            f"ir {ir} ms present 1 ms | fence 0 ms (0 waits) | draws {draws}/s resolves 30/s | "
            f"backend {backend} ms wait 0 ms (0) | dedup constants up {up} reuse {reuse} binds set 10 "
            f"skip 30 texmark 5 (2 touched) imload hit 8 miss 2 mirror batches 3 ranges 4")


def make_run(root: Path, interval_ms, spikes=0):
    probes = root / "session" / "errors" / "stamp" / "probes"
    probes.mkdir(parents=True)
    timeline = [f"frame {i} at {i*33.3:.1f} ms: interval {interval_ms if i % 50 or not spikes else 120.0:.1f} ms | draws 1"
                for i in range(300)]
    (probes / "frame_timeline.log").write_text("\n".join(timeline))
    (probes / "perf_frame.log").write_text(
        "\n".join([perf_line(2000, 100, 20, 60, 25, 75), perf_line(20, 1, 0, 1, 5, 5)]))
    (probes / "hitches.log").write_text("+1.000s frame took 70.0 ms (recent median 33.0 ms)\n" * 2)
    (probes / "slow_events.log").write_text(
        "+5.000s over 5 ms in the last 5 s:\n    kernel   NtFoo   x3     total     12.5 ms  max 7.0 ms\n")
    (probes / "wait_profile.log").write_text(
        "thread 17: 1000.5 ms blocked\n      1000.5  5  0  1  2  semaphore\nthread 16: 200.0 ms blocked\n")
    (probes / "guest_profile.log").write_text(
        "== 0s t5 (XAudio mixer): 1000 samples\n"
        "      600  60.0%  821E6AC8\n      400  40.0%  xboxkrnl.exe!KeWaitForSingleObject\n"
        "== 0s t6 (Worker): 1000 samples\n      900  90.0%  82000000\n")
    (root / "host_stderr.txt").write_text(
        "[audio-rate] 5.0s: callbacks 100.0/s, frames\n[audio-rate] 5.0s: callbacks 180.0/s, x\n"
        "[audio-rate] 5.0s: callbacks 190.0/s, x\n[audio-rate] 5.0s: callbacks 186.0/s, x\n")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        good, slow = tmp / "good", tmp / "slow"
        good.mkdir(), slow.mkdir()
        make_run(good, 33.3)
        make_run(slow, 40.0, spikes=1)

        metrics, _ = run_summary.collect(good, 30, r"mix|audio")
        assert metrics["frames"] == 270 and abs(metrics["frame_p50_ms"] - 33.3) < 1e-6
        assert abs(metrics["frame_p99_ms"] - 33.3) < 1e-6
        assert abs(metrics["frame_1pct_low_fps"] - 1000 / 33.3) < 1e-3
        # Only the 2000 draws/s second counts: (100+20) ms / 2000 draws = 60 us; backend 30 us.
        assert abs(metrics["frontend_us_per_draw"] - 60.0) < 1e-6
        assert abs(metrics["backend_us_per_draw"] - 30.0) < 1e-6
        assert metrics["constant_uploads"] == 30 and metrics["constant_reuses"] == 80
        assert metrics["imload_hits"] == 16 and metrics["binds_skipped"] == 60
        assert abs(metrics["constant_reuse_pct"] - 100 * 80 / 110) < 1e-6
        # The first audio window (still starting up) is dropped: mean of 180, 190, 186.
        assert abs(metrics["audio_callbacks_per_s"] - 185.3333333) < 1e-3
        assert metrics["mixer_running_ms"] == 600.0   # only the mixer thread's guest-address samples
        assert abs(metrics["wait_total_ms"] - 1200.5) < 1e-6
        assert metrics["hitches"] == 2 and abs(metrics["slow_event_ms"] - 12.5) < 1e-6

        base = tmp / "base.json"
        out = subprocess.run([sys.executable, str(TOOL), str(good), "--save-baseline", str(base)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        assert json.loads(base.read_text())["metrics"]["frames"] == 270
        # The same run against its own baseline passes.
        same = subprocess.run([sys.executable, str(TOOL), str(good), "--baseline", str(base)],
                              capture_output=True, text=True)
        assert same.returncode == 0 and "REGRESSION" not in same.stdout, same.stdout
        # A slower run with spikes regresses p50/p99/1% low; a huge budget accepts it.
        worse = subprocess.run([sys.executable, str(TOOL), str(slow), "--baseline", str(base)],
                               capture_output=True, text=True)
        assert worse.returncode == 1 and "REGRESSION" in worse.stdout, worse.stdout
        budgets = [a for n in ("frame_p50_ms", "frame_p99_ms", "frame_1pct_low_fps", "hitches")
                   for a in ("--budget", f"{n}=10000")]
        lenient = subprocess.run([sys.executable, str(TOOL), str(slow), "--baseline", str(base)] + budgets,
                                 capture_output=True, text=True)
        assert lenient.returncode == 0, lenient.stdout
        # Bad input is an error, not a summary.
        assert subprocess.run([sys.executable, str(TOOL), str(tmp / "missing")],
                              capture_output=True).returncode == 2
        assert subprocess.run([sys.executable, str(TOOL), str(good), "--budget", "nonsense=5"],
                              capture_output=True).returncode == 2
    print("run_summary tests passed")


if __name__ == "__main__":
    main()
