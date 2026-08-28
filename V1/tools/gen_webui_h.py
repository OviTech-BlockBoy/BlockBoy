"""Regenerates launcher/main/webui.html.h from tools/blockboy-webui.html.

Run from anywhere:  python tools/gen_webui_h.py
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "tools", "blockboy-webui.html")
DST = os.path.join(REPO, "launcher", "main", "webui.html.h")

src = open(SRC, encoding="utf-8").read()

# The source file is head-content (title/meta/style) followed by body content.
i = src.rindex("</style>") + len("</style>")
head, body = src[:i], src[i:]
doc = ('<!DOCTYPE html><html><head><meta charset="utf-8">'
       + head + "</head><body>" + body + "</body></html>")

out = ["// Generated from tools/blockboy-webui.html - do not edit by hand.",
       "// Regenerate with: python tools/gen_webui_h.py",
       "static const char webui_html[] ="]
for line in doc.split("\n"):
    esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("??", '?\\?')
    out.append('"' + esc + '\\n"')
out.append(";")

open(DST, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print("wrote", DST, "-", len(doc), "bytes of HTML")
