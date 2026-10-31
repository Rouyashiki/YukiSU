package com.anatdx.yukisu.ui.webui.file

import com.topjohnwu.superuser.Shell
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.test.*

class KsuIOTest {
    @Test fun filesShareTheOwnedShellAndDisposeClosesItOnce() {
        val shell = FileShell()
        var creations = 0
        val io = KsuIO { creations++; shell }
        assertSame(shell, io.file("/data/adb/first").shell)
        assertSame(shell, io.file("/data/adb/second").shell)
        assertEquals(1, creations)
        io.destroy()
        io.destroy()
        assertEquals(1, shell.closeCount)
        assertFailsWith<IOException> { io.file("/data/adb/late") }
        assertEquals(1, creations)
    }

    @Test fun disposingAnUnusedSessionDoesNotCreateAShell() {
        val io = KsuIO { error("Unexpected shell creation") }
        io.destroy()
        assertFailsWith<IOException> { io.file("/data/adb/late") }
    }

    @Test fun failedShellCreationCanBeRetried() {
        val shell = FileShell()
        var attempts = 0
        val io = KsuIO {
            if (++attempts == 1) throw IOException("Shell unavailable")
            shell
        }
        assertFailsWith<IOException> { io.file("/data/adb/file") }
        assertSame(shell, io.file("/data/adb/file").shell)
        assertEquals(2, attempts)
        io.destroy()
    }

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

    private class FileShell : Shell() {
        var closeCount = 0
            private set

        override fun isAlive(): Boolean = closeCount == 0
        override fun getStatus(): Int = ROOT_SHELL
        override fun close() { closeCount++ }
        override fun waitAndClose(timeout: Long, unit: TimeUnit): Boolean {
            close()
            return true
        }
        override fun execTask(task: Task) = error("Unexpected shell task")
        override fun submitTask(task: Task) = error("Unexpected shell task")
        override fun newJob(): Job = error("Unexpected shell job")
    }
}
