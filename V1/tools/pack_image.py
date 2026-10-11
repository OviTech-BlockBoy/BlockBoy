"""Packs a flashable .img from apps that were already built with ninja.

rg_tool.py build-img normally builds every app through idf.py first. On a machine
where idf.py cannot run (a broken ESP-IDF virtualenv, for instance) that step
fails before anything is packed. This runs the same build_image() from
rg_tool.py, with build_app() stubbed out, so it only assembles the image and
still signs and verifies every app against the key.

Run it with the ESP-IDF venv Python, from the tree root:

    python tools/pack_image.py . 0v1Tech-BlockBoy-N16R8

Everything rg_tool.py does when packing (ensure_signed, size checks, partition
table) still happens; only the compile step is skipped.

Usage: python pack_any.py <tree-dir> <target>

idf.py cannot run on this machine (broken ESP-IDF virtualenv), so the apps are
built with ninja first and this only assembles the .img from the .bin files that
are already there.
"""
import os
import re
import sys

tree, target = sys.argv[1], sys.argv[2]
os.chdir(tree)
os.environ.setdefault("IDF_TARGET", "esp32s3")
os.environ.setdefault("IDF_PATH", "C:/Espressif/frameworks/esp-idf-v5.5")

src = open("rg_tool.py", encoding="utf-8").read()
src = re.sub(r"(def build_app\([^)]*\):\n)", r"\1    return  # stubbed: ninja already built it\n", src, count=1)
sys.argv = ["rg_tool.py", "build-img", "all", "--target", target]
exec(compile(src, "rg_tool.py", "exec"), {"__name__": "__main__", "__file__": "rg_tool.py"})
