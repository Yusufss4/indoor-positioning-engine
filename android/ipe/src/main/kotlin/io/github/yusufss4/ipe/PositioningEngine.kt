package io.github.yusufss4.ipe

/** What to draw: the position on the map in metres, the floor and how sure the engine is. */
class Estimate(
    /** False until a radio reading of a known transmitter or a position fix has arrived. */
    val valid: Boolean,
    val x: Double,
    val y: Double,
    /** Level index: 0 is the venue's lowest floor. */
    val level: Int,
    /** Radius of the uncertainty [m]. */
    val sigma: Double,
    /** Walking direction [rad], counter-clockwise from the map's +x. */
    val heading: Double,
)

/**
 * The indoor positioning engine on a venue, fed one sensor event at a time.
 *
 * It keeps everything it is fed as a trace ([traceCsv]): a recording that `ipe-replay` replays to the same
 * estimates, and that `datasets/fit_radio_map.py` fits a venue's radio map from.
 *
 * - Times are seconds on one clock, in the order the events happened.
 * - Positions are metres in the venue's frame; levels are indices (0 is the lowest floor).
 * - One thread at a time: the object is not synchronised. Building it takes a moment on a large venue.
 * - [close] frees the native engine.
 *
 * @param venueJson the venue file's text (docs/formats.md)
 * @param configPatch a JSON merge patch on the settings the venue calls for, or empty
 * @throws IllegalArgumentException when the venue or the patch does not parse
 */
class PositioningEngine(venueJson: String, configPatch: String = "") : AutoCloseable {
    private var handle: Long = nativeCreate(venueJson.toByteArray(Charsets.UTF_8), configPatch.toByteArray(Charsets.UTF_8))
    private val scratch = DoubleArray(6)

    /** Number of floors of the venue. */
    val levelCount: Int get() = nativeLevelCount(live())

    /** Steps the engine has counted. */
    val steps: Int get() = nativeSteps(live())

    /** Time of the last event [s]. */
    val time: Double get() = nativeTime(live())

    /** An inertial sample in the engine's frame; [SensorFrame] makes one from Android's sensors. */
    fun onImu(t: Double, ax: Double, ay: Double, az: Double, gx: Double, gy: Double, gz: Double, mx: Double, my: Double, mz: Double) =
        nativeOnImu(live(), t, ax, ay, az, gx, gy, gz, mx, my, mz)

    /** One packet of one transmitter, for example an iBeacon advertisement. */
    fun onBle(t: Double, id: String, rssi: Double) = nativeOnBle(live(), t, id, rssi)

    /** The readings one Wi-Fi scan delivered, applied as one update. Transmitters the venue does not list are recorded only. */
    fun onScan(t: Double, ids: Array<String>, rssi: DoubleArray) {
        require(ids.size == rssi.size) { "a scan needs one signal strength per id" }
        nativeOnScan(live(), t, ids, rssi)
    }

    /** Barometer [hPa]. */
    fun onPressure(t: Double, hpa: Double) = nativeOnPressure(live(), t, hpa)

    /** A position known from outside the engine: a tap on the map, a tag at a known place. A negative [level] means the floor is not known. */
    fun onPositionFix(t: Double, x: Double, y: Double, level: Int, sigmaM: Double) = nativeOnPositionFix(live(), t, x, y, level, sigmaM)

    /**
     * A place the user says they are at. It is kept as ground truth in the recording and not shown to the
     * engine. Returns the estimate's distance from it [m], or -1 without an estimate or on another floor.
     */
    fun mark(t: Double, x: Double, y: Double, level: Int): Double = nativeMark(live(), t, x, y, level)

    fun estimate(): Estimate {
        nativeEstimate(live(), scratch)
        return Estimate(scratch[0] != 0.0, scratch[1], scratch[2], scratch[3].toInt(), scratch[4], scratch[5])
    }

    /** Adds a `# key: value` line to the recording. */
    fun setMetadata(key: String, value: String) = nativeSetMetadata(live(), key, value)

    /** Everything fed so far, as a trace file. */
    fun traceCsv(): String = String(nativeTrace(live()), Charsets.UTF_8)

    override fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0L
        }
    }

    private fun live(): Long {
        check(handle != 0L) { "the engine is closed" }
        return handle
    }

    private companion object {
        init {
            System.loadLibrary("ipe_jni")
        }

        @JvmStatic external fun nativeCreate(venueJson: ByteArray, configPatch: ByteArray): Long
        @JvmStatic external fun nativeDestroy(handle: Long)
        @JvmStatic external fun nativeOnImu(handle: Long, t: Double, ax: Double, ay: Double, az: Double, gx: Double, gy: Double, gz: Double, mx: Double, my: Double, mz: Double)
        @JvmStatic external fun nativeOnBle(handle: Long, t: Double, id: String, rssi: Double)
        @JvmStatic external fun nativeOnScan(handle: Long, t: Double, ids: Array<String>, rssi: DoubleArray)
        @JvmStatic external fun nativeOnPressure(handle: Long, t: Double, hpa: Double)
        @JvmStatic external fun nativeOnPositionFix(handle: Long, t: Double, x: Double, y: Double, level: Int, sigmaM: Double)
        @JvmStatic external fun nativeMark(handle: Long, t: Double, x: Double, y: Double, level: Int): Double
        @JvmStatic external fun nativeEstimate(handle: Long, out: DoubleArray)
        @JvmStatic external fun nativeLevelCount(handle: Long): Int
        @JvmStatic external fun nativeSteps(handle: Long): Int
        @JvmStatic external fun nativeTime(handle: Long): Double
        @JvmStatic external fun nativeSetMetadata(handle: Long, key: String, value: String)
        @JvmStatic external fun nativeTrace(handle: Long): ByteArray
    }
}
