package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import java.io.File
import java.io.IOException
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext

/** Separate from the ramdisk editor cache: completed exports outlive the editing session. */
internal class RamdiskExternalFiles(private val directory: File, private val clock: () -> Long = System::currentTimeMillis) {
    suspend fun prepare(entry: FileEntry, reader: RamdiskContentReader, onProgress: (FileByteProgress) -> Unit): File {
        var requestDirectory: File? = null
        try {
            return withContext(Dispatchers.IO) {
                require(entry.type == FileEntryType.REGULAR_FILE)
                cleanExpired()
                val targetDirectory = File(directory, UUID.randomUUID().toString()).also { requestDirectory = it }
                if (!targetDirectory.mkdirs()) throw IOException("Cannot create export directory")
                val incomplete = File(targetDirectory, ".partial")
                incomplete.outputStream().buffered(65536).use { output ->
                    reader.read(entry.id, onProgress) { bytes, count -> output.write(bytes, 0, count) }
                }
                currentCoroutineContext().ensureActive()
                val result = File(targetDirectory, safeName(entry.name))
                if (!incomplete.renameTo(result)) throw IOException("Cannot finish temporary export")
                // The directory timestamp is the retention marker, independent of the display name.
                if (!targetDirectory.setLastModified(clock())) throw IOException("Cannot retain temporary export")
                result
            }
        } catch (failure: Throwable) {
            // Includes cancellation while dispatching the completed file back to the caller.
            withContext(NonCancellable + Dispatchers.IO) { requestDirectory?.deleteRecursively() }
            throw failure
        }
    }

    fun discard(file: File) {
        val parent = file.parentFile ?: return
        if (parent.parentFile?.canonicalFile == directory.canonicalFile) parent.deleteRecursively()
    }

    fun cleanExpired() {
        val cutoff = clock() - RETENTION_MILLIS
        directory.listFiles()?.filter { it.isDirectory && it.lastModified() < cutoff }?.forEach { it.deleteRecursively() }
    }

    companion object {
        const val RETENTION_MILLIS = 24 * 60 * 60 * 1000L
        fun safeName(name: String): String {
            val result = name.map { if (it == '/' || it == '\\' || it.isISOControl()) '_' else it }.joinToString("").trim()
            return result.takeUnless { it.isBlank() || it == "." || it == ".." || it == ".partial" } ?: "file.bin"
        }
    }
}
