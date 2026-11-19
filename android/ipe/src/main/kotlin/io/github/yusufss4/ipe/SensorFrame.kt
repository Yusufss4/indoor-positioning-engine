package io.github.yusufss4.ipe

import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Turns what Android's sensors report into the inertial sample the engine takes
 * ([PositioningEngine.onImu], docs/formats.md).
 *
 * The engine reads three things from a sample: how strongly the phone is accelerated (steps), how fast it
 * turns about the vertical, and which way the walker is heading on the map. So the phone need not lie
 * flat:
 *
 * - the acceleration is passed on as measured (`TYPE_ACCELEROMETER`), only its magnitude counts;
 * - the turn rate is the gyroscope's rate (`TYPE_GYROSCOPE`) about the direction that is up, which is where
 *   gravity points (`TYPE_GRAVITY`, or the smoothed acceleration on a phone without that sensor);
 * - the heading is the direction of the phone's top edge (`TYPE_ROTATION_VECTOR`), turned from the compass
 *   into the map's frame. It is the walking direction as long as the top edge points where the walker
 *   goes; with the phone upright, or in a pocket, it is not, and no heading is passed on when the top edge
 *   points mostly up or down.
 *
 * This class has no Android types in it, so the arithmetic can be tested on any JVM. Feed it every sensor
 * event; [sample] makes the engine's sample at each accelerometer event.
 *
 * @param mapRotationDeg the venue's `georef.rotation_deg`: the angle of the map's +x counter-clockwise from
 *   east. 0 for a venue without a georef, whose +x is then taken to point east.
 * @param declinationDeg magnetic declination at the venue [degrees, east positive]
 *   (`android.hardware.GeomagneticField`), since the rotation vector is relative to magnetic north.
 */
class SensorFrame(private val mapRotationDeg: Double = 0.0, private val declinationDeg: Double = 0.0) {
    private var up: DoubleArray? = null       // unit vector pointing up, in the phone's axes
    private var gravitySensor = false
    private var smoothed: DoubleArray? = null  // the acceleration, low-passed: gravity on a phone without the sensor
    private val gyro = DoubleArray(3)
    private var heading = Double.NaN          // on the map [rad], counter-clockwise from +x
    private var headingTime = Double.NEGATIVE_INFINITY

    /** `TYPE_GRAVITY` [m/s^2]: the part of the acceleration that gravity causes, which points up. */
    fun onGravity(x: Double, y: Double, z: Double) {
        gravitySensor = true
        up = unit(x, y, z) ?: up
    }

    /** `TYPE_GYROSCOPE` [rad/s] about the phone's axes. */
    fun onGyroscope(x: Double, y: Double, z: Double) {
        gyro[0] = x
        gyro[1] = y
        gyro[2] = z
    }

    /**
     * `TYPE_ROTATION_VECTOR` at time [t] in seconds: the quaternion's x, y, z and, when the sensor gives it,
     * w. It turns the phone's axes into east, north and up.
     */
    fun onRotationVector(t: Double, x: Double, y: Double, z: Double, w: Double = Double.NaN) {
        val qw = if (w.isNaN()) sqrt((1.0 - x * x - y * y - z * z).coerceAtLeast(0.0)) else w
        // Where the phone's top edge (+y) points: the second column of the rotation matrix.
        val east = 2 * x * y - 2 * z * qw
        val north = 1 - 2 * x * x - 2 * z * z
        if (hypot(east, north) < MIN_HORIZONTAL) {
            heading = Double.NaN  // the top edge points up or down: it says nothing about the walking direction
            return
        }
        heading = atan2(north, east) - Math.toRadians(declinationDeg + mapRotationDeg)
        headingTime = t
    }

    /**
     * The engine's inertial sample for an accelerometer reading at time [t] in seconds: nine numbers for
     * [PositioningEngine.onImu] after the time (acceleration, gyroscope, magnetic field).
     */
    fun sample(t: Double, ax: Double, ay: Double, az: Double): DoubleArray {
        if (!gravitySensor) {
            // Up is where the acceleration points on average: smoothed over about a second at 50 Hz.
            val old = smoothed
            val now = if (old == null) doubleArrayOf(ax, ay, az) else
                doubleArrayOf(old[0] + SMOOTHING * (ax - old[0]), old[1] + SMOOTHING * (ay - old[1]), old[2] + SMOOTHING * (az - old[2]))
            smoothed = now
            up = unit(now[0], now[1], now[2]) ?: up
        }
        val u = up
        val yawRate = if (u == null) 0.0 else gyro[0] * u[0] + gyro[1] * u[1] + gyro[2] * u[2]
        var mx = 0.0
        var my = 0.0
        if (!heading.isNaN() && t - headingTime < HEADING_STALE_S) {
            // The engine reads the heading back as atan2(mx, my).
            mx = FIELD_UT * sin(heading)
            my = FIELD_UT * cos(heading)
        }
        return doubleArrayOf(ax, ay, az, 0.0, 0.0, yawRate, mx, my, 0.0)
    }

    /** The heading on the map last passed on [rad, counter-clockwise from +x], or NaN. */
    val mapHeading: Double get() = if (heading.isNaN()) heading else atan2(sin(heading), cos(heading))

    private fun unit(x: Double, y: Double, z: Double): DoubleArray? {
        val n = sqrt(x * x + y * y + z * z)
        return if (n < 1e-6) null else doubleArrayOf(x / n, y / n, z / n)
    }

    companion object {
        /** Strength of the horizontal field written for a heading [uT]; only its direction is read. */
        const val FIELD_UT = 30.0

        /** A heading older than this is not passed on [s]. */
        const val HEADING_STALE_S = 1.5

        private const val MIN_HORIZONTAL = 0.35  // of the top edge's unit vector: about 70 degrees from level
        private const val SMOOTHING = 0.03

        /** Degrees from an angle in radians, in (-180, 180]. */
        fun degrees(rad: Double): Double = atan2(sin(rad), cos(rad)) * 180.0 / PI
    }
}
