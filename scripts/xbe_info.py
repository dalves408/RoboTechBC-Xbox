"""Print key XBE certificate / section / library info for Robotech Battlecry readiness."""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

RETAIL_ENTRY_XOR = 0xA8FC57AB
DEBUG_ENTRY_XOR = 0x94859D4B


def analyze(path: Path) -> None:
    data = path.read_bytes()
    if data[0:4] != b"XBEH":
        raise SystemExit(f"not an XBE: {path}")
    base = struct.unpack_from("<I", data, 0x104)[0]
    cert_va = struct.unpack_from("<I", data, 0x118)[0]
    entry_raw = struct.unpack_from("<I", data, 0x128)[0]
    num_sec = struct.unpack_from("<I", data, 0x11C)[0]
    sec_va = struct.unpack_from("<I", data, 0x120)[0]
    lib_va = struct.unpack_from("<I", data, 0x148)[0]
    num_libs = struct.unpack_from("<I", data, 0x14C)[0]

    cert = cert_va - base
    title_id = struct.unpack_from("<I", data, cert + 0x08)[0]
    title = data[cert + 0x0C : cert + 0x0C + 80].decode("utf-16le", errors="ignore").split("\x00", 1)[0]
    version = struct.unpack_from("<I", data, cert + 0xA8)[0]

    print(f"file={path}")
    print(f"size={len(data)}")
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"base={base:#010x}")
    print(f"title_id={title_id:08X}")
    print(f"title={title}")
    print(f"version={version} ({version:#010x})")
    print(f"entry_raw={entry_raw:#010x}")
    print(f"entry_xor_retail={entry_raw ^ RETAIL_ENTRY_XOR:#010x}")
    print(f"entry_xor_debug={entry_raw ^ DEBUG_ENTRY_XOR:#010x}")

    # Retail XBEs XOR several header VAs; library table VA may be encoded.
    # Prefer xboxrecomp tools.xbe_parser for authoritative library output.
    print("libraries:")
    candidates = [lib_va]
    for xor_key in (0x00000000, 0x5B6D40B6, 0xEBC83B7A):  # raw / retail-ish / debug-ish
        candidates.append(lib_va ^ xor_key)
    seen = set()
    for cand in candidates:
        if not cand or cand in seen:
            continue
        seen.add(cand)
        loff = cand - base
        if loff < 0 or loff + max(num_libs, 1) * 0x40 > len(data):
            continue
        try:
            for i in range(num_libs):
                o = loff + i * 0x40
                name = data[o : o + 8].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
                if not name or not name.isprintable():
                    raise ValueError("bad lib name")
                major, minor, build = struct.unpack_from("<HHH", data, o + 8)
                print(f"  {name} {major}.{minor}.{build}")
            break
        except (struct.error, ValueError, UnicodeDecodeError):
            continue
    else:
        print("  (could not decode library table; see analysis/default_analysis.json)")

    print(f"sections={num_sec}")
    soff = sec_va - base
    for i in range(num_sec):
        o = soff + i * 0x38
        flags, vaddr, vsize, raddr, rsize = struct.unpack_from("<IIIII", data, o)
        name_va = struct.unpack_from("<I", data, o + 0x14)[0]
        name = data[name_va - base : name_va - base + 64].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
        print(
            f"  [{i}] {name:16} va={vaddr:08X} vs={vsize:08X} "
            f"ra={raddr:08X} rs={rsize:08X} flags={flags:08X}"
        )


if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parent.parent / "game_files" / "default.xbe"))
    analyze(target)
