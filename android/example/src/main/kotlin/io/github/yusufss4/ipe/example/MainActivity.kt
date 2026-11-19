package io.github.yusufss4.ipe.example

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The engine on this phone: choose a venue, press Start, tap the map where you are and walk.
 *
 * Every tap is kept in the recording as the true position. "Save walk" writes the recording as a trace
 * file: `ipe-replay` replays it, and `datasets/fit_radio_map.py` fits a venue's radio map from such walks.
 */
class MainActivity : Activity() {
    private lateinit var map: MapView
    private lateinit var status: TextView
    private lateinit var readout: TextView
    private lateinit var startButton: Button
    private lateinit var saveButton: Button
    private lateinit var floorLabel: TextView
    private lateinit var corrects: CheckBox
    private lateinit var venues: Spinner

    private val bundled = ArrayList<String>()  // asset names
    private var venueJson: String? = null
    private var venue: VenueMap? = null
    private var venueLabel = ""
    private var level = 0
    private var session: Session? = null
    private var running = false
    private var lastError = Double.NaN

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val dp = resources.displayMetrics.density
        val pad = (8 * dp).toInt()

        venues = Spinner(this)
        val open = Button(this).apply { setText(R.string.open_venue); setOnClickListener { openVenue() } }
        startButton = Button(this).apply { setText(R.string.start); setOnClickListener { if (running) stop() else askThenStart() } }
        saveButton = Button(this).apply { setText(R.string.save); setOnClickListener { save() } }
        val down = Button(this).apply { text = "−"; setOnClickListener { showLevel(level - 1) } }
        val up = Button(this).apply { text = "+"; setOnClickListener { showLevel(level + 1) } }
        floorLabel = TextView(this).apply { gravity = Gravity.CENTER; minWidth = (56 * dp).toInt() }
        corrects = CheckBox(this).apply { setText(R.string.taps_correct) }
        status = TextView(this).apply { setText(R.string.pick_venue); setPadding(pad, pad / 2, pad, 0) }
        readout = TextView(this).apply { setPadding(pad, 0, pad, pad / 2) }
        map = MapView(this).apply { onTap = { x, y -> session?.takeIf { running }?.tap(x, y, level, corrects.isChecked) } }

        fun row(vararg views: View) = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            for (v in views) addView(v, LinearLayout.LayoutParams(if (v === venues) 0 else WRAP_CONTENT, WRAP_CONTENT, if (v === venues) 1f else 0f))
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(row(venues, open))
            addView(row(startButton, saveButton, down, floorLabel, up))
            addView(corrects)
            addView(status)
            addView(readout)
            addView(map, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        }
        // Android draws the app under the status and navigation bars: keep the controls out of them.
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars())
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(root)

        bundled += assets.list("")?.filter { it.endsWith(".json") }?.sorted().orEmpty()
        venues.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, bundled.map { it.removeSuffix(".json") })
        venues.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                if (!running) load(assets.open(bundled[position]).bufferedReader().use { it.readText() }, bundled[position].removeSuffix(".json"))
            }

            override fun onNothingSelected(parent: AdapterView<*>) = Unit
        }
        venues.setSelection(bundled.indexOf("office_floor.json").coerceAtLeast(0))
    }

    override fun onDestroy() {
        session?.close()
        super.onDestroy()
    }

    // ---- Venue --------------------------------------------------------------------------------------

    private fun load(json: String, label: String): Boolean {
        val parsed = try {
            VenueMap.parse(json)
        } catch (e: Exception) {
            Toast.makeText(this, getString(R.string.bad_venue, e.message), Toast.LENGTH_LONG).show()
            return false
        }
        session?.close()
        session = null
        venueJson = json
        venue = parsed
        venueLabel = label
        map.show(parsed)
        showLevel(0)
        status.setText(R.string.pick_venue)
        readout.text = "${parsed.name}: ${parsed.levels.size} floor(s), ${parsed.transmitters} transmitters"
        return true
    }

    private fun openVenue() {
        if (running) return
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), OPEN_VENUE)
    }

    private fun showLevel(index: Int) {
        val v = venue ?: return
        level = index.coerceIn(0, v.levels.size - 1)
        floorLabel.text = v.levels[level].name.ifEmpty { "floor ${v.levels[level].ordinal}" }
        map.showLevel(level)
    }

    // ---- Walk ---------------------------------------------------------------------------------------

    private fun askThenStart() {
        val wanted = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.NEARBY_WIFI_DEVICES)
            if (Build.VERSION.SDK_INT >= 31) add(Manifest.permission.BLUETOOTH_SCAN)
        }.filter { checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED }
        if (wanted.isEmpty()) start() else requestPermissions(wanted.toTypedArray(), PERMISSIONS)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray) {
        // The walk starts either way: without the permissions it runs on the motion sensors alone.
        if (requestCode == PERMISSIONS) start()
    }

    private fun start() {
        val json = venueJson ?: return
        val v = venue ?: return
        session?.close()
        map.clearWalk()
        lastError = Double.NaN
        running = true
        startButton.setText(R.string.stop)
        venues.isEnabled = false
        status.setText(R.string.starting)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        session = Session(
            this, json, v,
            onError = { message ->
                stop()
                status.text = getString(R.string.bad_venue, message)
            },
            onSnapshot = ::show,
            onMark = { x, y, markLevel, setsPosition, error ->
                map.addMark(x, y, markLevel, setsPosition)
                if (error >= 0) lastError = error
            },
        ).also { it.start() }
    }

    private fun stop() {
        running = false
        session?.stop()
        startButton.setText(R.string.start)
        venues.isEnabled = true
        status.setText(R.string.stopped)
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun show(s: Snapshot) {
        map.update(s.estimate)
        if (running) {
            status.text = when {
                !s.sensorsRunning -> getString(R.string.waiting_for_sensors)
                s.throttled -> getString(R.string.throttled)
                !s.hasFix -> getString(R.string.tap_start)
                else -> getString(R.string.walk)
            }
        }
        val parts = ArrayList<String>()
        parts += String.format(Locale.ROOT, "%.0f s", s.seconds)
        parts += "${s.steps} steps"
        parts += if (s.scans == 0) "no Wi-Fi scan yet" else if (s.scanIntervalS.isNaN()) "${s.scans} Wi-Fi scan" else
            String.format(Locale.ROOT, "%d Wi-Fi scans, last after %.1f s", s.scans, s.scanIntervalS)
        if (s.beaconPackets > 0) parts += "${s.beaconPackets} beacon packets"
        if (!lastError.isNaN()) parts += String.format(Locale.ROOT, "error at the last tap %.1f m", lastError)
        if (s.estimate.valid && s.estimate.level != level) parts += "the estimate is on another floor"
        readout.text = parts.joinToString(" · ")
    }

    // ---- Recording ----------------------------------------------------------------------------------

    private fun save() {
        if (session == null) {
            Toast.makeText(this, R.string.nothing_to_save, Toast.LENGTH_SHORT).show()
            return
        }
        val name = "ipe-$venueLabel-" + SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date()) + ".csv"
        startActivityForResult(
            Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/csv").putExtra(Intent.EXTRA_TITLE, name),
            SAVE_WALK,
        )
    }

    private fun write(uri: Uri) {
        session?.trace { csv ->
            val ok = csv != null && runCatching {
                contentResolver.openOutputStream(uri)!!.use { it.write(csv.toByteArray(Charsets.UTF_8)) }
            }.isSuccess
            runOnUiThread { Toast.makeText(this, if (ok) R.string.saved else R.string.nothing_to_save, Toast.LENGTH_LONG).show() }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) return
        when (requestCode) {
            OPEN_VENUE -> {
                val text = runCatching { contentResolver.openInputStream(uri)!!.bufferedReader().use { it.readText() } }.getOrNull()
                if (text == null || !load(text, uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')?.removeSuffix(".json") ?: "venue")) return
            }
            SAVE_WALK -> write(uri)
        }
    }

    private companion object {
        const val OPEN_VENUE = 1
        const val SAVE_WALK = 2
        const val PERMISSIONS = 3
    }
}
