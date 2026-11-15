package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileContentChangedException
import java.io.*
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlinx.coroutines.*
import kotlin.test.*

/** Exercises the real framed client and response draining against an in-memory process. */
class YrcpSnapshotTest {
    @Test fun versionedReadUses64KiBChunksAndOneRevisionForAllChunks() = runBlocking<Unit> {
        val data = ByteArray(65553) { it.toByte() }
        val process = WireProcess(hello(true) + response(18, 2, longs(7, data.size.toLong())) +
            response(19, 3, data.copyOfRange(0, 65536)) + response(19, 4, data.copyOfRange(65536, data.size)))
        val session = YrcpSession.connect(process)
        try {
            val result = ByteArrayOutputStream()
            var progress = FileByteProgress()
            session.readSnapshot(42, { progress = it }) { buffer, count -> result.write(buffer, 0, count) }
            assertContentEquals(data, result.toByteArray())
            assertEquals(FileByteProgress(data.size.toLong(), data.size.toLong()), progress)
            val requests = process.frames()
            assertEquals(listOf(1, 18, 19, 19), requests.map { it.first })
            assertContentEquals(longs(42, 7, 0, 65536), requests[2].second)
            assertContentEquals(longs(42, 7, 65536, 17), requests[3].second)
        } finally { session.closeNow() }
    }

    @Test fun emptySnapshotStillChecksItsRevision() = runBlocking<Unit> {
        val process = WireProcess(hello(true) + response(18, 2, longs(3, 0)) + response(19, 3))
        val session = YrcpSession.connect(process)
        try {
            var consumed = false
            session.readSnapshot(42, {}) { _, _ -> consumed = true }
            assertFalse(consumed)
            assertContentEquals(longs(42, 3, 0, 0), process.frames().last().second)
        } finally { session.closeNow() }
    }

    @Test fun conflictDiscardsChecksumBatchAndNextRequestRemainsAligned() = runBlocking<Unit> {
        val process = WireProcess(hello(true) + response(18, 2, longs(3, 70000)) + response(19, 3, ByteArray(65536)) +
            response(19, 4, "conflict".encodeToByteArray(), status = 7) + response(14, 5))
        val session = YrcpSession.connect(process)
        try {
            val checksums = RamdiskFileChecksums { _, progress, consume -> session.readSnapshot(42, progress, consume) }
            assertFailsWith<FileContentChangedException> { checksums.calculate(com.anatdx.yukifb.model.EntryId("file"), defaultRamdiskChecksumIds) {} }
            session.dump()
            assertEquals(14, process.frames().last().first)
        } finally { session.closeNow() }
    }

    @Test fun legacyCancellationDrainsRemainingResponseBeforeReusingConnection() = runBlocking<Unit> {
        val process = WireProcess(hello(false) + response(4, 2, ByteArray(200000)) + response(14, 3))
        val session = YrcpSession.connect(process)
        try {
            val task = launch {
                val ownJob = currentCoroutineContext().job
                session.readSnapshot(42, {}) { _, _ -> ownJob.cancel() }
            }
            task.join()
            assertTrue(task.isCancelled)
            session.dump()
            assertEquals(listOf(1, 4, 14), process.frames().map { it.first })
        } finally { session.closeNow() }
    }

    @Test fun legacyResponseOwnsMutexUntilEntireReadCompletes() = runBlocking<Unit> {
        val process = WireProcess(hello(false) + response(4, 2, ByteArray(200000)) + response(14, 3))
        val session = YrcpSession.connect(process)
        val started = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        try {
            val read = launch { session.readWholeFile(42) { input, size ->
                assertEquals(200000L, size)
                input.read()
                started.complete(Unit)
                release.await()
            } }
            started.await()
            val dump = launch { session.dump() }
            yield()
            assertFalse(dump.isCompleted)
            assertEquals(listOf(1, 4), process.frames().map { it.first })
            release.complete(Unit)
            joinAll(read, dump)
        } finally { session.closeNow() }
    }

    @Test fun truncatedResponseInvalidatesConnection() = runBlocking<Unit> {
        val full = response(4, 2, ByteArray(200000))
        val process = WireProcess(hello(false) + full.copyOf(full.size - 100))
        val session = YrcpSession.connect(process)
        assertFailsWith<IOException> { session.readSnapshot(42, {}) { _, _ -> } }
        assertFalse(process.isAlive)
        assertFailsWith<IllegalStateException> { session.dump() }
    }

    private fun hello(binary: Boolean): ByteArray {
        val body = ByteBuffer.allocate(33).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(3).putLong(1).putLong(512L * 1024 * 1024).putLong(100000)
            .putInt(if (binary) 0x3ffff else 0xffff).put(0).array()
        return response(1, 1, body)
    }

    private fun longs(vararg values: Long) = ByteBuffer.allocate(values.size * 8).order(ByteOrder.LITTLE_ENDIAN).apply { values.forEach(::putLong) }.array()

    private fun response(opcode: Int, id: Int, body: ByteArray = byteArrayOf(), status: Int = 0): ByteArray =
        ByteBuffer.allocate(24 + body.size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("YRCP".encodeToByteArray()); putShort(3); putShort((opcode or 0x8000).toShort()); putInt(id)
            putLong(4L + body.size); putInt(status); put(body)
        }.array()

    private class WireProcess(response: ByteArray) : Process() {
        private val input = ByteArrayInputStream(response)
        private val output = ByteArrayOutputStream()
        private var alive = true
        override fun getInputStream(): InputStream = input
        override fun getOutputStream(): OutputStream = output
        override fun getErrorStream(): InputStream = ByteArrayInputStream(byteArrayOf())
        override fun isAlive() = alive
        override fun destroy() { alive = false }
        override fun exitValue() = 0
        override fun waitFor() = 0
        fun frames(): List<Pair<Int, ByteArray>> {
            val buffer = ByteBuffer.wrap(output.toByteArray()).order(ByteOrder.LITTLE_ENDIAN)
            return buildList {
                while (buffer.hasRemaining()) {
                    buffer.position(buffer.position() + 6)
                    val opcode = buffer.short.toInt()
                    buffer.int
                    val length = buffer.long.toInt()
                    val data = ByteArray(length).also { buffer.get(it) }
                    add(opcode to data)
                }
            }
        }
    }
}
