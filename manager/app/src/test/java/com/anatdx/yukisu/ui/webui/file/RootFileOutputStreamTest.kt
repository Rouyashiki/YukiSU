package com.anatdx.yukisu.ui.webui.file

import android.app.Application
import android.util.Base64
import com.anatdx.yukisu.ui.util.shellArg
import com.topjohnwu.superuser.Shell
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class RootFileOutputStreamTest {
    @Test
    fun binaryWritesAreBoundedAndCompleteBeforeCloseReturns() {
        val output = ByteArrayOutputStream()
        var chunks = 0
        val shell = RecordingShell { command ->
            if (command.startsWith("printf")) {
                val encoded = command.substringAfter("printf '%s' '").substringBefore("' |")
                val bytes = Base64.decode(encoded, Base64.NO_WRAP)
                assertTrue(bytes.size <= 64 * 1024)
                output.write(bytes)
                chunks++
            }
            0
        }
        val io = KsuIO { shell }
        val stream = io.FileOutputStream()
        val id = stream.open("/data/adb/binary")
        val bytes = ByteArray(128 * 1024 + 7) { it.toByte() }
        assertTrue(id.isNotEmpty())
        assertTrue(stream.write(id, Base64.encodeToString(bytes, Base64.NO_WRAP)))
        assertTrue(stream.writeByte(id, 255))
        assertTrue(stream.close(id))
        assertContentEquals(bytes + byteArrayOf(-1), output.toByteArray())
        assertEquals(4, chunks)
        assertEquals("exec 3>&-", shell.commands.last())
        assertEquals(1, shell.closeCount)
        io.destroy()
        assertEquals(1, shell.closeCount)
    }

    @Test
    fun closeWaitsForTheBufferedWriteResult() {
        val started = CountDownLatch(1)
        val complete = CountDownLatch(1)
        val shell = RecordingShell { command ->
            if (command.startsWith("printf")) {
                started.countDown()
                check(complete.await(5, TimeUnit.SECONDS))
            }
            0
        }
        val io = KsuIO { shell }
        val stream = io.FileOutputStream()
        val id = stream.open("/data/adb/config")
        assertTrue(stream.writeByte(id, 42))
        val executor = Executors.newSingleThreadExecutor()
        val close = executor.submit<Boolean> { stream.close(id) }
        try {
            assertTrue(started.await(5, TimeUnit.SECONDS))
            assertFalse(close.isDone)
            assertEquals(0, shell.closeCount)
            complete.countDown()
            assertTrue(close.get(5, TimeUnit.SECONDS))
            assertEquals(1, shell.closeCount)
        } finally {
            complete.countDown()
            executor.shutdownNow()
            executor.awaitTermination(5, TimeUnit.SECONDS)
            io.destroy()
        }
    }

    @Test
    fun failedWritesStayFailedAndCloseReleasesTheShell() {
        var writes = 0
        val shell = RecordingShell { command ->
            if (command.startsWith("printf")) {
                writes++
                1
            } else {
                0
            }
        }
        val io = KsuIO { shell }
        val stream = io.FileOutputStream()
        val id = stream.open("/data/adb/config")
        assertTrue(stream.writeByte(id, 42))
        assertFalse(stream.flush(id))
        assertFalse(stream.close(id))
        assertEquals(1, writes)
        assertEquals(1, shell.closeCount)
        assertTrue(io.openOutputStreams.isEmpty())
        io.destroy()
    }

    @Test
    fun partialWriteFailureStopsLaterChunksAndIsNotRetriedOnClose() {
        var writes = 0
        val shell = RecordingShell { command ->
            if (command.startsWith("printf") && ++writes == 2) 1 else 0
        }
        val io = KsuIO { shell }
        val stream = io.FileOutputStream()
        val id = stream.open("/data/adb/large")
        val bytes = Base64.encodeToString(ByteArray(192 * 1024), Base64.NO_WRAP)
        assertFalse(stream.write(id, bytes))
        assertFalse(stream.close(id))
        assertEquals(2, writes)
        assertEquals(1, shell.closeCount)
        io.destroy()
    }

    @Test
    fun openFailureClosesTheShellAndDoesNotPublishAHandle() {
        val shell = RecordingShell { 1 }
        val io = KsuIO { shell }
        assertEquals("", io.FileOutputStream().open("/missing/file"))
        assertEquals(1, shell.closeCount)
        assertTrue(io.openOutputStreams.isEmpty())
        io.destroy()
    }

    @Test
    fun outputHandlesOwnSeparateShellsAndDisposeClosesRemainingHandles() {
        val shells = mutableListOf<RecordingShell>()
        val io = KsuIO { RecordingShell().also { shells += it } }
        val stream = io.FileOutputStream()
        val first = io.File("/data/adb/first").newOutputStream()
        val second = stream.open("/data/adb/second")
        assertTrue(stream.close(first))
        assertEquals(1, shells[0].closeCount)
        assertEquals(0, shells[1].closeCount)
        assertTrue(stream.writeByte(second, 42))
        io.destroy()
        assertEquals(1, shells[1].closeCount)
        assertEquals("", stream.open("/data/adb/late"))
        assertEquals(2, shells.size)
    }

    @Test
    fun appendModeAndLiteralPathsArePreserved() {
        val shell = RecordingShell()
        val path = "/data/adb/space ' quote \$(command) `command`"
        val target = shellArg(File(path).absolutePath)
        RootFileOutputStream(shell, path, true).use { }
        assertEquals(
            "if [ -b $target ] || [ -c $target ]; then exec 3>$target; else exec 3>>$target; fi",
            shell.commands.first(),
        )
    }

    private class RecordingShell(private val execute: (String) -> Int = { 0 }) : Shell() {
        val commands = mutableListOf<String>()
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
        override fun newJob(): Job = object : Job() {
            private lateinit var command: String
            override fun add(vararg commands: String): Job = error("File contents must not enter command logs")
            override fun add(input: InputStream): Job = apply {
                command = input.bufferedReader().use { it.readText() }
            }
            override fun to(stdout: MutableList<String>?): Job = this
            override fun to(stdout: MutableList<String>?, stderr: MutableList<String>?): Job = this
            override fun exec(): Result {
                commands += command
                val code = execute(command)
                return object : Result() {
                    override fun getCode(): Int = code
                    override fun getOut(): MutableList<String> = mutableListOf()
                    override fun getErr(): MutableList<String> = mutableListOf()
                }
            }
            override fun submit(executor: Executor?, callback: ResultCallback?) = error("Unexpected asynchronous write")
            override fun enqueue(): Future<Result> = error("Unexpected asynchronous write")
        }
    }
}
