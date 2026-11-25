"""Compare the perf counters of two rbx_diag runs (baseline vs. change).

Usage (from repo root):
  python scripts\\diag\\perf_compare.py analysis\\diag\\base analysis\\diag\\xf4

Each argument is a run directory holding report.json from
`rbx_diag.py --gpu-stats`. Prints one row per metric with both values and the
change, so an A/B (for example RECOMP_XF_THREADS=4 or RECOMP_GPU_SKIPRB=1) reads
at a glance.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# (metric, better direction): +1 = higher is better, -1 = lower is better
METRICS = [
    ("guest_fps", +1), ("presented_fps", +1), ("frame_ms_avg", -1), ("frame_ms_worst", -1),
    ("hitches_over_33ms", -1), ("hitches_over_50ms", -1), ("hitches_over_100ms", -1),
    ("readback_ms_per_s", -1), ("shader_compiles", -1), ("shader_compile_ms", -1),
    ("forced_get_catchups", -1), ("fence_early_wakes", -1), ("fence_timeouts", -1),
    ("mmio_traps_per_s", -1), ("apu_lock_waits", -1), ("apu_lock_timeouts", -1),
    ("shader_cache_hits", +1), ("fence_rewait_ms_per_s", -1), ("pb_dropped_walks", -1),
    ("pb_midwalk_get_moves", -1),
]


def load(run: Path) -> dict:
    report = json.loads((run / "report.json").read_text(encoding="utf-8"))
    perf = report.get("perf")
    if not perf:
        raise SystemExit(f"{run}: no perf counters (run rbx_diag.py with --gpu-stats)")
    perf["fps_median_title"] = report.get("fps", {}).get("median")
    perf["cpu_cores_mean"] = report.get("cpu_cores_mean")
    return perf


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    a, b = Path(argv[1]), Path(argv[2])
    pa, pb = load(a), load(b)
    rows = [("metric", a.name, b.name, "change")]
    for key, better in METRICS + [("fps_median_title", +1), ("cpu_cores_mean", -1)]:
        va, vb = pa.get(key), pb.get(key)
        if va is None or vb is None:
            change = ""
        elif va == vb:
            change = "same"
        else:
            delta = vb - va
            pct = f" ({delta / va * 100:+.0f}%)" if va else ""
            tag = "better" if (delta > 0) == (better > 0) else "worse"
            change = f"{delta:+g}{pct} {tag}"
        rows.append((key, str(va), str(vb), change))
    widths = [max(len(r[i]) for r in rows) for i in range(4)]
    for r in rows:
        print("  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
