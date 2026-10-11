"""Compare production blend routines with the unmodified V3 reference, pixel by pixel."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", required=True)
    parser.add_argument("--linker", required=True)
    args = parser.parse_args()
    output = ROOT / "build/gba-render-tests"
    output.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "gbsp/components/gbsp-libretro/video.cpp").read_text(encoding="utf-8")
    start = source.index("#define BLND_MSK")
    end = source.index("// Fills a segment", start)
    (output / "render_candidate.inc").write_text(source[start:end], encoding="utf-8")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(output / "zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(output / "zig-local-cache"))
    for rgb1555 in (0, 1):
        obj = output / f"render-{rgb1555}.obj"
        dll = output / f"render-{rgb1555}.dll"
        flags = ["-DUSE_XBGR1555_FORMAT=1"] if rgb1555 else []
        subprocess.run([args.zig, "c++", "-target", "x86_64-windows-gnu", "-ffreestanding",
                        "-fno-builtin", "-fno-stack-protector", "-fno-exceptions", "-fno-rtti",
                        "-O2", "-Wall", "-Werror", *flags, "-I", str(output),
                        "-c", str(ROOT / "tools/tests/gba_render.cpp"), "-o", str(obj)],
                       check=True, env=env)
        subprocess.run([args.linker, "-flavor", "link", "/dll", "/noentry", "/nodefaultlib",
                        "/machine:x64", f"/out:{dll}", str(obj)], check=True)
        library = ctypes.CDLL(str(dll))
        result = library.test_render()
        if result:
            raise SystemExit(f"FAIL format={rgb1555} at gba_render.cpp:{result}")
        print(f"PASS {'XBGR1555' if rgb1555 else 'RGB565'}: 32768 cases, all blend modes, "
              "both palettes, clipped/empty/full spans, byte-exact output")
        print("Palette bindings per full 240px blend: "
              f"{library.reference_lookups()} -> {library.candidate_lookups()}")


if __name__ == "__main__":
    main()
