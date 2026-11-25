"""Robotech Battlecry recomp diagnostic harness.

Launches the recompiled game with the standard "play" environment and samples it
once per interval, then writes a report under analysis\\diag\\<name>\\:

  * FPS / draw count parsed from the window title (host present loop updates it)
  * process CPU, working set, thread count, per-thread CPU hot list
  * window responsiveness (IsHungAppWindow) and frame-progress hang detection
  * crash detection (exit code, fault strings in stderr)
  * screenshots of the client area; luma / bright-wash / frozen-frame metrics
  * stderr anomaly scan (UNIMPL, unhandled GPU methods, faults, stubs)
  * [RBX DX11] per-interval pipeline timings (scan/flip/draw us)
  * [RBXPERF] frame times, hitches, readback/shader-compile cost, forced GET catch-ups,
    fence timeouts, trapped register accesses and APU voice-lock waits (--gpu-stats);
    written to perf.csv and summarised in the report
  * optional timed key script (controls test) and WAV dump of audio output

Usage (from repo root):
  python scripts\\diag\\rbx_diag.py --secs 120 --name smoke
  python scripts\\diag\\rbx_diag.py --secs 90 --keys "40:enter:200,45:space:200" --name menu
  python scripts\\diag\\rbx_diag.py --secs 90 --audio --name audio
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import statistics
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "build" / "RelWithDebInfo" / "robotech_bc_recomp.exe"
OUT_ROOT = ROOT / "analysis" / "diag"
sys.path.insert(0, str(Path(__file__).resolve().parent))  # stackdump.py

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PLAY_ENV = {
    "RECOMP_USB": "1", "RECOMP_USB_PADS": "1", "RECOMP_CONTIG_AVOID_IMAGE": "1",
    "RECOMP_KEYBOARD": "1", "RECOMP_KEYBOARD_FOCUS": "0", "RECOMP_FB_WINDOW": "1",
    "RECOMP_D3D_PRESENT": "1", "RECOMP_SWM5_PACE_MS": "0", "RECOMP_FLIP_PACING": "0",
    "RECOMP_INPUT_DIAG": "1", "RECOMP_KEY_TRACE": "1", "RECOMP_USB_STATS": "1",
}
# Variables that must never be inherited (README "hard constraints").
CLEAR_ENV = [
    "RECOMP_USB_SYNTH_TD", "RECOMP_USB_XAPI_TD", "RECOMP_PAD_SCRIPT", "RECOMP_PAD_LIVE",
    "RECOMP_PAD_PRESS", "RECOMP_KEY_SCRIPT", "RECOMP_WATCHDOG_SECS", "RECOMP_FMV_DUMP",
    "RECOMP_SWM5_PACE_ENABLE", "RECOMP_SWM5_PACE_THREAD", "RECOMP_VBLANK_THREAD",
    "RECOMP_PAD_HOLD_MASK", "RECOMP_PAD_HOLD_ENABLE", "RECOMP_GPU_SKIPBLACK",
    "RECOMP_WATCH", "RECOMP_KERNEL_WATCH", "RECOMP_PEEK", "RECOMP_TIMER_DPC_MS",
]

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsHungAppWindow.argtypes = [wt.HWND]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.IsWindowVisible.argtypes = [wt.HWND]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.c_void_p] * 4

VK = {
    "enter": 0x0D, "space": 0x20, "esc": 0x1B, "z": 0x5A, "x": 0x58, "c": 0x43, "v": 0x56,
    "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "delete": 0x2E,
    "w": 0x57, "a": 0x41, "s": 0x53, "d": 0x44, "tab": 0x09, "q": 0x51, "e": 0x45,
    "f1": 0x70, "f11": 0x7A,
}
EXTENDED = {0x26, 0x28, 0x25, 0x27, 0x2E}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def send_key(vk: int, down: bool) -> None:
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = 0x0008  # KEYEVENTF_SCANCODE
    if vk in EXTENDED:
        flags |= 0x0001
    if not down:
        flags |= 0x0002
    inp = INPUT(type=1, u=_U(ki=KEYBDINPUT(0, scan, flags, 0, 0)))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def find_window(pid: int):
    found = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            if buf.value.startswith("Robotech"):
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else (None, "")


def client_bbox(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return (pt.x, pt.y, pt.x + r.right, pt.y + r.bottom)


def analyze_image(img):
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.uint8)
    if a.size == 0:
        return {}
    luma = a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114
    wash = ((a[..., 0] > 200) & (a[..., 1] > 220) & (a[..., 2] > 230)).mean() * 100
    black = (luma < 8).mean() * 100
    small = a[::16, ::16].astype(np.uint16) >> 3
    sig = zlib.crc32(small.tobytes())
    h, w = luma.shape
    cw = int(h * 4 / 3)  # 4:3 letterbox: ignore pillar bars
    x0 = max(0, (w - cw) // 2)
    core = luma[:, x0:x0 + cw]
    return {"mean": round(float(luma.mean()), 1), "wash_pct": round(float(wash), 1),
            "black_pct": round(float(black), 1), "core_mean": round(float(core.mean()), 1),
            "core_std": round(float(core.std()), 1), "sig": sig, "size": [w, h]}


class FILETIME(ctypes.Structure):
    _fields_ = [("lo", wt.DWORD), ("hi", wt.DWORD)]


def _ft(f):
    return ((f.hi << 32) | f.lo) / 1e7


def proc_cpu_seconds(handle):
    c, e, k, u = FILETIME(), FILETIME(), FILETIME(), FILETIME()
    kernel32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    return _ft(k) + _ft(u)


def thread_cpu_snapshot(proc):
    try:
        return {t.id: t.user_time + t.system_time for t in proc.threads()}
    except Exception:
        return {}


ANOMALY_PATTERNS = {
    "unimpl_instr": re.compile(r"\[UNIMPL\] untranslated instruction REACHED: `([^`]+)` at (0x[0-9A-Fa-f]+)"),
    "fault": re.compile(r"(access violation|EXCEPTION_|unhandled exception|fatal|abort\(|assertion|Segmentation)", re.I),
    "gpu_unhandled": re.compile(r"([1-9]\d*) unhandled methods \((\d+) distinct\)"),
    "stub_called": re.compile(r"\[(?:KERNEL|FILE|STUB)\][^\n]*\bstub\b", re.I),
    "unresolved": re.compile(r"(unresolved|UNRESOLVED|no such function|missing import)", re.I),
    "audio_underrun": re.compile(r"\b(?:underrun|underflow)\b", re.I),
    "dx_error": re.compile(r"(HRESULT|E_FAIL|DXGI_ERROR|device removed|shader compile)", re.I),
    "watchdog": re.compile(r"(WATCHDOG|watchdog|HANG|deadlock)", re.I),
}
RBX_STAT = re.compile(r"^\[RBX DX11\] frame=(\d+) flips=(\d+) draws=(\d+).*?us_draw=(\d+) us_clear=(\d+) us_flip=(\d+) scan_ms=(\d+)")
RBX_PERF = re.compile(r"^\[RBXPERF\] (.*)$")
PERF_FIELDS = ["ms", "flips", "pub", "ft_avg_us", "ft_max_us", "hitch33", "hitch50", "hitch100",
               "rb_n", "rb_us", "sh_n", "sh_us", "sh_total", "catchup", "fence_early", "fence_timeout",
               "mmio_traps", "apu_lock_waits", "apu_lock_timeouts", "sh_hits", "fence_wait_us",
               "pb_drops", "pb_midwalk_get", "rt_new", "rt_evict", "rt_alias", "pub_nort", "pub_multi",
               "clear_partial", "rb_defer", "rb_evict"]
XA2_UNDERRUN = re.compile(r"^\[XA2\] underrun x(\d+)\b")
AUDIO_HOST_UNDERRUN = re.compile(r"^\[AUDIO-HOST\].*\bunderruns=(\d+)\b")
FRAME_PRESS = re.compile(r"^\[RBX PAD\] (\w+) observed at f(\d+) \(scheduled f(\d+)\)$", re.M)
FRAME_NAME = re.compile(r"(?:^|_)f(\d+)\.bmp$", re.I)


def compare_frames(reference: Path, capture: Path):
    from PIL import Image, ImageChops, ImageStat

    def by_number(directory):
        frames = {}
        for path in directory.iterdir():
            match = FRAME_NAME.search(path.name)
            if not match or not path.is_file():
                continue
            number = int(match[1])
            if number in frames:
                raise ValueError(f"duplicate frame {number} in {directory}")
            frames[number] = path
        return frames

    expected, actual = by_number(reference), by_number(capture)
    scores, different_size, identical = {}, [], 0
    for number in sorted(expected.keys() & actual.keys()):
        with Image.open(expected[number]) as before, Image.open(actual[number]) as after:
            if before.size != after.size:
                different_size.append(number)
                continue
            diff = ImageChops.difference(before.convert("RGB"), after.convert("RGB"))
            identical += diff.getbbox() is None
            scores[number] = round(statistics.mean(ImageStat.Stat(diff).mean), 4)
    return {
        "reference": str(reference), "compared": len(scores),
        "identical": identical,
        "max_mean_rgb_error": max(scores.values(), default=None),
        "missing": sorted(expected.keys() - actual.keys()),
        "extra": sorted(actual.keys() - expected.keys()),
        "different_size": different_size,
        "mean_rgb_error_by_frame": scores,
    }


def scan_log(path: Path):
    res = {k: {"count": 0, "examples": {}} for k in ANOMALY_PATTERNS}
    rbx = []
    perf = []
    if not path.exists():
        return res, rbx, perf
    with path.open("r", errors="replace") as fh:
        for line in fh:
            counter = XA2_UNDERRUN.match(line) or AUDIO_HOST_UNDERRUN.match(line)
            if counter:
                count = int(counter[1])
                item = res["audio_underrun"]
                if count > item.get("cumulative", 0):
                    item["count"] += count - item.get("cumulative", 0)
                    item["cumulative"] = count
                    item["examples"] = {f"XAudio2 cumulative total: {count}": count}
                continue
            pm = RBX_PERF.match(line)
            if pm:
                row = {}
                for item in pm[1].split():
                    k, _, v = item.partition("=")
                    if k in PERF_FIELDS and v.lstrip("-").isdigit():
                        row[k] = int(v)
                if "ms" in row:
                    perf.append(row)
                continue
            m = RBX_STAT.match(line)
            if m:
                rbx.append({k: int(v) for k, v in zip(
                    ["frame", "flips", "draws", "us_draw", "us_clear", "us_flip", "scan_ms"], m.groups())})
                continue
            for k, rx in ANOMALY_PATTERNS.items():
                m2 = rx.search(line)
                if m2:
                    res[k]["count"] += 1
                    key = (m2.group(0) if k != "unimpl_instr" else f"{m2.group(1)}@{m2.group(2)}")[:140]
                    ex = res[k]["examples"]
                    ex[key] = ex.get(key, 0) + 1
                    break
    for k in res:
        ex = res[k]["examples"]
        res[k]["examples"] = dict(sorted(ex.items(), key=lambda kv: -kv[1])[:8])
    return res, rbx, perf


def summarize_perf(perf, out: Path):
    """Write perf.csv (one row per stats window) and return the run's totals."""
    if not perf:
        return None
    with (out / "perf.csv").open("w", encoding="utf-8") as fh:
        fh.write(",".join(PERF_FIELDS) + "\n")
        for row in perf:
            fh.write(",".join(str(row.get(k, "")) for k in PERF_FIELDS) + "\n")
    rows = perf[1:] if len(perf) > 1 else perf   # the first window starts at boot, not at the last line
    secs = max(1e-3, sum(r.get("ms", 0) for r in rows) / 1000.0)

    def total(k):
        return sum(r.get(k, 0) for r in rows)
    flips = total("flips")
    return {
        "windows": len(rows), "secs": round(secs, 1),
        "guest_fps": round(flips / secs, 1),
        "presented_fps": round(total("pub") / secs, 1),
        "frame_ms_avg": round(statistics.mean(r["ft_avg_us"] for r in rows if r.get("flips")) / 1000, 2)
                        if any(r.get("flips") for r in rows) else None,
        "frame_ms_worst": round(max(r.get("ft_max_us", 0) for r in rows) / 1000, 1),
        "hitches_over_33ms": total("hitch33"), "hitches_over_50ms": total("hitch50"),
        "hitches_over_100ms": total("hitch100"),
        "readback_ms_per_s": round(total("rb_us") / 1000 / secs, 2),
        "shader_compiles": total("sh_n"), "shader_compile_ms": round(total("sh_us") / 1000, 1),
        "shader_variants_since_boot": max(r.get("sh_total", 0) for r in perf),
        "forced_get_catchups": total("catchup"), "fence_early_wakes": total("fence_early"),
        "fence_timeouts": total("fence_timeout"),
        "mmio_traps_per_s": round(total("mmio_traps") / secs),
        "apu_lock_waits": total("apu_lock_waits"), "apu_lock_timeouts": total("apu_lock_timeouts"),
        "shader_cache_hits": total("sh_hits"),
        "fence_rewait_ms_per_s": round(total("fence_wait_us") / 1000 / secs, 2),
        "pb_dropped_walks": total("pb_drops"), "pb_midwalk_get_moves": total("pb_midwalk_get"),
        "rt_created": total("rt_new"), "rt_evicted": total("rt_evict"), "rt_alias_created": total("rt_alias"),
        "flips_without_gpu_target": total("pub_nort"), "flips_with_several_targets": total("pub_multi"),
        "partial_clears": total("clear_partial"),
        "readbacks_deferred": total("rb_defer"), "readbacks_at_eviction": total("rb_evict"),
    }


def parse_keys(spec: str):
    """'40:enter:200,45:space:200' -> [(t_sec, vk, hold_ms, name)]"""
    out = []
    for item in filter(None, (s.strip() for s in spec.split(","))):
        t, k, *h = item.split(":")
        vk = VK.get(k.lower()) or (ord(k.upper()) if len(k) == 1 and k.isalnum() else None)
        if vk is None:
            raise SystemExit(f"unknown key {k!r}")
        out.append((float(t), vk, int(h[0]) if h else 150, k))
    return sorted(out)


FPS_RE = re.compile(r"FPS:\s*([\d.]+)\s*\|\s*draws:\s*(\d+)")


def main() -> int:
    import psutil
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--secs", type=float, default=120)
    ap.add_argument("--name", default=time.strftime("run_%Y%m%d_%H%M%S"))
    ap.add_argument("--interval", type=float, default=1.0, help="sample period (s)")
    ap.add_argument("--shot-every", type=float, default=5.0, help="screenshot period (s), 0=off")
    input_mode = ap.add_mutually_exclusive_group()
    input_mode.add_argument("--keys", default="", help="t:key[:holdms],... timed key presses")
    input_mode.add_argument("--frame-pad", default="", help="fN:button[:frames],... guest-frame pad presses")
    ap.add_argument("--frame-dump-every", type=int, default=0, help="capture at multiples of this guest frame number")
    ap.add_argument("--compare-frames", type=Path, help="compare captured BMPs with a numbered reference directory")
    ap.add_argument("--audio", action="store_true", help="dump audio device stream to WAV and analyse")
    ap.add_argument("--gpu-stats", action="store_true", help="RECOMP_GPU_STATS=1 (periodic [RBX DX11] timings)")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE env")
    ap.add_argument("--hang-secs", type=float, default=15, help="no frame progress for this long = hang")
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--cwd", default=str(ROOT), help="working dir (must hold game_files\\); e.g. an unzipped release")
    a = ap.parse_args()
    if a.frame_dump_every < 0:
        ap.error("--frame-dump-every must be nonnegative")
    if a.compare_frames and (not a.frame_dump_every or not a.compare_frames.is_dir()):
        ap.error("--compare-frames requires a reference directory and --frame-dump-every")
    if a.frame_pad and not all(re.fullmatch(r"f\d+:(up|down|left|right|start|back|ls|rs|a|b|x|y|black|white|lt|rt)(?::[1-9]\d*)?", item)
                               for item in a.frame_pad.split(",")):
        ap.error("--frame-pad must be a comma-separated list of fN:button[:frames]")

    out = OUT_ROOT / a.name
    (out / "shots").mkdir(parents=True, exist_ok=True)
    errlog = out / "stderr.log"
    env = os.environ.copy()
    for k in CLEAR_ENV:
        env.pop(k, None)
    env.update(PLAY_ENV)
    env.pop("RECOMP_RBX_FRAME_PAD", None)
    if a.frame_pad:
        env["RECOMP_RBX_FRAME_PAD"] = a.frame_pad
        env["RECOMP_SAVE_DIR"] = str(out / "scenario_saves")
        env["RECOMP_RBX_SETTINGS"] = str(out / "scenario.ini")
    if a.frame_dump_every:
        frame_dir = out / "frames"
        frame_dir.mkdir(exist_ok=True)
        if any(frame_dir.iterdir()):
            ap.error(f"{frame_dir} already has captures, choose a new --name")
        env["RECOMP_GPU_PRESDUMP"] = f"{out / 'frames' / 'frame'}:{a.frame_dump_every}"
        env["RECOMP_GPU_PRESDUMP_FRAME"] = "1"
    env["RECOMP_FB_SHOT_DIR"] = str(out / "shots")
    if a.gpu_stats:
        env["RECOMP_GPU_STATS"] = "1"
    wav = out / "audio.wav"
    if a.audio:
        env["RECOMP_AUDIO_WAV"] = str(wav)
        env["RECOMP_AUDIO_HOST"] = "1"
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
        if k == "RECOMP_GPU_PRESDUMP":   # "<abs prefix>:<every>"; the dumper does not mkdir
            pre = v.rsplit(":", 1)[0] if v.count(":") > 1 else v
            Path(pre).parent.mkdir(parents=True, exist_ok=True)

    keys = parse_keys(a.keys) if a.keys else []
    print(f"[diag] out={out}\n[diag] launching {a.exe}", flush=True)
    t_launch = time.time()
    samples, shots, key_log, hangs = [], [], [], []
    thr_total = {}
    exited = None
    with errlog.open("wb") as ef, (out / "stdout.log").open("wb") as of:
        proc = subprocess.Popen([a.exe], cwd=a.cwd, env=env, stdout=of, stderr=ef)
        pp = psutil.Process(proc.pid)
        handle = kernel32.OpenProcess(0x1000 | 0x0400, False, proc.pid)
        last_cpu, last_t = proc_cpu_seconds(handle), time.time()
        last_thr = thread_cpu_snapshot(pp)
        pending = list(keys)
        held = None
        next_shot = a.shot_every
        last_progress_t = time.time()
        try:
            while True:
                now = time.time()
                el = now - t_launch
                if proc.poll() is not None:
                    exited = proc.returncode
                    break
                if el >= a.secs:
                    break
                hwnd, title = find_window(proc.pid)
                m = FPS_RE.search(title or "")
                fps = float(m.group(1)) if m else None
                draws = int(m.group(2)) if m else None
                hung = bool(hwnd and user32.IsHungAppWindow(hwnd))
                cpu = proc_cpu_seconds(handle)
                cores = (cpu - last_cpu) / max(1e-6, now - last_t)
                last_cpu, last_t = cpu, now
                thr = thread_cpu_snapshot(pp)
                for tid, tt in thr.items():
                    thr_total[tid] = thr_total.get(tid, 0.0) + max(0.0, tt - last_thr.get(tid, tt))
                last_thr = thr
                try:
                    ws, nthr = pp.memory_info().rss / 1048576, pp.num_threads()
                except Exception:
                    ws, nthr = 0, 0
                s = {"t": round(el, 1), "fps": fps, "draws": draws, "cpu_cores": round(cores, 2),
                     "ws_mb": round(ws), "threads": nthr, "hung": hung, "title": title}
                if fps and fps > 0:
                    last_progress_t = now
                elif now - last_progress_t > a.hang_secs and (not hangs or now - hangs[-1]["at_abs"] > a.hang_secs):
                    hangs.append({"t": round(el, 1), "at_abs": now, "hung_flag": hung,
                                  "cpu_cores": s["cpu_cores"], "title": title})
                    print(f"[diag] !! no frame progress {a.hang_secs:.0f}s at t={el:.0f}s cpu={s['cpu_cores']} cores", flush=True)
                    try:
                        from stackdump import dump_stacks
                        sp = out / f"hang_t{int(el):04d}_stacks.txt"
                        sp.write_text(dump_stacks(pp.pid))
                        hangs[-1]["stacks"] = str(sp)
                        print(f"[diag]    thread stacks -> {sp}", flush=True)
                    except Exception as e:
                        print(f"[diag]    stack dump failed: {e}", flush=True)

                while pending and el >= pending[0][0]:
                    _, vk, hold, name = pending.pop(0)
                    send_key(vk, True)
                    held = (vk, time.time() + hold / 1000.0, name)
                    key_log.append({"t": round(el, 1), "key": name, "hold_ms": hold})
                if held and time.time() >= held[1]:
                    send_key(held[0], False)
                    held = None

                if a.shot_every and el >= next_shot and hwnd:
                    next_shot += a.shot_every
                    try:
                        from PIL import ImageGrab
                        img = ImageGrab.grab(bbox=client_bbox(hwnd), all_screens=True)
                        p = out / "shots" / f"t{int(el):04d}.png"
                        img.save(p)
                        st = analyze_image(img)
                        st.update({"t": round(el, 1), "file": p.name})
                        shots.append(st)
                        s["shot"] = p.name
                    except Exception as e:
                        s["shot_err"] = str(e)
                samples.append(s)
                print(f"[diag] t={el:6.1f}s fps={fps} draws={draws} cpu={s['cpu_cores']:.2f}c ws={s['ws_mb']}MB thr={nthr}"
                      f"{' HUNG' if hung else ''}", flush=True)
                time.sleep(max(0.0, a.interval - (time.time() - now)))
        finally:
            if held:
                send_key(held[0], False)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except Exception:
                    proc.kill()
    return write_report(a, out, samples, shots, key_log, hangs, exited, thr_total, wav)


def exit_meaning(code):
    if code is None:
        return "still running at end of test (stopped by harness)"
    c = code & 0xFFFFFFFF
    names = {0xC0000005: "ACCESS_VIOLATION", 0xC0000409: "STACK_BUFFER_OVERRUN/fastfail",
             0xC00000FD: "STACK_OVERFLOW", 0xC000001D: "ILLEGAL_INSTRUCTION",
             0xC0000094: "INT_DIVIDE_BY_ZERO", 0x80000003: "BREAKPOINT"}
    return f"exit 0x{c:08X} ({names.get(c, 'normal' if c == 0 else 'abnormal')})"


def write_report(a, out, samples, shots, key_log, hangs, exited, thr_total, wav):
    log, rbx, perf = scan_log(out / "stderr.log")
    fps_vals = [s["fps"] for s in samples if s["fps"] is not None]
    cpu_vals = [s["cpu_cores"] for s in samples]
    srt = sorted(fps_vals)
    rep = {
        "name": a.name, "secs_requested": a.secs, "samples": len(samples),
        "exit": exit_meaning(exited), "crashed": exited is not None and (exited & 0xFFFFFFFF) != 0,
        "fps": {
            "n": len(fps_vals), "mean": round(statistics.mean(fps_vals), 1) if fps_vals else None,
            "median": round(statistics.median(fps_vals), 1) if fps_vals else None,
            "min": srt[0] if srt else None, "max": srt[-1] if srt else None,
            "p5": round(srt[int(len(srt) * 0.05)], 1) if srt else None,
            "pct_samples_ge_58": round(100 * sum(f >= 58 for f in fps_vals) / len(fps_vals), 1) if fps_vals else None,
            "zero_fps_samples": sum(1 for f in fps_vals if f == 0),
        },
        "cpu_cores_mean": round(statistics.mean(cpu_vals), 2) if cpu_vals else None,
        "ws_mb_max": max((s["ws_mb"] for s in samples), default=None),
        "ws_mb_growth": (samples[-1]["ws_mb"] - samples[5]["ws_mb"]) if len(samples) > 6 else None,
        "hung_window_samples": sum(1 for s in samples if s["hung"]),
        "frame_hangs": [{k: v for k, v in h.items() if k != "at_abs"} for h in hangs],
        "log": log, "keys_sent": key_log,
    }
    if a.frame_pad:
        rep["frame_pad"] = {
            "scheduled": a.frame_pad.split(","),
            "observed": [
                {"button": button, "frame": int(frame), "scheduled_frame": int(scheduled)}
                for button, frame, scheduled in FRAME_PRESS.findall((out / "stderr.log").read_text(errors="replace"))
            ],
        }
    if a.compare_frames:
        rep["frame_comparison"] = compare_frames(a.compare_frames, out / "frames")
    if rbx:
        def avg(k):
            return round(statistics.mean(r[k] for r in rbx), 1)
        rep["rbx_dx11"] = {"intervals": len(rbx), "avg_scan_ms": avg("scan_ms"), "avg_us_flip": avg("us_flip"),
                           "avg_us_draw": avg("us_draw"), "max_scan_ms": max(r["scan_ms"] for r in rbx)}
    perf_summary = summarize_perf(perf, out)
    if perf_summary:
        rep["perf"] = perf_summary
    dur = max(1.0, samples[-1]["t"] if samples else 1.0)
    hot = sorted(thr_total.items(), key=lambda kv: -kv[1])[:6]
    rep["hot_threads_cpu_pct_of_core"] = {str(t): round(100 * v / dur, 1) for t, v in hot}
    if shots:
        sigs = [s["sig"] for s in shots]
        longest = cur = 1
        for i in range(1, len(sigs)):
            cur = cur + 1 if sigs[i] == sigs[i - 1] else 1
            longest = max(longest, cur)
        rep["screens"] = {
            "count": len(shots), "max_identical_in_a_row": longest,
            "black_frames": sum(1 for s in shots if s.get("core_mean", 99) < 3),
            "wash_frames_gt20pct": sum(1 for s in shots if s.get("wash_pct", 0) > 20),
            "max_wash_pct": max(s.get("wash_pct", 0) for s in shots),
            "series": [{k: s[k] for k in ("t", "mean", "wash_pct", "black_pct", "core_std")} for s in shots],
        }
    if a.audio:
        rep["audio"] = analyze_wav(wav)
    rep["verdicts"] = verdicts(rep)
    (out / "report.json").write_text(json.dumps({**rep, "samples": samples}, indent=1))
    md = render_md(rep)
    (out / "report.md").write_text(md, encoding="utf-8")
    print("\n" + md)
    missed_input = a.frame_pad and len(rep["frame_pad"]["observed"]) != len(rep["frame_pad"]["scheduled"])
    no_comparable_frames = a.compare_frames and not rep["frame_comparison"]["compared"]
    return 1 if rep["crashed"] or rep["frame_hangs"] or missed_input or no_comparable_frames else 0


def analyze_wav(path: Path):
    import wave
    import numpy as np
    if not path.exists() or path.stat().st_size < 100:
        return {"error": "no WAV produced (APU never started or audio not routed)"}
    ch, sr = 2, 48000
    try:
        with wave.open(str(path)) as w:
            ch, sr = w.getnchannels(), w.getframerate()
            raw = w.readframes(w.getnframes())
    except Exception:
        data = path.read_bytes()[44:]  # header not finalised if the process was killed
        raw = data[: len(data) // 4 * 4]
    a = np.frombuffer(raw, dtype=np.int16).reshape(-1, ch).astype(np.float32) / 32768
    if a.size == 0:
        return {"error": "empty WAV (APU started but nothing was mixed)"}
    mono = np.abs(a).max(axis=1)
    win = sr // 10
    nwin = len(mono) // win
    wins = mono[: nwin * win].reshape(nwin, win).max(axis=1) if nwin else mono
    silent = wins < 0.002
    run = best = 0
    for sv in silent:
        run = run + 1 if sv else 0
        best = max(best, run)
    d = np.abs(np.diff(a[:, 0]))
    return {"seconds": round(len(a) / sr, 1), "rate": sr, "peak": round(float(mono.max()), 3),
            "rms": round(float(np.sqrt((a ** 2).mean())), 4),
            "silent_pct": round(float(silent.mean() * 100), 1),
            "longest_silence_s": round(best / 10, 1),
            "clipped_pct": round(float((mono > 0.99).mean() * 100), 3), "hard_clicks": int((d > 0.6).sum()),
            "left_right_identical": bool(np.allclose(a[:, 0], a[:, -1], atol=1e-4))}


def verdicts(r):
    v = []
    if r["crashed"]:
        v.append(f"CRASH: {r['exit']}")
    if r["frame_hangs"]:
        v.append(f"HANG: {len(r['frame_hangs'])} stall(s) with no frame progress")
    if r["hung_window_samples"]:
        v.append(f"UI-THREAD HANG: window unresponsive for {r['hung_window_samples']} sample(s)")
    f = r["fps"]
    if f["mean"] is not None:
        if f["median"] < 55:
            v.append(f"PERF: median FPS {f['median']} < 60 (mean {f['mean']}, p5 {f['p5']}, >=58fps in {f['pct_samples_ge_58']}% samples)")
        else:
            v.append(f"PERF OK: median FPS {f['median']}")
    else:
        v.append("PERF: no FPS in window title (window not found?)")
    if r.get("ws_mb_growth") and r["ws_mb_growth"] > 300:
        v.append(f"LEAK?: working set grew {r['ws_mb_growth']} MB")
    sc = r.get("screens")
    if sc:
        if sc["black_frames"] > sc["count"] * 0.5:
            v.append(f"VISUAL: {sc['black_frames']}/{sc['count']} screenshots black")
        if sc["wash_frames_gt20pct"]:
            v.append(f"VISUAL: {sc['wash_frames_gt20pct']} screenshot(s) >20% blown-out white/cyan (max {sc['max_wash_pct']}%)")
        if sc["max_identical_in_a_row"] >= 6:
            v.append(f"FROZEN?: {sc['max_identical_in_a_row']} identical screenshots in a row (may be a static screen)")
    au = r.get("audio")
    if au:
        if "error" in au:
            v.append("AUDIO: " + au["error"])
        else:
            if au["peak"] < 0.01:
                v.append("AUDIO: output is silent")
            if au["clipped_pct"] > 0.5:
                v.append(f"AUDIO: clipping {au['clipped_pct']}%")
            if au["hard_clicks"] > 50:
                v.append(f"AUDIO: {au['hard_clicks']} hard discontinuities (crackle)")
    p = r.get("perf")
    if p:
        if p["hitches_over_100ms"]:
            v.append(f"STUTTER: {p['hitches_over_100ms']} frame(s) over 100 ms (worst {p['frame_ms_worst']} ms)")
        if p["forced_get_catchups"]:
            v.append(f"PB: {p['forced_get_catchups']} forced GET catch-up(s) (unread commands marked consumed)")
        if p["fence_timeouts"]:
            v.append(f"PB: {p['fence_timeouts']} BlockOnFence recheck timeout(s) (2 s each)")
        if p["shader_compiles"]:
            v.append(f"SHADERS: {p['shader_compiles']} compile(s) during the run, {p['shader_compile_ms']} ms total")
        if p.get("pb_dropped_walks"):
            v.append(f"PB: {p['pb_dropped_walks']} walk(s) abandoned at PUT (commands skipped)")
        if p.get("rt_evicted"):
            v.append(f"RT: {p['rt_evicted']} render target(s) evicted from the 8 slots (possible flashes)")
        if p.get("flips_without_gpu_target"):
            v.append(f"PRESENT: {p['flips_without_gpu_target']} flip(s) showed guest memory instead of the GPU image")
        if p["apu_lock_timeouts"]:
            v.append(f"AUDIO: {p['apu_lock_timeouts']} voice-lock wait timeout(s)")
    if r["log"]["fault"]["count"]:
        v.append(f"LOG: {r['log']['fault']['count']} fault-like lines")
    return v


def render_md(r):
    L = [f"# Diagnostic report: {r['name']}", "", "## Verdicts"] + [f"- {x}" for x in r["verdicts"]]
    L += ["", "## Summary", f"- exit: {r['exit']}", f"- FPS: {r['fps']}",
          f"- CPU cores mean: {r['cpu_cores_mean']}  WS max: {r['ws_mb_max']} MB  hot threads (tid -> % of a core): {r['hot_threads_cpu_pct_of_core']}"]
    if "rbx_dx11" in r:
        L.append(f"- RBX DX11 timings: {r['rbx_dx11']}")
    if "perf" in r:
        L.append(f"- Perf counters (perf.csv): {r['perf']}")
    if "audio" in r:
        L.append(f"- audio: {r['audio']}")
    L += ["", "## Log anomalies"]
    for k, v in r["log"].items():
        if v["count"]:
            L.append(f"- {k}: {v['count']}  e.g. {v['examples']}")
    if r["keys_sent"]:
        L += ["", f"## Keys sent: {r['keys_sent']}"]
    if "frame_pad" in r:
        observed = r["frame_pad"]["observed"]
        total = len(r["frame_pad"]["scheduled"])
        L += ["", "## Frame-numbered input", "", f"- Observed {len(observed)} of {total} scheduled presses",
              *[f"- {p['button']} at f{p['frame']} (scheduled f{p['scheduled_frame']})" for p in observed]]
    if "frame_comparison" in r:
        comparison = r["frame_comparison"]
        L += ["", "## Frame comparison", "",
              f"- Compared {comparison['compared']} frames, {comparison['identical']} identical",
              f"- Largest mean RGB error (0–255): {comparison['max_mean_rgb_error']}",
              f"- Missing: {len(comparison['missing'])}, extra: {len(comparison['extra'])}, different dimensions: {len(comparison['different_size'])}"]
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    sys.exit(main())
