package io.github.yusufss4.ipe.example

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.GeomagneticField
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import io.github.yusufss4.ipe.Estimate
import io.github.yusufss4.ipe.PositioningEngine
import io.github.yusufss4.ipe.SensorFrame
import java.util.Locale

/** What the screen shows of a running walk. */
class Snapshot(
    val estimate: Estimate,
    val seconds: Double,
    val steps: Int,
    val scans: Int,
    /** Seconds between the last two Wi-Fi scans, or NaN before the second. */
    val scanIntervalS: Double,
    val beaconPackets: Int,
    /** Android refused a Wi-Fi scan: it limits apps to a few scans per two minutes. */
    val throttled: Boolean,
    val hasFix: Boolean,
    /** False until the first motion sensor event: there is no clock for taps before it. */
    val sensorsRunning: Boolean,
)

/**
 * One walk: the engine on a thread of its own, fed by this phone's sensors and radios.
 *
 * - Motion sensors at about 50 Hz, turned into the engine's frame by [SensorFrame]; the barometer.
 * - Wi-Fi scans, one after the other, each given to the engine as one scan. An access point is `w:` and
 *   its BSSID; a reading is passed on once, in the first scan that reports it.
 * - iBeacon packets, as `b:<uuid>_<major>_<minor>`. Other Bluetooth devices are left out: they move.
 *
 * Every event's time is seconds since the first sensor event, on the clock the sensors stamp their events
 * with. Everything the engine is fed is in its recording ([trace]).
 */
class Session(
    context: Context,
    private val venueJson: String,
    private val venue: VenueMap,
    private val onError: (String) -> Unit,
    private val onSnapshot: (Snapshot) -> Unit,
    private val onMark: (x: Double, y: Double, level: Int, setsPosition: Boolean, errorM: Double) -> Unit,
) : SensorEventListener {
    private val app = context.applicationContext
    private val thread = HandlerThread("ipe-engine").apply { start() }
    private val engineThread = Handler(thread.looper)
    private val ui = Handler(Looper.getMainLooper())
    private val sensors = app.getSystemService(SensorManager::class.java)
    private val wifi = app.getSystemService(WifiManager::class.java)
    private val bluetooth = app.getSystemService(BluetoothManager::class.java)

    // Everything below belongs to the engine thread.
    private var engine: PositioningEngine? = null
    private var frame = SensorFrame()
    private var running = false
    private var startNs = -1L        // sensor clock at the first event
    private var sensorClockLagNs = 0L  // elapsed-realtime clock minus sensor clock
    private var hasFix = false
    private var scans = 0
    private var lastScanS = Double.NaN
    private var scanIntervalS = Double.NaN
    private var beaconPackets = 0
    private var throttled = false
    private val lastSeen = HashMap<String, Long>()  // BSSID -> the time of its last reading passed on

    fun start() = engineThread.post {
        try {
            val e = PositioningEngine(venueJson)
            e.setMetadata("phone", "${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE}")
            engine = e
        } catch (ex: IllegalArgumentException) {
            ui.post { onError(ex.message ?: "the venue could not be loaded") }
            return@post
        }
        val declination = venue.origin?.let { (lat, lon) ->
            GeomagneticField(lat.toFloat(), lon.toFloat(), 0f, System.currentTimeMillis()).declination.toDouble()
        } ?: 0.0
        frame = SensorFrame(venue.rotationDeg, declination)
        running = true
        for (type in intArrayOf(Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_GRAVITY, Sensor.TYPE_ROTATION_VECTOR)) {
            sensors.getDefaultSensor(type)?.let { sensors.registerListener(this, it, IMU_PERIOD_US, engineThread) }
        }
        sensors.getDefaultSensor(Sensor.TYPE_PRESSURE)?.let { sensors.registerListener(this, it, PRESSURE_PERIOD_US, engineThread) }
        startRadio()
        engineThread.post(report)
    }

    /** Stops the sensors; the recording stays until [close]. */
    fun stop() = engineThread.post {
        if (!running) return@post
        running = false
        sensors.unregisterListener(this)
        stopRadio()
        engineThread.removeCallbacks(report)
        engineThread.removeCallbacks(askForScan)
        publish()
    }

    fun close() {
        stop()
        engineThread.post {
            engine?.close()
            engine = null
        }
        thread.quitSafely()
    }

    /**
     * The user says "I am here". Always a mark: ground truth in the recording, which is what a walk for
     * fitting a radio map needs at every turn. The first tap, and every tap with [correct], is then also
     * given to the engine as a position fix; the mark comes first, so its error is the engine's own.
     */
    fun tap(x: Double, y: Double, level: Int, correct: Boolean) = engineThread.post {
        val e = engine ?: return@post
        if (!running || startNs < 0) return@post
        val error = e.mark(now(), x, y, level)
        val setsPosition = !hasFix || correct
        if (setsPosition) {
            e.onPositionFix(now(), x, y, level, TAP_SIGMA_M)
            hasFix = true
        }
        ui.post { onMark(x, y, level, setsPosition, error) }
        publish()
    }

    /** The recording so far, handed to [use] on the engine's thread; null before anything was recorded. */
    fun trace(use: (String?) -> Unit) = engineThread.post {
        val e = engine
        use(if (e == null || startNs < 0) null else e.traceCsv())
    }

    // ---- Sensors ------------------------------------------------------------------------------------

    override fun onSensorChanged(event: SensorEvent) {
        val e = engine ?: return
        if (!running) return
        if (startNs < 0) {
            startNs = event.timestamp
            sensorClockLagNs = SystemClock.elapsedRealtimeNanos() - event.timestamp
        }
        val t = (event.timestamp - startNs) / 1e9
        val v = event.values
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                val s = frame.sample(t, v[0].toDouble(), v[1].toDouble(), v[2].toDouble())
                e.onImu(t, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8])
            }
            Sensor.TYPE_GYROSCOPE -> frame.onGyroscope(v[0].toDouble(), v[1].toDouble(), v[2].toDouble())
            Sensor.TYPE_GRAVITY -> frame.onGravity(v[0].toDouble(), v[1].toDouble(), v[2].toDouble())
            Sensor.TYPE_ROTATION_VECTOR ->
                frame.onRotationVector(t, v[0].toDouble(), v[1].toDouble(), v[2].toDouble(), if (v.size > 3) v[3].toDouble() else Double.NaN)
            Sensor.TYPE_PRESSURE -> e.onPressure(t, v[0].toDouble())
        }
    }

    override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) = Unit

    /** Seconds since the walk's first sensor event, for an event that happens now. */
    private fun now(): Double = seconds(SystemClock.elapsedRealtimeNanos())

    /** The same for a time on the elapsed-realtime clock, which Wi-Fi and Bluetooth results carry. */
    private fun seconds(elapsedRealtimeNs: Long): Double = (elapsedRealtimeNs - sensorClockLagNs - startNs) / 1e9

    // ---- Radio --------------------------------------------------------------------------------------

    private fun granted(permission: String) = app.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private val wifiResults = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = onWifiScan(intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, true))
    }

    private val askForScan = Runnable { requestScan() }

    @Suppress("DEPRECATION")  // startScan: there is no other way to ask for a scan
    private fun requestScan() {
        if (!running) return
        throttled = !wifi.startScan()
        // Asked again if no result comes, and at once after a result (onWifiScan).
        engineThread.removeCallbacks(askForScan)
        engineThread.postDelayed(askForScan, SCAN_RETRY_MS)
    }

    @SuppressLint("MissingPermission")
    private fun onWifiScan(updated: Boolean) {
        val e = engine ?: return
        if (!running) return
        if (updated && startNs >= 0) {
            val ids = ArrayList<String>()
            val rssi = ArrayList<Double>()
            var newestUs = Long.MIN_VALUE
            for (r in wifi.scanResults) {
                val bssid = r.BSSID ?: continue
                // A scan repeats an access point until it is heard again: each reading is passed on once.
                if (lastSeen.put(bssid, r.timestamp) == r.timestamp) continue
                ids.add("w:" + bssid.lowercase(Locale.ROOT))
                rssi.add(r.level.toDouble())
                if (r.timestamp > newestUs) newestUs = r.timestamp
            }
            if (ids.isNotEmpty()) {
                // The scan's time is its newest reading's, as in the data sets' traces.
                val t = seconds(newestUs * 1000).coerceIn(0.0, now())
                e.onScan(t, ids.toTypedArray(), rssi.toDoubleArray())
                scans += 1
                if (!lastScanS.isNaN()) scanIntervalS = t - lastScanS
                lastScanS = t
            }
        }
        engineThread.removeCallbacks(askForScan)
        engineThread.postDelayed(askForScan, SCAN_PAUSE_MS)
    }

    private val beacons = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val id = iBeaconId(result.scanRecord?.getManufacturerSpecificData(APPLE)) ?: return
            val at = result.timestampNanos
            val dbm = result.rssi.toDouble()
            engineThread.post {
                val e = engine ?: return@post
                if (!running || startNs < 0) return@post
                e.onBle(seconds(at).coerceIn(0.0, now()), id, dbm)
                beaconPackets += 1
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun startRadio() {
        if (!granted(Manifest.permission.ACCESS_FINE_LOCATION)) return
        if (Build.VERSION.SDK_INT < 33 || granted(Manifest.permission.NEARBY_WIFI_DEVICES)) {
            app.registerReceiver(wifiResults, IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION), null, engineThread)
            requestScan()
        }
        if (Build.VERSION.SDK_INT < 31 || granted(Manifest.permission.BLUETOOTH_SCAN)) {
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
            runCatching { bluetooth?.adapter?.bluetoothLeScanner?.startScan(null, settings, beacons) }
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopRadio() {
        runCatching { app.unregisterReceiver(wifiResults) }
        runCatching { bluetooth?.adapter?.bluetoothLeScanner?.stopScan(beacons) }
    }

    // ---- To the screen ------------------------------------------------------------------------------

    private val report = object : Runnable {
        override fun run() {
            publish()
            if (running) engineThread.postDelayed(this, REPORT_MS)
        }
    }

    private fun publish() {
        val e = engine ?: return
        val snapshot = Snapshot(e.estimate(), if (startNs < 0) 0.0 else e.time, e.steps, scans, scanIntervalS, beaconPackets, throttled, hasFix, startNs >= 0)
        ui.post { onSnapshot(snapshot) }
    }

    companion object {
        private const val IMU_PERIOD_US = 20_000        // 50 Hz, the rate the step detector expects
        private const val PRESSURE_PERIOD_US = 200_000
        private const val TAP_SIGMA_M = 1.0             // how well a tap places the user
        private const val SCAN_PAUSE_MS = 200L          // between a scan's result and the next request
        private const val SCAN_RETRY_MS = 4_000L
        private const val REPORT_MS = 200L
        private const val APPLE = 0x004C

        /** `b:<uuid>_<major>_<minor>` of an iBeacon advertisement's manufacturer data, or null for anything else. */
        fun iBeaconId(data: ByteArray?): String? {
            if (data == null || data.size < 22 || data[0] != 0x02.toByte() || data[1] != 0x15.toByte()) return null
            val hex = StringBuilder(36)
            for (i in 2 until 18) {
                if (i == 6 || i == 8 || i == 10 || i == 12) hex.append('-')
                hex.append(String.format(Locale.ROOT, "%02X", data[i]))
            }
            val major = ((data[18].toInt() and 0xFF) shl 8) or (data[19].toInt() and 0xFF)
            val minor = ((data[20].toInt() and 0xFF) shl 8) or (data[21].toInt() and 0xFF)
            return "b:${hex}_${major}_$minor"
        }
    }
}
