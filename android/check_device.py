#!/usr/bin/env python3
"""Checks the Android build of the engine against the native one, on an emulator or a phone.

    python3 android/check_device.py [--serial emulator-5554] [--replay build/tools/ipe-replay] [--no-build]

The device test of the `ipe` module (ReplayTest) feeds a recorded mall walk of the repository through the
Kotlin API and leaves two files on the device: the recording the engine kept, and its estimate at every
truth row. This script

- builds and installs the test, runs it, and fetches the two files;
- replays the recording with the native `ipe-replay`;
- compares the two at every truth row: the Android build and the native one must give the same position,
  and the recording must hold everything the engine was fed.

It needs `adb` (ANDROID_HOME or the PATH), a built `ipe-replay`, and a device or a running emulator. With
more than one device attached, --serial says which; nothing is installed on the others. The test app is
removed again afterwards.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PACKAGE = "io.github.yusufss4.ipe.test"
APK = HERE / "ipe" / "build" / "outputs" / "apk" / "androidTest" / "debug" / "ipe-debug-androidTest.apk"
VENUE = ROOT / "data" / "demo" / "msil" / "site1_F2" / "venue.json"
TOLERANCE_M = 0.005  # the files keep millimetres; the two builds use different maths libraries

failed = False


def check(ok: bool, what: str) -> None:
    global failed
    print(("ok   " if ok else "FAIL ") + what)
    failed |= not ok


def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def find_adb() -> str:
    for home in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"), str(Path.home() / "Android" / "Sdk")):
        if home and (Path(home) / "platform-tools" / "adb").exists():
            return str(Path(home) / "platform-tools" / "adb")
    adb = shutil.which("adb")
    if not adb:
        sys.exit("error: adb not found (set ANDROID_HOME)")
    return adb


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="the device to use (adb devices); needed when several are attached")
    ap.add_argument("--replay", type=Path, default=ROOT / "build" / "tools" / "ipe-replay")
    ap.add_argument("--out", type=Path, default=ROOT / ".cache" / "check_device")
    ap.add_argument("--no-build", action="store_true", help="use the test APK that is already built")
    ap.add_argument("--keep", action="store_true", help="leave the test app on the device")
    args = ap.parse_args()

    adb = find_adb()
    devices = [line.split("\t")[0] for line in run([adb, "devices"]).stdout.splitlines()[1:] if line.endswith("\tdevice")]
    if args.serial:
        if args.serial not in devices:
            sys.exit(f"error: {args.serial} is not attached (adb devices: {', '.join(devices) or 'none'})")
    elif len(devices) != 1:
        sys.exit(f"error: {len(devices)} devices attached ({', '.join(devices) or 'none'}): say which with --serial")
    serial = args.serial or devices[0]
    if not args.replay.exists():
        sys.exit(f"error: {args.replay} not found: build the native tools first")

    if not args.no_build:
        build = run([HERE / "gradlew", ":ipe:assembleDebugAndroidTest", "--console=plain", "-q"], cwd=HERE)
        if build.returncode != 0:
            sys.exit(f"error: the build failed\n{build.stdout}{build.stderr}")
    install = run([adb, "-s", serial, "install", "-r", "-t", APK])
    if install.returncode != 0:
        sys.exit(f"error: could not install the test on {serial}\n{install.stdout}{install.stderr}")

    args.out.mkdir(parents=True, exist_ok=True)
    try:
        test = run([adb, "-s", serial, "shell", "am", "instrument", "-w", f"{PACKAGE}/androidx.test.runner.AndroidJUnitRunner"])
        check("OK (" in test.stdout and "FAILURES" not in test.stdout,
              f"the device tests pass on {serial}" + ("" if "OK (" in test.stdout else f"\n{test.stdout}{test.stderr}"))
        for name in ("saved.csv", "shown.csv"):
            pulled = subprocess.run([adb, "-s", serial, "exec-out", "run-as", PACKAGE, "cat", f"files/{name}"], capture_output=True)
            (args.out / name).write_bytes(pulled.stdout)
    finally:
        if not args.keep:
            run([adb, "-s", serial, "uninstall", PACKAGE])

    shown = list(csv.DictReader((args.out / "shown.csv").open()))
    check(len(shown) >= 15, f"the device scored {len(shown)} truth rows")
    if failed:
        return 1

    replay = run([args.replay, "--venue", VENUE, "--trace", args.out / "saved.csv", "--warmup", "0", "--out", args.out / "native.csv"])
    if replay.returncode != 0:
        sys.exit(f"error: ipe-replay failed on the device's recording\n{replay.stderr}")
    native = list(csv.DictReader((args.out / "native.csv").open()))
    check(len(native) == len(shown), f"the recording holds every truth row: {len(native)}")

    worst = 0.0
    same_fix = same_level = True
    for dev, nat in zip(shown, native):
        valid = dev["valid"] == "1"
        same_fix &= valid == (nat["has_fix"] == "1")
        if valid:
            worst = max(worst, math.hypot(float(dev["x"]) - float(nat["est_x"]), float(dev["y"]) - float(nat["est_y"])))
            same_level &= dev["level"] == nat["est_level"]
    check(same_fix, "both have an estimate at the same truth rows")
    check(same_level, "both report the same floor")
    check(worst <= TOLERANCE_M, f"ipe-replay of the recording gives the device's positions (largest difference {worst:.4f} m)")

    errors = [float(r["error_m"]) for r in shown if r["valid"] == "1"]
    print(f"     on the device: mean error {sum(errors) / len(errors):.2f} m over {len(errors)} truth rows; files in {args.out}")
    print("FAILED" if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
