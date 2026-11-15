package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.backend.FileChecksumCapability
import com.anatdx.yukifb.model.EntryId
import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileChecksumAlgorithm
import com.anatdx.yukifb.model.FileChecksumValue
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.Adler32
import java.util.zip.CRC32
import java.util.zip.Checksum
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext

internal fun interface RamdiskContentReader {
    suspend fun read(entryId: EntryId, onProgress: (FileByteProgress) -> Unit, consume: (ByteArray, Int) -> Unit)
}

internal val defaultRamdiskChecksumIds = setOf("MD5", "SHA-1", "SHA-256", "CRC32")

internal class RamdiskFileChecksums(private val reader: RamdiskContentReader) : FileChecksumCapability {
    override fun algorithms(entry: FileEntry): List<FileChecksumAlgorithm> =
        if (entry.type == FileEntryType.REGULAR_FILE) algorithms else emptyList()

    override suspend fun calculate(entryId: EntryId, algorithmIds: Set<String>, onProgress: (FileByteProgress) -> Unit): List<FileChecksumValue> = withContext(Dispatchers.Default) {
        require(algorithmIds.isNotEmpty() && algorithms.map { it.id }.containsAll(algorithmIds))
        val digests = algorithmIds.filterNot { it == "CRC32" || it == "Adler32" }.associateWith { MessageDigest.getInstance(it) }
        val checksums = algorithmIds.mapNotNull { id -> when (id) {
            "CRC32" -> id to CRC32()
            "Adler32" -> id to Adler32()
            else -> null
        } }.toMap<String, Checksum>()
        reader.read(entryId, onProgress) { bytes, count ->
            digests.values.forEach { it.update(bytes, 0, count) }
            checksums.values.forEach { it.update(bytes, 0, count) }
        }
        currentCoroutineContext().ensureActive()
        algorithms.filter { it.id in algorithmIds }.map { algorithm ->
            val hex = digests[algorithm.id]?.digest()?.joinToString("") { "%02X".format(Locale.ROOT, it.toInt() and 255) }
                ?: checkNotNull(checksums[algorithm.id]).value.toString(16).uppercase(Locale.ROOT).padStart(8, '0')
            FileChecksumValue(algorithm.id, hex)
        }
    }

    private companion object {
        val algorithms = listOf(
            FileChecksumAlgorithm("MD5", "MD5", 32), FileChecksumAlgorithm("SHA-1", "SHA-1", 40),
            FileChecksumAlgorithm("SHA-224", "SHA-224", 56), FileChecksumAlgorithm("SHA-256", "SHA-256", 64),
            FileChecksumAlgorithm("SHA-384", "SHA-384", 96), FileChecksumAlgorithm("SHA-512", "SHA-512", 128),
            FileChecksumAlgorithm("CRC32", "CRC32", 8), FileChecksumAlgorithm("Adler32", "Adler32", 8),
        )
    }
}

internal class RamdiskProgressReporter(private val report: (FileByteProgress) -> Unit) {
    private var lastReport = 0L
    fun update(completed: Long, total: Long) {
        val now = System.nanoTime()
        if (completed == 0L || completed == total || now - lastReport >= 50_000_000L) {
            lastReport = now
            report(FileByteProgress(completed, total))
        }
    }
}
