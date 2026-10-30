package com.anatdx.yukisu.ui.webui.file

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import kotlin.test.*

class KsuIOTest {
    @Test fun fileStringConversionIsExposedToJavaScript() {
        assertNotNull(FileInterface::class.java.getMethod("toString")
            .getAnnotation(android.webkit.JavascriptInterface::class.java))
    }

    @Test fun disposeOnlyClosesStreamsOwnedByTheSession() {
        val first = KsuIO()
        val second = KsuIO()
        var firstClosed = false
        var secondClosed = false
        first.openOutputStreams["first"] = object : ByteArrayOutputStream() {
            override fun close() { firstClosed = true }
        }
        second.openOutputStreams["second"] = object : ByteArrayOutputStream() {
            override fun close() { secondClosed = true }
        }
        first.destroy()
        assertTrue(firstClosed)
        assertFalse(secondClosed)
        assertFalse(first.canOpenStream())
        assertTrue(second.canOpenStream())
        second.destroy()
        assertTrue(secondClosed)
    }

    @Test fun streamQuotaCountsBothDirections() {
        val io = KsuIO()
        repeat(25) { io.openInputStreams["$it"] = ManagedInputStream(ByteArrayInputStream(byteArrayOf())) }
        repeat(25) { io.openOutputStreams["$it"] = ByteArrayOutputStream() }
        assertFalse(io.canOpenStream())
        io.destroy()
        assertTrue(io.openInputStreams.isEmpty())
        assertTrue(io.openOutputStreams.isEmpty())
    }

    @Test fun inputReadsHonorChunkBoundariesAndEndOfFile() {
        val bytes = ByteArray(70000) { it.toByte() }
        ManagedInputStream(ByteArrayInputStream(bytes)).use { input ->
            assertEquals(65536, input.readInto(Int.MAX_VALUE))
            assertContentEquals(bytes.copyOf(65536), input.buffer().copyOf(65536))
            assertEquals(4464, input.readInto())
            assertEquals(-1, input.readInto())
        }
    }
}
