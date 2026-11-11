#!/usr/bin/env python3
# Renders assets/icon.svg to assets/icon.ico and a 256 px png, with headless Chromium and Pillow.
# 32 px and below come from icon-small.svg, which is simpler so it still reads in the tray.
# Set CHROME to the browser binary if `chromium` isn't on PATH; Edge works too.
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image

ASSETS = pathlib.Path(__file__).resolve().parent.parent / "assets"
CHROME = os.environ.get("CHROME", "chromium")
SIZES = [256, 64, 48, 40, 32, 24, 20, 16]
SMALL = 32


def render(svg, tmp):
    # render once, big, and downscale: Chromium is unreliable with tiny windows
    page = tmp / (svg.stem + ".html")
    out = tmp / (svg.stem + ".png")
    page.write_text("<html><head><style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}"
                    "img{display:block;width:1024px;height:1024px}</style></head>"
                    f"<body><img src='{svg.as_uri()}'></body></html>")
    subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
                    f"--user-data-dir={tmp / ('profile-' + svg.stem)}", "--allow-file-access-from-files",
                    "--default-background-color=00000000", "--force-device-scale-factor=1",
                    "--window-size=1024,1024", f"--screenshot={out}", page.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Edge on Windows hands the work to another process and returns straight away
    last = -1
    for _ in range(100):
        size = out.stat().st_size if out.exists() else -1
        if size > 0 and size == last:
            return Image.open(out).convert("RGBA").crop((0, 0, 1024, 1024))
        last = size
        time.sleep(0.3)
    sys.exit(f"{CHROME} didn't write {out.name}")


def main():
    tmp = pathlib.Path(tempfile.mkdtemp())
    big = render(ASSETS / "icon.svg", tmp)
    small = render(ASSETS / "icon-small.svg", tmp) if (ASSETS / "icon-small.svg").exists() else big
    images = [(small if s <= SMALL else big).resize((s, s), Image.LANCZOS) for s in SIZES]
    images[0].save(ASSETS / "icon-256.png")
    images[0].save(ASSETS / "icon.ico", sizes=[(s, s) for s in SIZES], append_images=images[1:])
    for _ in range(20):  # the browser may hold on to its profile for a moment
        shutil.rmtree(tmp, ignore_errors=True)
        if not tmp.exists():
            break
        time.sleep(0.5)
    print("assets/icon.ico written")


if __name__ == "__main__":
    sys.exit(main())
