#!/usr/bin/env python3
"""Generates a CycloneDX SBOM for a BlockBoy firmware release.

The CRA (Annex I, part II, point 1) asks manufacturers to identify and document
the components in their product in "a commonly used machine-readable format",
covering at the very least the top-level dependencies. That is what this
produces: one document per firmware release, listing ESP-IDF, every managed
component pulled from the Espressif registry, the vendored libraries that live
in this tree, and the emulator cores.

Run it from the root of a firmware tree:

    python tools/gen_sbom.py --output ../../CE-dossier/08_cybersecurity/sbom-v3-<version>.json

Espressif ships a richer tool (esp-idf-sbom) that also checks the result against
the National Vulnerability Database. Use that when it is installed; this exists
so the dossier never blocks on a missing tool, and so the SBOM can be rebuilt
years from now from nothing but this repository.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import sys
import uuid

# Libraries that are checked into this tree rather than pulled from a registry.
# They have no version of their own there, so the version is the one we vendored
# and the note says where it came from.
VENDORED = [
    ("cJSON", "1.7.18", "https://github.com/DaveGamble/cJSON", "MIT",
     "components/retro-go/libs/cJSON"),
    ("lodepng", "20230410", "https://github.com/lvandeve/lodepng", "Zlib",
     "components/retro-go/libs/lodepng"),
    ("miniz", "2.2.0", "https://github.com/richgel999/miniz", "MIT",
     "components/retro-go/libs/miniz"),
]

# The emulator cores, each its own upstream project under its own licence.
CORES = [
    ("retro-go", "GPL-2.0-only", "https://github.com/ducalex/retro-go",
     "components/retro-go"),
    ("gnuboy", "GPL-2.0-only", "https://github.com/ducalex/retro-go", "retro-core/components/gnuboy"),
    ("smsplus", "GPL-2.0-only", "https://github.com/ducalex/retro-go", "retro-core/components/smsplus"),
    ("nofrendo", "GPL-2.0-only", "https://github.com/ducalex/retro-go", "retro-core/components/nofrendo"),
    ("gw-emulator", "GPL-2.0-only", "https://github.com/bzhxx/game-and-watch-simulator",
     "retro-core/components/gw-emulator"),
    ("prboom", "GPL-2.0-only", "https://github.com/coelckers/prboom-plus", "prboom-go/components/prboom"),
    # The upstream project declares AGPL-3.0 (GitHub sidebar and LICENSE file),
    # while the individual source headers say "GPL version 3 or, at your option,
    # any later version". Recorded as the project declares it, because that is
    # what a licence audit will find. Either way it is GPLv3-family and not v2,
    # which matters: see 08_cybersecurity/licenties.md in the CE dossier.
    ("gwenesis@168e466", "AGPL-3.0-only", "https://github.com/bzhxx/gwenesis", "gwenesis/components/gwenesis"),
    ("gbsp-libretro", "GPL-2.0-only", "https://github.com/libretro/gpsp", "gbsp/components/gbsp-libretro"),
]


def firmware_version(target):
    path = os.path.join("components", "retro-go", "targets", target, "config.h")
    with open(path, encoding="utf-8", errors="replace") as fp:
        found = re.search(r'#\s*define\s+RG_FIRMWARE_VERSION\s+"([^"]+)"', fp.read())
    return found.group(1) if found else "unknown"


def parse_locks():
    """Every managed component in the tree, keyed by name, with its version."""
    components = {}
    idf_version = None

    for lock in glob.glob("*/dependencies.lock"):
        with open(lock, encoding="utf-8", errors="replace") as fp:
            text = fp.read()

        # Deliberately parsed with regex rather than a YAML library: this has to
        # keep working in a bare Python years from now, and the file shape is
        # fixed by the component manager.
        for match in re.finditer(r"^  ([\w./-]+):\n(.*?)(?=^  \S|\Z)", text, re.S | re.M):
            name, body = match.group(1), match.group(2)
            version = re.search(r"^    version: '?([^'\n]+)'?", body, re.M)
            if not version:
                continue
            if name == "idf":
                idf_version = version.group(1)
                continue
            components.setdefault(name, version.group(1).strip())

    return components, idf_version


def component_entry(name, version, purl, licence, note):
    entry = {
        "type": "library",
        "name": name,
        "version": version,
        "purl": purl,
        "description": note,
    }
    if licence:
        entry["licenses"] = [{"license": {"id": licence}}]
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default=None,
                        help="target name; defaults to the only one in the tree")
    parser.add_argument("--output", default="sbom.json")
    args = parser.parse_args()

    targets = [os.path.basename(d) for d in glob.glob("components/retro-go/targets/*")
               if os.path.isdir(d)]
    target = args.target or (targets[0] if len(targets) == 1 else None)
    if not target:
        sys.exit("Give --target, this tree has: %s" % ", ".join(targets))

    version = firmware_version(target)
    managed, idf_version = parse_locks()

    components = []

    if idf_version:
        components.append(component_entry(
            "esp-idf", idf_version,
            "pkg:github/espressif/esp-idf@v%s" % idf_version,
            "Apache-2.0", "SDK, bootloader, TCP/IP stack, mbedTLS, Wi-Fi and BLE stacks"))

    for name in sorted(managed):
        components.append(component_entry(
            name, managed[name],
            "pkg:idf/%s@%s" % (name, managed[name]),
            None, "managed component from components.espressif.com"))

    for name, ver, url, licence, path in VENDORED:
        components.append(component_entry(
            name, ver, "pkg:generic/%s@%s" % (name, ver), licence,
            "vendored in %s, upstream %s" % (path, url)))

    for name, licence, url, path in CORES:
        if not os.path.isdir(path):
            continue  # not every tree carries every core (V1 has no GBA/Genesis/Doom)
        # "name@snapshot" records an upstream snapshot id; otherwise the core is
        # versioned with the firmware it ships in.
        name, _, snapshot = name.partition("@")
        components.append(component_entry(
            name, snapshot or version, "pkg:generic/%s" % name, licence,
            "emulator core / framework in %s, upstream %s" % (path, url)))

    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": "urn:uuid:%s" % uuid.uuid4(),
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc)
                          .replace(microsecond=0).isoformat(),
            "tools": [{"name": "gen_sbom.py", "vendor": "OviTech"}],
            "authors": [{"name": "OviTech"}],
            "component": {
                "type": "firmware",
                "name": "BlockBoy firmware",
                "version": version,
                "description": "BlockBoy handheld firmware for target %s" % target,
                "licenses": [{"license": {"id": "GPL-2.0-only"}}],
            },
        },
        "components": components,
    }

    with open(args.output, "w", encoding="utf-8") as fp:
        json.dump(sbom, fp, indent=2, ensure_ascii=False)
        fp.write("\n")

    digest = hashlib.sha256(open(args.output, "rb").read()).hexdigest()
    print("Wrote %s" % args.output)
    print("  firmware %s, target %s" % (version, target))
    print("  %d components (ESP-IDF %s)" % (len(components), idf_version or "?"))
    print("  sha256 %s" % digest)


if __name__ == "__main__":
    main()
