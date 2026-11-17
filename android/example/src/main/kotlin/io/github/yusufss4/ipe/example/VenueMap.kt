package io.github.yusufss4.ipe.example

import org.json.JSONArray
import org.json.JSONObject

/** What the app draws of a venue file (docs/formats.md): the walls of every floor, the stairs and lifts. */
class VenueMap(
    val name: String,
    /** min x, min y, max x, max y [m]. */
    val bounds: DoubleArray,
    /** Sorted by ordinal: a level's position here is its level index in the engine. */
    val levels: List<Level>,
    val connectors: List<Connector>,
    /** Angle of the map's +x counter-clockwise from east [degrees]; 0 without a georef. */
    val rotationDeg: Double,
    /** Latitude and longitude of the map's origin, or null without a georef. */
    val origin: Pair<Double, Double>?,
    /** Transmitters the venue lists: 0 for a venue that is only a map. */
    val transmitters: Int,
) {
    class Level(val ordinal: Int, val name: String, /** x1, y1, x2, y2 per wall. */ val walls: FloatArray)

    class Connector(/** Level indices. */ val levels: IntArray, /** x, y per corner. */ val area: FloatArray)

    companion object {
        fun parse(json: String): VenueMap {
            val v = JSONObject(json)
            val b = v.getJSONArray("bounds")
            val bounds = DoubleArray(4) { b.getDouble(it) }
            require(bounds[2] > bounds[0] && bounds[3] > bounds[1]) { "bounds must be [min x, min y, max x, max y]" }

            val levels = ArrayList<Level>()
            val list = v.optJSONArray("levels")
            if (list == null) {
                levels.add(Level(0, "", walls(v.optJSONArray("walls"))))
            } else {
                for (i in 0 until list.length()) {
                    val l = list.getJSONObject(i)
                    val ordinal = l.getInt("ordinal")
                    levels.add(Level(ordinal, l.optString("name", "L$ordinal"), walls(l.optJSONArray("walls"))))
                }
                levels.sortBy { it.ordinal }
            }
            val indexOf = levels.withIndex().associate { (i, l) -> l.ordinal to i }

            val connectors = ArrayList<Connector>()
            val cs = v.optJSONArray("connectors")
            for (i in 0 until (cs?.length() ?: 0)) {
                val c = cs!!.getJSONObject(i)
                val on = c.getJSONArray("levels")
                val area = c.getJSONArray("area")
                connectors.add(
                    Connector(
                        IntArray(on.length()) { indexOf[on.getInt(it)] ?: -1 },
                        FloatArray(area.length() * 2) { area.getJSONArray(it / 2).getDouble(it % 2).toFloat() },
                    ),
                )
            }
            val georef = v.optJSONObject("georef")
            return VenueMap(
                v.optString("name", "venue"), bounds, levels, connectors,
                georef?.optDouble("rotation_deg", 0.0) ?: 0.0,
                georef?.let { if (it.has("origin_lat") && it.has("origin_lon")) it.getDouble("origin_lat") to it.getDouble("origin_lon") else null },
                v.optJSONArray("beacons")?.length() ?: 0,
            )
        }

        private fun walls(a: JSONArray?): FloatArray {
            if (a == null) return FloatArray(0)
            val out = FloatArray(a.length() * 4)
            for (i in 0 until a.length()) {
                val w = a.getJSONArray(i)
                for (k in 0 until 4) out[i * 4 + k] = w.getDouble(k).toFloat()
            }
            return out
        }
    }
}
