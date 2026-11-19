package io.github.yusufss4.ipe

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * A recorded mall walk of the repository (data/demo/msil/site1_F2) fed through the Kotlin API on a device.
 *
 * It checks that the engine runs and follows the walk. It also leaves two files in the test app's storage:
 * `saved.csv`, the recording the engine kept, and `shown.csv`, its estimate at every truth row.
 * `android/check_device.py` replays the first with the native `ipe-replay` and compares with the second:
 * the Android build and the native one must agree.
 */
@RunWith(AndroidJUnit4::class)
class ReplayTest {
    private val assets get() = InstrumentationRegistry.getInstrumentation().context.assets
    private val files get() = InstrumentationRegistry.getInstrumentation().targetContext.filesDir

    private fun asset(name: String) = assets.open(name).bufferedReader(Charsets.UTF_8).use { it.readText() }

    @Test
    fun aRecordedWalkReplaysOnTheDevice() {
        val trace = asset(TRACE)
        PositioningEngine(asset("venue.json")).use { engine ->
            assertEquals(1, engine.levelCount)
            val shown = StringBuilder("t,x,y,level,valid,error_m\n")
            val errors = ArrayList<Double>()
            var scan = 0
            val ids = ArrayList<String>()
            val rssi = ArrayList<Double>()
            var scanTime = 0.0
            fun flushScan() {
                if (ids.isNotEmpty()) engine.onScan(scanTime, ids.toTypedArray(), rssi.toDoubleArray())
                ids.clear()
                rssi.clear()
                scan = 0
            }
            for (line in trace.lineSequence()) {
                if (line.isEmpty() || line[0] == '#') continue
                val c = line.split(',')
                val t = c[1].toDouble()
                val inScan = c[0] == "ble" && c.size > 4
                if (!inScan || c[4].toInt() != scan) flushScan()
                when (c[0]) {
                    "imu" -> engine.onImu(t, c[2].toDouble(), c[3].toDouble(), c[4].toDouble(), c[5].toDouble(), c[6].toDouble(),
                        c[7].toDouble(), c[8].toDouble(), c[9].toDouble(), c[10].toDouble())
                    "ble" -> if (inScan) {
                        scan = c[4].toInt()
                        scanTime = t
                        ids.add(c[2])
                        rssi.add(c[3].toDouble())
                    } else {
                        engine.onBle(t, c[2], c[3].toDouble())
                    }
                    "baro" -> engine.onPressure(t, c[2].toDouble())
                    "fix" -> engine.onPositionFix(t, c[2].toDouble(), c[3].toDouble(), c[4].toInt(), c[5].toDouble())
                    "gt" -> {
                        val x = c[2].toDouble()
                        val y = c[3].toDouble()
                        engine.mark(t, x, y, 0)
                        val e = engine.estimate()
                        if (e.valid) errors.add(hypot(e.x - x, e.y - y))
                        shown.append("${engine.time},${e.x},${e.y},${e.level},${if (e.valid) 1 else 0},${hypot(e.x - x, e.y - y)}\n")
                    }
                }
            }
            flushScan()

            File(files, "saved.csv").writeText(engine.traceCsv())
            File(files, "shown.csv").writeText(shown.toString())

            // On the host this walk replays with a mean error of 3.2 m (docs/datasets.md).
            assertTrue("marks with an estimate: ${errors.size}", errors.size >= 15)
            assertTrue("steps: ${engine.steps}", engine.steps in 150..400)
            assertTrue("mean error ${errors.average()} m", errors.average() < 6.0)
            assertTrue(engine.traceCsv().startsWith("# ipe-trace v1"))
        }
    }

    @Test
    fun aVenueThatDoesNotParseIsAnIllegalArgument() {
        try {
            PositioningEngine("{\"name\": \"no bounds\"}")
            throw AssertionError("a venue without bounds was accepted")
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!.isNotEmpty())
        }
        try {
            PositioningEngine(asset("venue.json"), "{\"filter\": {\"no_such_setting\": 1}}")
            throw AssertionError("an unknown setting was accepted")
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!.contains("no_such_setting"))
        }
    }

    private companion object {
        const val TRACE = "5dda5afcc5b77e0006b17719.csv"
    }
}
