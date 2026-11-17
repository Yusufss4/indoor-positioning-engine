# The engine on Android

Two Gradle modules:

- **`ipe`**, a library: the engine of this checkout behind a Kotlin class, and the conversion from
  Android's sensors to the engine's input. It is what an app of your own depends on.
- **`example`**, a small app: it shows a venue and your position on it, and records walks.

The recordings are trace files ([docs/formats.md](../docs/formats.md#trace-csv)). `ipe-replay` replays
them, and `datasets/fit_radio_map.py` fits a venue's Wi-Fi radio map from them.

## Build and install

```bash
cd android
./gradlew :example:assembleDebug        # example/build/outputs/apk/debug/example-debug.apk
adb install -r example/build/outputs/apk/debug/example-debug.apk
```

- Needs the Android SDK (`ANDROID_HOME`, or `sdk.dir` in `android/local.properties`) with platform 37,
  NDK 28.2.13676358 and CMake 3.31.6 (`sdkmanager "ndk;28.2.13676358" "cmake;3.31.6"`). Gradle downloads
  the JDK 17 it builds with.
- The CI job `Android build` builds the same APK and keeps it as an artifact.
- Android 8.0 or later, `arm64-v8a` and `x86_64`.

## Using the app

1. Choose a bundled venue, or **Open venue…** for a venue file on the phone.
2. **Start**, and allow location and nearby devices: to Android, Wi-Fi and Bluetooth scans are location.
   Without the permissions the app runs on the motion sensors alone.
3. **Tap the map where you are.** The first tap gives the engine its start.
4. Walk, holding the phone in front of you with its top edge pointing where you go. Every later tap is a
   mark: "I am really here". The app shows how far off the estimate was, and the recording keeps the tap
   as ground truth. With **Taps correct the position** on, a tap also moves the estimate there.
5. **Stop**, then **Save walk**.

Drag the map to move it and pinch to zoom. The `−` and `+` buttons change the floor shown, which is also
the floor a tap is on.

**Switch off Wi-Fi scan throttling.** Android lets an app scan four times in two minutes. A position
from Wi-Fi, and a walk for a radio map, need a scan every few seconds: in the phone's Developer options,
switch off "Wi-Fi scan throttling". The app says so when Android refuses a scan.

## Your own building

1. **The map**: a venue file with walls ([docs/formats.md](../docs/formats.md#venue-datavenuesjson)).
2. **Walks**: open that venue in the app, walk the building, and tap your position at every turn. Save
   each walk. A reading is placed on the straight line between two taps, so a turn without a tap puts
   readings in the wrong place.
3. **The radio map**, on a computer:

   ```bash
   .venv/bin/python datasets/fit_radio_map.py --venue walls.json --traces walks/*.csv --out venue.json \
       --cross-validate
   ```

   `--cross-validate` prints the accuracy to expect ([docs/extending.md](../docs/extending.md#your-own-building)).
4. **Position**: open `venue.json` in the app.

## What the sensors become

| Engine input | From |
|---|---|
| Acceleration (steps) | `TYPE_ACCELEROMETER` as measured; only its magnitude is used |
| Turn rate | `TYPE_GYROSCOPE` about the direction that is up, from `TYPE_GRAVITY`: the phone need not lie flat |
| Heading | `TYPE_ROTATION_VECTOR`: the direction of the phone's top edge, turned into the map's frame with the venue's `georef` and the magnetic declination there. None when the top edge points up or down |
| Pressure | `TYPE_PRESSURE` |
| Wi-Fi | One scan after the other, each as one update. An access point is `w:<bssid>`; a reading is passed on once, in the first scan that reports it |
| Beacons | iBeacon packets as `b:<UUID>_<major>_<minor>`. Other Bluetooth devices are left out, because they move |

`SensorFrame` in the `ipe` module does the first three and has no Android types in it. A venue's beacon
ids must follow the two conventions above for the app to recognise them.

## In an app of your own

Add the module to your build (`include(":ipe")` with `project(":ipe").projectDir` pointing at
`android/ipe` of a checkout; it builds the engine from that checkout) and:

```kotlin
val engine = PositioningEngine(venueJson)             // settings chosen from the venue
val frame = SensorFrame(mapRotationDeg, declinationDeg)

// from the sensor callbacks, all on one thread, t in seconds on one clock
frame.onGravity(x, y, z); frame.onGyroscope(x, y, z); frame.onRotationVector(t, x, y, z, w)
val s = frame.sample(t, ax, ay, az)                   // at each accelerometer event
engine.onImu(t, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8])
engine.onScan(t, ids, rssi)                           // a Wi-Fi scan
engine.onPressure(t, hpa)
engine.onPositionFix(t, x, y, level, 1.0)             // a tap, a QR code at a known place

val e = engine.estimate()                             // valid, x, y, level, sigma, heading
```

`example/src/main/kotlin/.../Session.kt` is this, complete, with the Wi-Fi and Bluetooth scanning.

## Checks

- `./gradlew :ipe:testDebugUnitTest :example:testDebugUnitTest`: the sensor arithmetic on poses whose
  answer is known, and the beacon id. No device needed; CI runs them.
- `python3 android/check_device.py --serial <device>`: on an emulator or a phone, a recorded mall walk goes
  through the Kotlin API, and the native `ipe-replay` of the recording must give the same positions.
- **Not checked by any of this: a walk.** How well the heading and the Wi-Fi scanning do in a hand, on a
  given phone, shows only when someone walks with it.

## Left out

- Running with the screen off. The app keeps the screen on while a walk runs.
- A phone in a pocket or held upright: the heading is the direction of the top edge.
- iOS.
