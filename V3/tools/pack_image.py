"""Pack a flashable image from apps that have already been built.

Run with the ESP-IDF Python environment, from the source tree root:

    python tools/pack_image.py . 0v1Tech-BlockBoy-N16R8

Uses rg_tool.py's signing, signature verification, size checks and partition
layout. Only compilation is skipped. Set BLOCKBOY_SIGNING_KEY before running.
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
