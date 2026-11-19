package io.github.yusufss4.ipe

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The arithmetic of [SensorFrame] on poses whose answer is known. */
class SensorFrameTest {
    private val g = 9.81

    /** Quaternion (x, y, z, w) of a rotation by [deg] about a unit axis. */
    private fun rotation(deg: Double, ax: Double, ay: Double, az: Double): DoubleArray {
        val h = Math.toRadians(deg) / 2
        return doubleArrayOf(ax * sin(h), ay * sin(h), az * sin(h), cos(h))
    }

    /** a then b, both about world axes: the product b * a. */
    private fun then(a: DoubleArray, b: DoubleArray): DoubleArray {
        val (ax, ay, az, aw) = a
        val (bx, by, bz, bw) = b
        return doubleArrayOf(
            bw * ax + bx * aw + by * az - bz * ay,
            bw * ay - bx * az + by * aw + bz * ax,
            bw * az + bx * ay - by * ax + bz * aw,
            bw * aw - bx * ax - by * ay - bz * az,
        )
    }

    /** Flat on a table with the top edge pointing [compassDeg] clockwise from north. */
    private fun flat(compassDeg: Double) = rotation(-compassDeg, 0.0, 0.0, 1.0)

    /** The heading on the map [degrees] the engine reads from a sample. */
    private fun headingDeg(sample: DoubleArray) = Math.toDegrees(atan2(sample[6], sample[7]))

    private fun SensorFrame.rotationVector(t: Double, q: DoubleArray) = onRotationVector(t, q[0], q[1], q[2], q[3])

    @Test
    fun aFlatPhonePointingNorthHeadsUpTheMapsYAxis() {
        val frame = SensorFrame()
        frame.rotationVector(1.0, flat(0.0))
        val s = frame.sample(1.0, 0.0, 0.0, g)
        assertEquals(90.0, headingDeg(s), 1e-9)
        assertEquals(SensorFrame.FIELD_UT, s[6], 1e-9)
        assertEquals(0.0, s[7], 1e-9)
    }

    @Test
    fun compassDirectionsBecomeMapHeadings() {
        for ((compass, map) in listOf(90.0 to 0.0, 180.0 to -90.0, 270.0 to 180.0, 45.0 to 45.0)) {
            val frame = SensorFrame()
            frame.rotationVector(0.0, flat(compass))
            val got = headingDeg(frame.sample(0.0, 0.0, 0.0, g))
            assertEquals("compass $compass", 0.0, SensorFrame.degrees(Math.toRadians(got - map)), 1e-9)
        }
    }

    @Test
    fun theMapsRotationAndTheDeclinationAreTakenOff() {
        val rotated = SensorFrame(mapRotationDeg = 30.0)
        rotated.rotationVector(0.0, flat(0.0))
        assertEquals(60.0, headingDeg(rotated.sample(0.0, 0.0, 0.0, g)), 1e-9)

        // Magnetic north is 5 degrees east of true north: a phone pointing at it points 5 degrees east.
        val declined = SensorFrame(declinationDeg = 5.0)
        declined.rotationVector(0.0, flat(0.0))
        assertEquals(85.0, headingDeg(declined.sample(0.0, 0.0, 0.0, g)), 1e-9)
    }

    @Test
    fun aPhoneTiltedInTheHandKeepsItsHeading() {
        // Pointing north-east, then the top edge lifted 40 degrees towards the walker's face: a rotation
        // about the horizontal axis across the walking direction.
        val across = doubleArrayOf(cos(Math.toRadians(-45.0)), sin(Math.toRadians(-45.0)))
        val q = then(flat(45.0), rotation(40.0, across[0], across[1], 0.0))
        val frame = SensorFrame()
        frame.rotationVector(0.0, q)
        assertEquals(45.0, headingDeg(frame.sample(0.0, 0.0, 0.0, g)), 1e-9)
    }

    @Test
    fun anUprightPhoneGivesNoHeading() {
        val frame = SensorFrame()
        frame.rotationVector(0.0, then(flat(0.0), rotation(85.0, 1.0, 0.0, 0.0)))
        val s = frame.sample(0.0, 0.0, g, 0.0)
        assertEquals(0.0, s[6], 0.0)
        assertEquals(0.0, s[7], 0.0)
        assertTrue(frame.mapHeading.isNaN())
    }

    @Test
    fun aHeadingGoesStale() {
        val frame = SensorFrame()
        frame.rotationVector(10.0, flat(0.0))
        assertEquals(90.0, headingDeg(frame.sample(11.0, 0.0, 0.0, g)), 1e-9)
        val late = frame.sample(10.0 + SensorFrame.HEADING_STALE_S + 0.1, 0.0, 0.0, g)
        assertEquals(0.0, late[6], 0.0)
        assertEquals(0.0, late[7], 0.0)
    }

    @Test
    fun theRotationVectorMayComeWithoutItsFourthNumber() {
        val q = flat(120.0)
        val frame = SensorFrame()
        frame.onRotationVector(0.0, q[0], q[1], q[2])
        assertEquals(-30.0, headingDeg(frame.sample(0.0, 0.0, 0.0, g)), 1e-9)
    }

    @Test
    fun theTurnRateIsTheRateAboutTheVertical() {
        val turn = 0.5  // rad/s, counter-clockwise seen from above

        val flat = SensorFrame()
        flat.onGravity(0.0, 0.0, g)
        flat.onGyroscope(0.0, 0.0, turn)
        assertEquals(turn, flat.sample(0.0, 0.0, 0.0, g)[5], 1e-12)

        // Tilted 40 degrees: up is partly along the phone's y, and so is the turn.
        val t = Math.toRadians(40.0)
        val tilted = SensorFrame()
        tilted.onGravity(0.0, g * sin(t), g * cos(t))
        tilted.onGyroscope(0.0, turn * sin(t), turn * cos(t))
        assertEquals(turn, tilted.sample(0.0, 0.0, g * sin(t), g * cos(t))[5], 1e-12)

        // Screen down: the phone's z points at the floor.
        val down = SensorFrame()
        down.onGravity(0.0, 0.0, -g)
        down.onGyroscope(0.0, 0.0, -turn)
        assertEquals(turn, down.sample(0.0, 0.0, 0.0, -g)[5], 1e-12)

        // A roll about the walking direction is not a turn.
        val rolling = SensorFrame()
        rolling.onGravity(0.0, 0.0, g)
        rolling.onGyroscope(0.0, 2.0, 0.0)
        assertEquals(0.0, rolling.sample(0.0, 0.0, 0.0, g)[5], 1e-12)
    }

    @Test
    fun withoutAGravitySensorUpIsTheSmoothedAcceleration() {
        val t = Math.toRadians(30.0)
        val frame = SensorFrame()
        frame.onGyroscope(0.0, 0.4 * sin(t), 0.4 * cos(t))
        var s = DoubleArray(9)
        for (i in 0 until 200) {
            // Steps shake the phone along its own z; the average still points up.
            val shake = if (i % 2 == 0) 3.0 else -3.0
            s = frame.sample(i * 0.02, 0.0, g * sin(t), g * cos(t) + shake)
        }
        assertEquals(0.4, s[5], 0.01)
    }

    @Test
    fun theAccelerationIsPassedOnAsMeasured() {
        val s = SensorFrame().sample(0.0, 1.5, -2.5, 9.0)
        assertEquals(1.5, s[0], 0.0)
        assertEquals(-2.5, s[1], 0.0)
        assertEquals(9.0, s[2], 0.0)
        assertEquals(0.0, s[3], 0.0)
        assertEquals(0.0, s[4], 0.0)
        assertEquals(0.0, s[8], 0.0)
    }
}
