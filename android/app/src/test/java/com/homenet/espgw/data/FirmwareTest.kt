package com.homenet.espgw.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class FirmwareTest {
    private fun image(project: String = "esp_s3_gateway", version: String = "0.7.0") =
        ByteArray(256).also { bytes ->
            bytes[0] = 0xE9.toByte()
            ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(32, 0xABCD5432.toInt())
            project.toByteArray().copyInto(bytes, 80)
            version.toByteArray().copyInto(bytes, 48)
        }

    @Test fun validHeader() {
        assertEquals(FwImage("esp_s3_gateway", "0.7.0", 3157000),
            Firmware.parseHeader(image(), 3157000))
    }

    @Test fun rejectsTruncatedAndInvalidHeaders() {
        assertNull(Firmware.parseHeader(ByteArray(111), 111))
        assertNull(Firmware.parseHeader(image().also { it[0] = 0 }, 256))
        assertNull(Firmware.parseHeader(image().also { it[32] = 0 }, 256))
    }

    @Test fun comparesVersions() {
        assertTrue(Firmware.compareVersions("0.7.0", "0.6.2") > 0)
        assertTrue(Firmware.compareVersions("1.0.0", "1.0.1") < 0)
        assertEquals(0, Firmware.compareVersions("1.2", "1.2.0"))
        assertEquals(0, Firmware.compareVersions("broken", "0"))
    }
}
