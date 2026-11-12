package com.homenet.espgw.data

import org.junit.Assert.*
import org.junit.Test

class RgbTest {
    @Test fun roundTrip() {
        val value = Rgb.pack(true, 65, Rgb.WHITE)
        assertTrue(Rgb.on(value))
        assertEquals(65, Rgb.brightness(value))
        assertEquals(Rgb.WHITE, Rgb.color(value))
        assertFalse(Rgb.on(Rgb.pack(false, 65, Rgb.WHITE)))
    }

    @Test fun clampsInvalidValues() {
        assertEquals(1, Rgb.brightness(Rgb.pack(true, -5, 300)))
        assertEquals(44, Rgb.color(Rgb.pack(true, 1, 300)))
        assertEquals(100, Rgb.brightness(Rgb.pack(true, 999, 0)))
        assertEquals(100, Rgb.brightness(0))
    }
}
