package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.model.*
import java.io.File
import java.io.IOException
import java.nio.file.Files
import kotlinx.coroutines.*
import kotlin.test.*

class RamdiskExternalFilesTest {
    private val entry = FileEntry(EntryId("opaque"), null, "../init.rc", FileEntryType.REGULAR_FILE)
    private val bytes = ByteArray(140000) { it.toByte() }
    private val reader = RamdiskContentReader { _, _, consume ->
        bytes.asList().chunked(65536).forEach { consume(it.toByteArray(), it.size) }
    }

    @Test fun independentExportsRetainNamesAndBytesAcrossSessionCloseAndExpireAfter24Hours() = runBlocking<Unit> {
        val root = Files.createTempDirectory("ramdisk-exports").toFile()
        try {
            var now = 1_000_000_000L
            val exports = RamdiskExternalFiles(root) { now }
            val first = exports.prepare(entry, reader) {}
            val second = exports.prepare(entry, reader) {}
            assertNotEquals(first.parentFile, second.parentFile)
            assertEquals(".._init.rc", first.name)
            assertContentEquals(bytes, first.readBytes())
            // A later service instance represents returning after the editor has closed.
            now += RamdiskExternalFiles.RETENTION_MILLIS
            RamdiskExternalFiles(root) { now }.cleanExpired()
            assertTrue(first.isFile)
            assertTrue(second.isFile)
            now++
            exports.cleanExpired()
            assertTrue(root.listFiles()!!.isEmpty())
        } finally { root.deleteRecursively() }
    }

    @Test fun failedOrCancelledReadRemovesItsIncompleteCopy() = runBlocking<Unit> {
        val root = Files.createTempDirectory("ramdisk-exports").toFile()
        try {
            val exports = RamdiskExternalFiles(root)
            assertFailsWith<IOException> { exports.prepare(entry, { _, _, consume ->
                consume(byteArrayOf(1), 1)
                throw IOException("disk or transport failure")
            }) {} }
            assertTrue(root.listFiles()!!.isEmpty())
            val started = CompletableDeferred<Unit>()
            val task = launch { exports.prepare(entry, { _, _, consume ->
                consume(byteArrayOf(2), 1)
                started.complete(Unit)
                awaitCancellation()
            }) {} }
            started.await()
            task.cancelAndJoin()
            assertTrue(root.listFiles()!!.isEmpty())
        } finally { root.deleteRecursively() }
    }

    @Test fun cancellationAtCompletedReturnBoundaryAlsoDeletesTheCopy() = runBlocking<Unit> {
        val root = Files.createTempDirectory("ramdisk-exports").toFile()
        try {
            val task = Job()
            val exports = RamdiskExternalFiles(root) {
                // cleanExpired calls the clock first; the second call follows rename.
                if (root.walkTopDown().any { it.isFile && it.name != ".partial" }) task.cancel()
                System.currentTimeMillis()
            }
            withContext(task) { assertFailsWith<CancellationException> { exports.prepare(entry, reader) {} } }
        } catch (_: CancellationException) {
            assertTrue(root.listFiles()!!.isEmpty())
        } finally { root.deleteRecursively() }
    }

    @Test fun unsafeNamesAreSanitizedAndDiscardCannotRemoveOtherCaches() = runBlocking<Unit> {
        val root = Files.createTempDirectory("ramdisk-exports").toFile()
        try {
            val exports = RamdiskExternalFiles(File(root, "external"))
            val unrelated = File(root, "editor/file").apply { parentFile!!.mkdirs(); writeText("keep") }
            exports.discard(unrelated)
            assertTrue(unrelated.isFile)
            assertEquals("file.bin", RamdiskExternalFiles.safeName(".."))
            assertEquals("a_b_c_.bin", RamdiskExternalFiles.safeName("a/b\\c\u0000.bin"))
            val file = exports.prepare(entry, reader) {}
            exports.discard(file)
            assertFalse(file.exists())
            assertTrue(unrelated.isFile)
        } finally { root.deleteRecursively() }
    }
}
