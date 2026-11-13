#!/usr/bin/env python3
"""Records docs/img/demo.gif from the web demo: a recorded walk in the 2D view, as the page draws it.

The walk is the one the mall recordings name as their default (data/demo/msil/demos.json), so the GIF is
not picked for a good result. Needs the demo built and served (README, "Web demo"), Chrome, and

    .venv/bin/pip install playwright pillow
    .venv/bin/python viz/record_demo.py                 # http://127.0.0.1:8000 by default

It waits until the engine has computed the whole walk, then steps the page's timeline and takes one
picture of the map per frame.
"""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8000/")
    ap.add_argument("--scenario", help="menu value, e.g. real:site1_F2_5dda5afc or kit_tour (default: the mall default)")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "img" / "demo.gif")
    ap.add_argument("--step", type=float, default=2.0, help="seconds of the walk per frame")
    ap.add_argument("--fps", type=float, default=8.0)
    ap.add_argument("--width", type=int, default=720, help="GIF width in pixels")
    ap.add_argument("--colors", type=int, default=128)
    ap.add_argument("--zoom", type=float, default=1.8, help="2D zoom about the middle of the walk's waypoints")
    args = ap.parse_args()
    scenario = args.scenario or "real:" + json.loads((ROOT / "data/demo/msil/demos.json").read_text())["default"]

    frames = []
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="chrome", headless=True)
        page = browser.new_page(viewport={"width": 1500, "height": 1000})
        page.goto(args.url)
        page.wait_for_function(f"!!document.querySelector('#scenario option[value=\"{scenario}\"]')", timeout=60000)
        page.click("#view2d")
        page.select_option("#scenario", scenario)
        page.wait_for_function("run && run.finished && recorded.length > 0", timeout=900000)
        duration = page.evaluate("setPlaying(false), lastRecorded()")
        # Zoom in on the walk: canvas px = k * fitted px + (x, y), about the middle of the surveyed points.
        page.evaluate("""k => {
            const T = transform(), c = document.getElementById('map');
            const xs = waypoints.map(w => T.toX(w[1])), ys = waypoints.map(w => T.toY(w[2]));
            const cx = (Math.min(...xs) + Math.max(...xs)) / 2, cy = (Math.min(...ys) + Math.max(...ys)) / 2;
            zoom = { k, x: c.width / 2 - k * cx, y: c.height / 2 - k * cy };
        }""", args.zoom)
        t = 0.0
        while t <= duration:
            page.evaluate("t => seek(t)", t)
            page.wait_for_timeout(120)  # two animation frames: the page redraws on the next tick
            shot = Image.open(io.BytesIO(page.locator("#map").screenshot())).convert("RGB")
            frames.append(shot.resize((args.width, round(shot.height * args.width / shot.width)), Image.LANCZOS))
            t += args.step
        stats = page.inner_text("#stats").replace("\n", " ")
        browser.close()

    # One palette for every frame, so colours do not flicker: from a strip of frames across the walk, with
    # a method that keeps the few saturated colours (the estimate, the cells, the access points).
    picks = frames[:: max(1, len(frames) // 6)]
    strip = Image.new("RGB", (frames[0].width, frames[0].height * len(picks)))
    for i, f in enumerate(picks):
        strip.paste(f, (0, i * f.height))
    palette = strip.quantize(colors=args.colors, method=Image.Quantize.MAXCOVERAGE)
    quantized = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    ms = round(1000 / args.fps)
    quantized[0].save(args.out, save_all=True, append_images=quantized[1:], duration=[ms] * (len(quantized) - 1) + [2500],
                      loop=0, optimize=True)
    print(f"{args.out}: {len(frames)} frames, {args.out.stat().st_size / 1e6:.1f} MB, {scenario}, {duration:.0f} s walk")
    print(f"end-of-walk stats row: {stats}")


if __name__ == "__main__":
    main()
