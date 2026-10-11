#!/usr/bin/env python3
"""Prepares a built firmware as an OTA update.

Generates the manifest the BlockBoy fetches, and collects the app binaries that
must be uploaded as release assets.

Usage (from the project directory, after a successful build):

    python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
    python tools/publish_update.py 3.1.0 --notes "USB mode bug fix" "GBA crash fix"

Every --notes argument is one point. The device lists them under each other,
each with a dash in front, above Yes and No. That box has room for six points
of about 28 characters; this script measures them with the device's font and
refuses notes that would not fit. (Firmware older than x.1.2 draws an older box
with room for a single line, which only matters for the update to x.1.2.)

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


def format_notes(points):
    """One line per point, each starting with a dash."""
    lines = []
    for point in points:
        for line in point.splitlines():
            line = line.strip()
            if line:
                lines.append(line if line.startswith("-") else "- " + line)
    return "\n".join(lines)


def check_notes_fit(target, notes):
    """Refuses notes the update box on the device cannot show.

    Mirrors confirm_update() in launcher/main/updater.c and draw_dialog() in
    rg_gui.c: the box is at most 82% x 74% of the visible screen, and the
    versions, the notes, Yes and No all have to fit on one page. A point that
    wraps takes two lines, so each point has to fit the width as well.
    """
    if len(notes.encode("utf-8")) > 255:
        sys.exit("The notes are longer than the 255 bytes the device keeps.")

    font_path = os.path.join("components", "retro-go", "fonts", "VeraBold11.c")
    config_path = os.path.join("components", "retro-go", "targets", target, "config.h")
    with open(font_path, encoding="utf-8", errors="replace") as f:
        font = f.read()
    with open(config_path, encoding="utf-8") as f:
        margins = re.search(r"RG_SCREEN_VISIBLE_AREA\s+\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", f.read())
    left, top, right, bottom = (int(x) for x in margins.groups()) if margins else (0, 0, 0, 0)
    screen_width = int(read_config_value(target, "RG_SCREEN_WIDTH")) - left - right
    screen_height = int(read_config_value(target, "RG_SCREEN_HEIGHT")) - top - bottom

    # Glyph records: code (2 bytes), y offset, width, height, x offset, advance, bitmap.
    data = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", font[font.index(".data"):])]
    advance, i = {}, 0
    while i + 7 <= len(data) and (data[i] or data[i + 1]):
        advance[data[i] | (data[i + 1] << 8)] = data[i + 6]
        i += 7 + (data[i + 3] * data[i + 4] + 7) // 8

    def width(text):
        return 2 + sum(advance.get(ord(ch), 8) for ch in text)

    line_height = int(re.search(r"\.height\s*=\s*(\d+)", font).group(1)) + 2
    text_width = int(0.82 * screen_width) - width(": ") - 28
    # Box height minus padding and title, minus one line each for the versions, Yes and No.
    max_lines = (int(0.74 * screen_height) - 12 - (line_height + 4) - 3 * line_height) // line_height

    lines = notes.split("\n") if notes else []
    for line in lines:
        if width(line) > text_width:
            sys.exit("This point is %d px wide and the box holds %d px:\n  %s\nShorten it."
                     % (width(line), text_width, line))
    if len(lines) > max_lines:
        sys.exit("%d points, but the update box on the device has room for %d." % (len(lines), max_lines))


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
    parser.add_argument("--notes", nargs="*", default=[],
                        help="What changed, one argument per point (up to 6 short points)")
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

    config = {"os": os}  # rg_config.py reads BLOCKBOY_SIGNING_KEY via os, like rg_tool.py provides
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

    notes = format_notes(args.notes)
    check_notes_fit(args.target, notes)

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
        "notes": notes,
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
