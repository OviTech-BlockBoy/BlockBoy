"""Summarize V3 GBA benchmark CSVs without averaging unequal FPS intervals.

Usage: python tools/gba_benchmark_report.py "benchmarks/*.csv"
"""
import argparse
import csv
import glob
import io
from pathlib import Path


def summarize(text):
    metadata = {}
    data = []
    for line in text.splitlines():
        if line.startswith('# '):
            key, sep, value = line[2:].partition('=')
            if sep:
                metadata[key] = value
        elif line.strip():
            data.append(line)
    if metadata.get('schema') != 'gba-benchmark-v3-1':
        raise ValueError('Unsupported CSV schema (use the V3 benchmark)')
    rows = list(csv.DictReader(io.StringIO('\n'.join(data))))
    if not rows:
        raise ValueError('No measured frames: run cancelled during warmup or start failed')
    seconds = sum(float(r['interval_s']) for r in rows)
    if seconds <= 0 or any(float(r['interval_s']) <= 0 for r in rows):
        raise ValueError('Invalid measurement interval')
    frames = sum(int(r['frames']) for r in rows)
    if not frames:
        raise ValueError('No emulated frames')
    result = dict(seconds=seconds, frames=frames,
                  fps=frames / seconds,
                  drawn_fps=sum(int(r['drawn']) for r in rows) / seconds,
                  max_ms=max(float(r['max_frame_ms']) for r in rows),
                  over33=sum(int(r['frames_gt33ms']) for r in rows))
    for column in rows[0]:
        if column.endswith('_ms') and column != 'max_frame_ms':
            result[column + '_per_frame'] = sum(float(r[column]) for r in rows) / frames
    return metadata, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()
    paths = sorted({Path(p) for pattern in args.files for p in glob.glob(pattern)})
    if not paths:
        parser.error('No CSV files found')
    failed = False
    for path in paths:
        try:
            meta, result = summarize(path.read_text(encoding='utf-8-sig'))
            print(f"\n{path.name}: {meta.get('mode')} / timings={meta.get('timings')} / "
                  f"{meta.get('stop', 'missing stop marker')}")
            print(f"  ROM: {meta.get('rom')} | ELF: {meta.get('elf_sha256', '')[:16]}")
            print(f"  {result['seconds']:.2f}s: emu {result['fps']:.2f} FPS, "
                  f"drawn {result['drawn_fps']:.2f} FPS; "
                  f"max {result['max_ms']:.1f}ms, >33ms {result['over33']}")
            if meta.get('timings') == '1':
                fields = ['execute', 'classic', 'jit', 'compile', 'rom_swap', 'cpuset',
                          'cpufastset', 'lz77', 'live_render', 'm4a_mixer', 'offload_render', 'pacing']
                print('  Inclusive ms per emulated frame (overlapping scopes):')
                print('  ' + ', '.join(f"{key}={result.get(key + '_ms_per_frame', 0):.3f}" for key in fields))
            if meta.get('offload_drained') != '1':
                print('  Incomplete offload drain: do not use this run as a reference.')
            if result['max_ms'] >= 17000 and meta.get('timings') == '1':
                print('  Long stall: cycle-counter scope timings may have wrapped.')
        except (ValueError, KeyError, TypeError, OSError) as exc:
            failed = True
            print(f'{path}: {exc}')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
