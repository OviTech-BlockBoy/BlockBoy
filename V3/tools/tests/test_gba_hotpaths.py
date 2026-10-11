"""Run native C regressions against functions extracted from the emulator source.

Windows: Python + Zig's C compiler and lld.exe (no Windows SDK needed).
Usage: python tools/tests/test_gba_hotpaths.py --zig PATH/zig.exe --linker PATH/lld.exe
The harness mocks IO; this does not test Xtensa code generation or SD hardware.
"""
import argparse
import ctypes
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "gbsp/components/gbsp-libretro"


def function(text, name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\s*\{", text, re.M)
    if not match:
        raise RuntimeError(f"Cannot extract {name}")
    start = text.index("{", match.start())
    # Strip comments/strings for brace counting, preserving character offsets.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: " " * len(m[0]), text, flags=re.S)
    depth = 1
    end = start + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", required=True)
    parser.add_argument("--linker", required=True)
    args = parser.parse_args()
    output = ROOT / "build/gba-host-tests"
    output.mkdir(parents=True, exist_ok=True)
    jit = (CORE / "cpu_jit.c").read_text(encoding="utf-8")
    names = ("jhash_pc", "jhash_lookup", "jhash_insert", "jit_half_flush")
    (output / "jit_functions.h").write_text(
        "\n\n".join(function(jit, name) for name in names), encoding="utf-8")
    memory = (CORE / "gba_memory.c").read_text(encoding="utf-8")
    (output / "rom_functions.h").write_text(
        "\n\n".join(function(memory, name) for name in
                    ("gamepak_read_initial", "load_gamepak_raw")), encoding="utf-8")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(output / "zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(output / "zig-local-cache"))
    for raw in (1, 0):
        obj = output / f"hotpaths-{raw}.obj"
        dll = output / f"hotpaths-{raw}.dll"
        subprocess.run([args.zig, "cc", "-target", "x86_64-windows-gnu", "-ffreestanding",
                        "-fno-builtin", "-fno-stack-protector", "-O1", "-Wall", "-Werror",
                        f"-DROM_RAW_READ={raw}", "-I", str(output), "-c",
                        str(ROOT / "tools/tests/gba_hotpaths.c"), "-o", str(obj)],
                       check=True, env=env)
        subprocess.run([args.linker, "-flavor", "link", "/dll", "/noentry",
                        "/nodefaultlib", "/machine:x64", f"/out:{dll}", str(obj)], check=True)
        library = ctypes.CDLL(str(dll))
        for name in ("test_jit_hash", "test_rom_load"):
            result = getattr(library, name)()
            if result:
                raise SystemExit(f"FAIL {name} (raw={raw}) at gba_hotpaths.c:{result}")
            print(f"PASS {name} (raw={raw})")


if __name__ == "__main__":
    main()
