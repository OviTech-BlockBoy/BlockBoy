"""Prepare local publication files from a completed V3 build; never uploads.

Run from the repository root: python tools/package_publication.py
The build must have completed signature verification through rg_tool.py first.
"""
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {'build', 'managed_components', '__pycache__', '.git', '.cache',
             '.claude', '.codex', '.agents', '.vscode', 'node_modules'}
SKIP_EXT = {'.img', '.fw', '.zip', '.log', '.pyc', '.o', '.obj', '.elf', '.map',
            '.pem', '.key', '.p12', '.pfx', '.gba', '.gb', '.gbc', '.sav', '.tmp', '.old'}


def source_files(root=ROOT):
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            path = Path(base) / name
            relative = path.relative_to(root)
            if path.is_symlink():
                raise ValueError(f'Symlink requires review: {relative}')
            if path.suffix.lower() in SKIP_EXT or '.bak' in name.lower():
                continue
            if name == '.env' or name.startswith('.env.'):
                continue
            if name == 'sdkconfig' and 'targets' not in relative.parts:
                continue
            if name in {'partitions.bin', 'partitions.csv', '.DS_Store'}:
                continue
            yield path, relative


def main():
    config = (ROOT / 'components/retro-go/targets/0v1Tech-BlockBoy-N16R8/config.h').read_text()
    version = re.search(r'#define\s+RG_FIRMWARE_VERSION\s+"([^"]+)"', config)[1]
    image_name = f'blockboy_{version}_0v1tech-blockboy-n16r8.img'
    image = (ROOT / image_name).read_bytes()
    apps = []
    with (ROOT / 'partitions.csv').open() as f:
        for row in csv.reader(f):
            name, kind, subtype, offset, size = (x.strip() for x in row)
            if kind != 'app' or name in {'app5', 'app6'}:
                continue
            offset, size = int(offset, 0), int(size, 0)
            data = (ROOT / name / 'build' / (name + '.bin')).read_bytes()
            if len(data) > size or image[offset:offset + len(data)] != data:
                raise ValueError(f'Image does not match the built {name} app')
            if image[offset + len(data):offset + size] != b'\xff' * (size - len(data)):
                raise ValueError(f'Unexpected partition padding: {name}')
            apps.append(dict(name=name, offset=offset, partition_bytes=size,
                             app_bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    for filename, offset in [('launcher/build/bootloader/bootloader.bin', 0), ('partitions.bin', 0x8000)]:
        data = (ROOT / filename).read_bytes()
        if image[offset:offset + len(data)] != data:
            raise ValueError(f'Image does not match {filename}')
    if len(apps) != 6 or len(image) != 10158080:
        raise ValueError('Unexpected V3 image layout')

    out = ROOT / 'build' / f'publication-v3-{version}'
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / image_name, out / image_name)
    archive = out / f'blockboy-v3-{version}-source.zip'
    manifest = {}
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path, relative in source_files():
            data = path.read_bytes()
            # Refuse embedded private key material, including renamed key files.
            if re.search(rb'-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----', data):
                raise ValueError(f'Private key material found in {relative}')
            name = relative.as_posix()
            z.writestr('BlockBoy-V3/' + name, data)
            manifest[name] = hashlib.sha256(data).hexdigest()
    required = {'components/retro-go/targets/0v1Tech-BlockBoy-N16R8/sdkconfig',
                'components/retro-go/targets/0v1Tech-BlockBoy-N16R8/config.h',
                'gbsp/sdkconfig.overrides', 'gbsp/dependencies.lock', 'COPYING',
                'rg_tool.py', 'rg_config.py', 'base.cmake'}
    if not required.issubset(manifest):
        raise ValueError('Required source/build configuration missing from archive')
    (out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with zipfile.ZipFile(archive) as z:
        bad = z.testzip()
        if bad:
            raise ValueError(f'Archive verification failed: {bad}')
        if len(z.namelist()) != len(manifest):
            raise ValueError('Source manifest count mismatch')
    (out / 'firmware-manifest.json').write_text(json.dumps(dict(
        version=version, target='0v1Tech-BlockBoy-N16R8', image=image_name,
        image_bytes=len(image), sha256=hashlib.sha256(image).hexdigest(),
        benchmark_default='hidden', partitions=apps), indent=2) + '\n')
    (out / 'README.txt').write_text(
        f'BlockBoy V3 firmware {version}\n\n'
        f'{image_name}: complete signed image for ESP32-S3 N16R8.\n'
        f'{archive.name}: source code, licenses, build instructions and tests.\n'
        'The GBA benchmark is hidden by default. Service > Shop Menu > In game\n'
        '> GBA benchmark can enable it. Shop Reset restores the hidden default.\n\n'
        'See BUILDING.md in the source archive for build and installation\n'
        'instructions. Firmware and source hashes are listed in SHA256SUMS.txt.\n'
        'Build caches, private keys and local research notes are excluded.\n', encoding='utf-8')
    hashes = []
    for path in sorted(out.iterdir()):
        if path.is_file() and path.name != 'SHA256SUMS.txt':
            hashes.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}')
    (out / 'SHA256SUMS.txt').write_text('\n'.join(hashes) + '\n')
    print(f'Publication folder: {out}')
    print(f'Source files: {len(manifest)}; source archive: {archive.stat().st_size} bytes')
    print(f'Image SHA256: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
