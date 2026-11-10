"""Run the xboxrecomp analysis pipeline and optionally lift Robotech Battlecry to C."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
toolkit = root / "tools/xboxrecomp"
xbe = root / "game_files/default.xbe"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--lift", action="store_true", help="Generate C after analysis")
parser.add_argument("--after-disasm", action="store_true", help="Reuse existing disassembly")
args = parser.parse_args()

if not xbe.is_file():
    raise SystemExit(f"Missing {xbe}. Run scripts/extract_disc.ps1 first.")
if not toolkit.is_dir():
    raise SystemExit(f"Missing {toolkit}. Run scripts/setup.ps1 first.")

env = os.environ.copy()
env["PYTHONPATH"] = str(root / ".deps") + os.pathsep + env.get("PYTHONPATH", "")
env["PYTHONIOENCODING"] = "utf-8"
(root / "analysis").mkdir(exist_ok=True)
(root / "src/recomp/gen").mkdir(parents=True, exist_ok=True)


def run(module: str, *options: object) -> None:
    log = root / "analysis" / (module.rsplit(".", 1)[-1] + ".log")
    print(f"Running {module}; log: {log}", flush=True)
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(
            [sys.executable, "-m", module, str(xbe), *map(str, options)],
            cwd=toolkit,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
        )


if not args.after_disasm:
    analysis_json = root / "analysis/default_analysis.json"
    run("tools.xbe_parser", "--json", analysis_json)
    run("tools.xbe_parser", "--json", xbe.with_name("default_analysis.json"))
    # Common XDK executable sections; unused names are ignored by the disassembler.
    disasm_opts: list[object] = [
        "--text-only",
        "--extra-sections",
        "D3D,D3DX,DSOUND,XNET,XONLINE,XMV,WMADEC,XGRPH,XACTENG,XPP,DOLBY,XON_RD,"
        "XACTENLT,D3D8LTCG,BINK,BINK32,BINK32A,BINK16,BINK4444,BINK5551,"
        "BINKYUY2,BINK16MX,BINK16X2,BINK16M,BINK32MX,BINK32X2,BINK32M,BINKDATA",
    ]
    seed = root / "config/seed_functions.json"
    if seed.is_file():
        disasm_opts.extend(["--seed-functions", seed])
    run("tools.disasm", *disasm_opts)

run("tools.func_id", "-v")
run("tools.abi_analysis", "-v")

if args.lift:
    run(
        "tools.recomp",
        "--all",
        "--split",
        "250",
        "--game-name",
        "Robotech Battlecry",
        "--gen-dir",
        root / "src/recomp/gen",
    )

print("Pipeline complete.", flush=True)
