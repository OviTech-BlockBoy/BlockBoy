#!/usr/bin/env python3
"""Prepares a built firmware as an OTA update.

Generates the manifest the BlockBoy fetches, and collects the app binaries that
must be uploaded as release assets.

Usage (from the project directory, after a successful build):

    python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
    python tools/publish_update.py 3.1.0 --notes "GBA audio fix"

Result in build/ota/:
    v3-n16r8.json     -> commit into the firmware repo under ota/
    launcher.bin      -> attach as an asset to the GitHub Release
    retro-core.bin
    ...

The manifest points at the release assets via the tag. Default:
    https://github.com/<repo>/releases/download/v<version>/<app>.bin

Note: the version here must equal RG_FIRMWARE_VERSION in the target config.h
of the build you are publishing. Otherwise a device keeps seeing an update
after installing, or stops seeing any at all.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys

DEFAULT_REPO = "OviTech-BlockBoy/BlockBoy"
OUTPUT_DIR = os.path.join("build", "ota")


def read_config_value(target, name):
    """Reads a #define from the target config.h, so we don't have to guess."""
    path = os.path.join("components", "retro-go", "targets", target, "config.h")
    with open(path, encoding="utf-8") as f:
        source = f.read()

    match = re.search(r'#define\s+%s\s+"([^"]*)"' % re.escape(name), source)
    if match:
        return match.group(1)
    match = re.search(r"#define\s+%s\s+(\d+)" % re.escape(name), source)
    if match:
        return match.group(1)
    return None


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Publish a BlockBoy OTA update")
    parser.add_argument("version", help="Version of this release, e.g. 3.1.0")
    parser.add_argument("--target", default="0v1Tech-BlockBoy-N16R8")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub repo for the release assets")
    parser.add_argument("--tag", help="Release tag (default: v<version>)")
    parser.add_argument("--notes", default="", help="Short text shown on the device")
    parser.add_argument("--date", help="Date in the release (default: today)")
    parser.add_argument("--manifest-name", help="Filename of the manifest")
    parser.add_argument(
        "--base-url",
        help="Base URL for the .bin files instead of the GitHub release. "
             "For local testing, e.g. http://192.168.1.50:8000",
    )
    args = parser.parse_args()

    if not os.path.exists("rg_config.py"):
        sys.exit("Run this from the project directory (rg_config.py not found).")

    config = {}
    with open("rg_config.py", "rb") as f:
        exec(f.read(), config)

    apps = config["PROJECT_APPS"]
    flasher = config.get("PROJECT_FLASHER")

    # The version in the manifest must match what the firmware believes about
    # itself. If they diverge, a device keeps offering the same update after
    # installing it.
    built_version = read_config_value(args.target, "RG_FIRMWARE_VERSION")
    if built_version and built_version != args.version:
        sys.exit(
            "RG_FIRMWARE_VERSION in the target config.h is '%s', but you are publishing '%s'.\n"
            "Adjust config.h and rebuild, or publish version '%s'."
            % (built_version, args.version, built_version)
        )

    layout = int(read_config_value(args.target, "RG_LAYOUT_VERSION") or 1)
    model = read_config_value(args.target, "RG_OTA_MODEL")
    if not model:
        sys.exit("RG_OTA_MODEL not found in the target config.h")

    tag = args.tag or ("v" + args.version)

    if args.date:
        date = args.date
    else:
        import datetime

        date = datetime.date.today().isoformat()

    # Derive the name from the model, not the target name: the firmware
    # requests ota/<model-without-blockboy->.json and the two must be equal.
    manifest_name = args.manifest_name or (model.replace("blockboy-", "") + ".json")

    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    os.makedirs(OUTPUT_DIR)

    files = []
    total = 0

    for app, part in apps.items():
        binary = os.path.join(app, "build", app + ".bin")
        if not os.path.exists(binary):
            sys.exit("'%s' is not built. Run rg_tool.py build-img all first." % app)

        size = os.path.getsize(binary)
        limit = part[2]
        if size > limit:
            sys.exit(
                "'%s' is %d bytes, but the partition is %d. The layout is frozen;\n"
                "publishing this would brick devices in the field." % (app, size, limit)
            )

        shutil.copy2(binary, os.path.join(OUTPUT_DIR, app + ".bin"))

        if args.base_url:
            url = "%s/%s.bin" % (args.base_url.rstrip("/"), app)
        else:
            url = "https://github.com/%s/releases/download/%s/%s.bin" % (args.repo, tag, app)

        files.append(
            {
                "partition": app,
                "url": url,
                "size": size,
                "sha256": sha256_file(binary),
            }
        )
        total += size

    manifest = {
        "version": args.version,
        "model": model,
        "layout": layout,
        "date": date,
        "notes": args.notes,
        "files": files,
    }

    manifest_path = os.path.join(OUTPUT_DIR, manifest_name)
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print("\nOTA package %s ready in %s/\n" % (args.version, OUTPUT_DIR))
    for entry in files:
        print("  %-14s %8d bytes  %s" % (entry["partition"], entry["size"], entry["sha256"][:16] + "..."))
    print("\n  total: %.2f MB" % (total / 1048576))

    # The flasher is deliberately not part of the package: it runs during the
    # installation and cannot replace itself. New flasher = new .img via the
    # web flasher.
    if flasher:
        print("\n  ('%s' does not belong in an OTA package and was skipped)" % flasher[0])

    if args.base_url:
        print("\nLOCAL TEST BUILD -- points at %s" % args.base_url)
        print("\nNext steps:")
        print("  1. Start the test server:  python tools/serve_ota.py")
        print("  2. On the test device's SD card, put this in")
        print("     /BlockBoy/config/ota.json:")
        print('       { "ManifestURL": "%s/%s" }' % (args.base_url.rstrip("/"), manifest_name))
        print("  3. Settings > Firmware update on the device.")
        print("\n  The dialog shows '[TEST]' as long as that override is present.\n")
    else:
        print("\nNext steps:")
        print("  1. Create release '%s' at https://github.com/%s/releases/new" % (tag, args.repo))
        print("  2. Upload all .bin files from %s/ as assets" % OUTPUT_DIR)
        print("  3. Commit %s to ota/%s in the firmware repo" % (manifest_name, manifest_name))
        print("\n  Step 3 last: as soon as the manifest is live, devices start downloading.\n")


if __name__ == "__main__":
    main()
