package io.github.yusufss4.ipe.example

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The id an iBeacon gets in a recording: what a venue's beacon list and a fitted radio map must use. */
class BeaconIdTest {
    private fun bytes(hex: String) = ByteArray(hex.length / 2) { hex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test
    fun anIBeaconIsItsUuidMajorAndMinor() {
        // Apple's manufacturer data: type 02, length 15, 16 bytes of UUID, major, minor, power at 1 m.
        val data = bytes("0215" + "9195B3ADA9D0450085FF9FB0F65A5201" + "0102" + "FFFE" + "C5")
        assertEquals("b:9195B3AD-A9D0-4500-85FF-9FB0F65A5201_258_65534", Session.iBeaconId(data))
    }

    @Test
    fun otherAdvertisementsAreLeftOut() {
        assertNull(Session.iBeaconId(null))
        assertNull(Session.iBeaconId(bytes("1005031C6F2A3B")))        // another Apple packet
        assertNull(Session.iBeaconId(bytes("0215" + "9195B3AD")))      // cut short
    }
}
