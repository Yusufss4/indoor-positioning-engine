"""Checks the live page (live.html) without a phone.

    .venv/bin/python web/check_live.py [--url http://127.0.0.1:8000/] [--replay build/tools/ipe-replay]
                                       [--sim build/tools/ipe-sim] [--out DIR]

Needs the site served (README, "Web demo") and `pip install playwright` with Chrome installed.

A simulated office walk is turned back into what a phone's browser reports (devicemotion and
deviceorientation events) and fed to the page in headless Chrome, with a tap at the start and a tap
every ten seconds at the true position. It checks that

- the page's listeners take real DeviceMotionEvent / DeviceOrientationEvent objects,
- the estimate follows the walk (steps, compass and walls only; the page has no radio),
- the recording the page saves, replayed with ipe-replay, gives the estimates the page showed at every
  tap: the page and the native engine agree, and the recording holds everything the engine was fed.
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
SCENARIO = ROOT / "data/scenarios/office_nominal.json"
VENUE = "data/venues/office_floor.json"
MARK_EVERY_S = 10.0
CHUNK = 400  # sensor samples per call into the page


def read_trace(path):
    imu, truth = [], []
    for line in Path(path).read_text().splitlines():
        f = line.split(",")
        if f[0] == "imu":
            imu.append([float(v) for v in f[1:]])
        elif f[0] == "gt":
            truth.append([float(v) for v in f[1:4]])
    return imu, truth


def browser_events(imu, rotation_deg=0.0):
    """Engine-frame IMU rows -> what a phone lying flat, top edge forward, would report."""
    out = []
    for t, ax, ay, az, gx, gy, gz, mx, my, _ in imu:
        psi = math.atan2(mx, my)  # heading on the map, counter-clockwise from +x
        compass = (90.0 - math.degrees(psi) - rotation_deg) % 360.0
        out.append([t * 1000.0, ax, ay, az, math.degrees(gx), math.degrees(gy), math.degrees(gz), (360.0 - compass) % 360.0])
    return out


FEED = """(rows) => {
  for (const [tMs, ax, ay, az, beta, gamma, alpha, absAlpha] of rows) {
    onOrientation({ absolute: true, alpha: absAlpha }, tMs);
    onMotion({ accelerationIncludingGravity: { x: ax, y: ay, z: az }, rotationRate: { alpha, beta, gamma } }, tMs);
  }
  flush();
}"""


def truth_at(truth, t):
    return min(truth, key=lambda row: abs(row[0] - t))


def run(page, events, truth, taps):
    """Feeds the walk; `taps` is True for venues where the start is tapped. Returns the session's marks."""
    marks = []
    next_mark = MARK_EVERY_S
    started = not taps
    for k in range(0, len(events), CHUNK):
        chunk = events[k:k + CHUNK]
        # Split the chunk where a tap is due, so that taps land between the right samples.
        begin = 0
        for i, row in enumerate(chunk):
            t = row[0] / 1000.0
            due_start = not started and i > 0
            if due_start or t >= next_mark:
                page.evaluate(FEED, chunk[begin:i + 1])
                begin = i + 1
                _, x, y = truth_at(truth, t)
                page.evaluate("([x, y]) => tap(x, y)", [x, y])
                if due_start:
                    started = True
                else:
                    marks.append((t, x, y))
                    next_mark += MARK_EVERY_S
        if begin < len(chunk):
            page.evaluate(FEED, chunk[begin:])
        page.wait_for_function("(t) => session.engineT >= t - 0.5", arg=chunk[-1][0] / 1000.0 - events[0][0] / 1000.0,
                               timeout=120000)
    # A last mark, so that the replay (which ends at the last one) covers the whole walk.
    t, x, y = truth[-1]
    page.evaluate("([x, y]) => tap(x, y)", [x, y])
    marks.append((t, x, y))
    page.wait_for_function("session.marks.every((m) => m.error !== null)", timeout=60000)
    return marks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8000/")
    ap.add_argument("--replay", default=str(ROOT / "build/tools/ipe-replay"))
    ap.add_argument("--sim", default=str(ROOT / "build/tools/ipe-sim"))
    ap.add_argument("--out", type=Path, default=ROOT / ".cache/check_live")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    sim_trace = args.out / "simulated.csv"
    subprocess.run([args.sim, "--scenario", str(SCENARIO), "--out", str(sim_trace)], check=True, capture_output=True)
    imu, truth = read_trace(sim_trace)
    events = browser_events(imu)
    failures = []

    def check(ok, text):
        print(("ok   " if ok else "FAIL ") + text)
        if not ok:
            failures.append(text)

    with sync_playwright() as p:
        browser = p.chromium.launch(channel="chrome", headless=True)
        context = browser.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=3, is_mobile=True,
                                      has_touch=True, accept_downloads=True)
        page = context.new_page()
        problems = []
        page.on("console", lambda m: problems.append(f"console: {m.text}") if m.type == "error" else None)
        page.on("pageerror", lambda e: problems.append(f"pageerror: {e}"))
        page.goto(args.url + "live.html")

        # 1. Real events reach the page (open area, which starts by itself in the middle).
        page.click("#start")
        page.wait_for_function("session && session.ready", timeout=60000)
        page.evaluate("""() => {
          for (let k = 0; k < 20; ++k) {
            window.dispatchEvent(new DeviceOrientationEvent("deviceorientationabsolute", { absolute: true, alpha: 270 }));
            window.dispatchEvent(new DeviceMotionEvent("devicemotion", {
              accelerationIncludingGravity: { x: 0, y: 0, z: 9.81 }, rotationRate: { alpha: 0, beta: 0, gamma: 0 } }));
          }
        }""")
        page.wait_for_function("session.est && session.est.valid", timeout=30000)
        info = page.evaluate("({ samples: session.samples, compass: session.compass, est: session.est, cells: session.cellCount })")
        check(info["samples"] == 20 and abs(info["compass"] - 90) < 1e-9, f"real sensor events are taken: {info['samples']} samples, compass {info['compass']:.0f} deg")
        check(abs(info["est"]["x"] - 20) < 0.5 and abs(info["est"]["y"] - 20) < 0.5,
              f"the open area starts in its middle: {info['est']['x']:.2f}, {info['est']['y']:.2f} ({info['cells']} cells)")
        page.click("#start")  # stop

        # 2. The office walk on the office map: a tapped start, then a mark every ten seconds.
        page.select_option("#venue", VENUE)
        page.wait_for_function("venue && venue.levels[0].walls.length > 10")
        page.click("#start")
        page.wait_for_function("session && session.ready", timeout=60000)
        marks = run(page, events, truth, taps=True)
        state = page.evaluate("""({ marks: session.marks, steps: session.steps, busyMs: session.busyMs, t: session.engineT,
                                   cells: session.cellCount, status: document.getElementById("status").innerText,
                                   load: document.getElementById("r-load").innerText })""")
        errors = [m["error"] for m in state["marks"]]
        check(len(errors) == len(marks) and all(e >= 0 for e in errors), f"{len(errors)} marks scored")
        check(max(errors) < 3.0, f"the estimate follows the walk without radio: error at the marks mean "
              f"{sum(errors) / len(errors):.2f} m, worst {max(errors):.2f} m, {state['steps']} steps")
        print(f"     engine load in this browser: {state['load']} on {state['cells']} cells")
        page.screenshot(path=str(args.out / "live_office.png"))

        page.click("#start")  # stop
        with page.expect_download() as download:
            page.click("#save")
        saved = args.out / "recorded.csv"
        download.value.save_as(str(saved))
        hint = page.inner_text("#saved")
        check(VENUE in hint, f"the page names the replay command: {hint}")
        check(not problems, f"no page errors {problems[:3] if problems else ''}")
        browser.close()

    # 3. The saved recording through the native engine.
    est_csv = args.out / "replayed.csv"
    out = subprocess.run([args.replay, "--venue", str(ROOT / VENUE), "--trace", str(saved), "--warmup", "0",
                          "--out", str(est_csv)], check=True, capture_output=True, text=True)
    metrics = json.loads(out.stdout)
    rows = [line.split(",") for line in est_csv.read_text().splitlines()[1:]]
    # The first truth row is the start tap's... no: the start is a fix, not a mark. Rows are the marks.
    check(len(rows) == len(state["marks"]), f"the recording holds every mark: {len(rows)}")
    worst = max((abs(float(r[6]) - m["error"]) for r, m in zip(rows, state["marks"])), default=1e9)
    check(worst < 1e-3, f"ipe-replay of the recording gives the page's errors at every mark (largest difference {worst:.4f} m)")
    print(f"     native replay of the recording: mean {metrics['mean_m']:.2f} m, CEP95 {metrics['cep95_m']:.2f} m, "
          f"{metrics['steps']} steps")
    check(metrics["steps"] == state["steps"], "the same number of steps")
    recorded = saved.read_text()
    check("\nfix," in recorded and "# source: web live" in recorded, "the recording has the start fix and its source")

    print("\nFAILED" if failures else "\nall checks passed", f"(files in {args.out})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
