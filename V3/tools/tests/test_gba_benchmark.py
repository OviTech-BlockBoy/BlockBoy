"""Native tests of the real benchmark with mocked ESP/SD/menu/clock services."""
import argparse
import ctypes
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig', required=True)
    p.add_argument('--linker', required=True)
    args = p.parse_args()
    out = ROOT / 'build/gba-host-tests'
    out.mkdir(parents=True, exist_ok=True)
    src = (ROOT / 'gbsp/main/rg_benchmark.c').read_text(encoding='utf-8')
    src = re.sub(r'^#include.*$', '', src, flags=re.M)
    (out / 'benchmark_under_test.h').write_text(src, encoding='utf-8')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'), ZIG_LOCAL_CACHE_DIR=str(out / 'zig-local-cache'))
    obj, dll = out / 'benchmark.obj', out / 'benchmark.dll'
    subprocess.run([args.zig, 'cc', '-target', 'x86_64-windows-gnu', '-ffreestanding',
                    '-fno-builtin', '-fno-stack-protector', '-O1', '-Wall', '-Werror',
                    '-I', str(out), '-I', str(ROOT / 'gbsp/components/gbsp-libretro'),
                    '-include', str(ROOT / 'gbsp/main/rg_benchmark.h'),
                    '-c', str(ROOT / 'tools/tests/gba_benchmark.c'), '-o', str(obj)], check=True, env=env)
    subprocess.run([args.linker, '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib',
                    '/machine:x64', f'/out:{dll}', str(obj)], check=True)
    lib = ctypes.CDLL(str(dll))
    for name in ('test_benchmark', 'test_profile'):
        result = getattr(lib, name)()
        if result: raise SystemExit(f'FAIL {name}: gba_benchmark.c:{result}')
        print(f'PASS {name}')

if __name__ == '__main__':
    main()
