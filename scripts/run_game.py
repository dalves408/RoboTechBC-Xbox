"""Launch Robotech Battlecry recomp from the project root with log capture."""
from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--seconds", type=int, default=0, help="Optional watchdog timeout")
parser.add_argument("--label", default="game")
parser.add_argument(
    "--configuration",
    choices=["Debug", "Release", "RelWithDebInfo"],
    default="RelWithDebInfo",
)
args = parser.parse_args()
if not args.label or any(
    c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
    for c in args.label
):
    parser.error("label must contain only letters, numbers, hyphens or underscores")

exe = root / "build" / args.configuration / "robotech_bc_recomp.exe"
if not exe.is_file():
    raise SystemExit(f"Missing {exe}. Run scripts/build.ps1 -Configuration {args.configuration}")

env = os.environ.copy()
if args.seconds:
    env["RECOMP_WATCHDOG_SECS"] = str(args.seconds)
else:
    env.pop("RECOMP_WATCHDOG_SECS", None)
env.setdefault("RECOMP_PB_REPORT_MS", "3000")
env.setdefault("RECOMP_KERNEL_LOG_BUDGET", "50")
# PB_EXEC / FB_WINDOW / etc. are also defaulted inside main.c; keep these
# here so a launch without rebuilding still gets a visible window.
env.setdefault("RECOMP_PB_EXEC", "1")
env.setdefault("RECOMP_PB_WALK", "dma")
env.setdefault("RECOMP_FB_WINDOW", "1")
env.setdefault("RECOMP_KEYBOARD", "1")
env.setdefault("RECOMP_FENCE_EXEC", "1")
env.setdefault("RECOMP_AC97_READY", "1")
env.setdefault("RECOMP_HEAP_RECLAIM", "1")


logs = root / "analysis"
logs.mkdir(exist_ok=True)
with (logs / f"{args.label}.stdout.log").open("wb") as stdout, (
    logs / f"{args.label}.stderr.log"
).open("wb") as stderr:
    process = subprocess.Popen([str(exe)], cwd=root, env=env, stdout=stdout, stderr=stderr)
    (logs / "game.pid").write_text(str(process.pid))
    print(f"PID {process.pid}; logs: analysis/{args.label}.*.log", flush=True)
    print(f"Exit code: {process.wait()}", flush=True)
