package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.backend.*
import com.anatdx.yukifb.model.*
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.Adler32
import java.util.zip.CRC32
import kotlinx.coroutines.*
import kotlin.test.*

class RamdiskFileActionsTest {
    private val entry = FileEntry(EntryId("id"), null, "init.rc", FileEntryType.REGULAR_FILE, size = 3)

    @Test fun allEightAlgorithmsShareOneReadAndMatchAbcVectors() = runBlocking<Unit> {
        var reads = 0
        val capability = RamdiskFileChecksums { id, _, consume ->
            assertEquals(entry.id, id)
            reads++
            consume("abc".encodeToByteArray(), 3)
        }
        val actual = capability.calculate(entry.id, capability.algorithms(entry).map { it.id }.toSet()) {}.associate { it.algorithmId to it.hex }
        assertEquals(1, reads)
        assertEquals(mapOf(
            "MD5" to "900150983CD24FB0D6963F7D28E17F72",
            "SHA-1" to "A9993E364706816ABA3E25717850C26C9CD0D89D",
            "SHA-224" to "23097D223405D8228642A477BDA255B32AADBCE4BDA0B3F7E36C9DA7",
            "SHA-256" to "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
            "SHA-384" to "CB00753F45A35E8BB5A03D699AC65007272C32AB0EDED1631A8B605A43FF5BED8086072BA1E7CC2358BAECA134C825A7",
            "SHA-512" to "DDAF35A193617ABACC417349AE20413112E6FA4E89A97EA20A9EEEE64B55D39A2192992A274FC1A836BA3C23A3FEEBBD454D4423643CE80E2A9AC94FA54CA49F",
            "CRC32" to "352441C2", "Adler32" to "024D0127",
        ), actual)
    }

    @Test fun emptyFileRetainsLeadingZerosAndKnownDigests() = runBlocking<Unit> {
        val capability = RamdiskFileChecksums { _, _, _ -> }
        val actual = capability.calculate(entry.id, setOf("MD5", "CRC32", "Adler32")) {}.associate { it.algorithmId to it.hex }
        assertEquals(mapOf("MD5" to "D41D8CD98F00B204E9800998ECF8427E", "CRC32" to "00000000", "Adler32" to "00000001"), actual)
    }

    @Test fun binaryDataAcrossBlocksUpdatesOnlyValidBufferBytes() = runBlocking<Unit> {
        val bytes = ByteArray(65536 * 2 + 17) { (it * 31).toByte() }
        val capability = RamdiskFileChecksums { _, progress, consume ->
            val buffer = ByteArray(65536)
            var offset = 0
            while (offset < bytes.size) {
                val count = minOf(buffer.size, bytes.size - offset)
                bytes.copyInto(buffer, 0, offset, offset + count)
                consume(buffer, count)
                offset += count
                progress(FileByteProgress(offset.toLong(), bytes.size.toLong()))
            }
        }
        var lastProgress = FileByteProgress()
        val result = capability.calculate(entry.id, setOf("SHA-256", "CRC32", "Adler32")) { lastProgress = it }.associate { it.algorithmId to it.hex }
        assertEquals(MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02X".format(it) }, result["SHA-256"])
        assertEquals(CRC32().apply { update(bytes) }.value.toString(16).uppercase().padStart(8, '0'), result["CRC32"])
        assertEquals(Adler32().apply { update(bytes) }.value.toString(16).uppercase().padStart(8, '0'), result["Adler32"])
        assertEquals(FileByteProgress(bytes.size.toLong(), bytes.size.toLong()), lastProgress)
    }

    @Test fun readFailureAndRevisionConflictCannotReturnPartialBatch() = runBlocking<Unit> {
        for (failure in listOf(IOException("read failed"), FileContentChangedException())) {
            val capability = RamdiskFileChecksums { _, _, consume -> consume(byteArrayOf(1), 1); throw failure }
            val actual = assertFails { capability.calculate(entry.id, defaultRamdiskChecksumIds) {} }
            assertEquals(failure::class, actual::class)
            assertEquals(failure.message, actual.message)
        }
    }

    @Test fun cancellingCalculationPropagatesToReader() = runBlocking<Unit> {
        val started = CompletableDeferred<Unit>()
        val cleaned = CompletableDeferred<Unit>()
        val capability = RamdiskFileChecksums { _, _, _ ->
            started.complete(Unit)
            try { awaitCancellation() } finally { cleaned.complete(Unit) }
        }
        val task = async { capability.calculate(entry.id, defaultRamdiskChecksumIds) {} }
        started.await()
        task.cancelAndJoin()
        assertTrue(cleaned.isCompleted)
    }

    @Test fun menuFiltersEachCapabilityAndFileTypeWithoutFollowingSymlinks() {
        val checksums = RamdiskFileChecksums { _, _, _ -> }
        val sharing = object : FileShareCapability {
            override fun canShare(entry: FileEntry) = true
            override suspend fun share(entryId: EntryId, onProgress: (FileByteProgress) -> Unit) = Unit
        }
        val external = object : FileExternalOpenCapability {
            override fun canOpenExternally(entry: FileEntry) = true
            override suspend fun openExternally(entryId: EntryId, onProgress: (FileByteProgress) -> Unit) = Unit
        }
        assertEquals(listOf(RamdiskFileAction.TEXT), ramdiskFileActions(entry, false, null, null, null))
        assertEquals(RamdiskFileAction.entries, ramdiskFileActions(entry.copy(mimeType = ELF_MIME_TYPE), true, checksums, sharing, external))
        assertEquals(listOf(RamdiskFileAction.CHECKSUMS), ramdiskFileActions(entry.copy(size = Long.MAX_VALUE), false, checksums, null, null))
        for (type in FileEntryType.entries.filter { it != FileEntryType.REGULAR_FILE }) {
            assertTrue(ramdiskFileActions(entry.copy(type = type), true, checksums, sharing, external).isEmpty())
            assertTrue(checksums.algorithms(entry.copy(type = type)).isEmpty())
        }
    }
}
