package com.anatdx.yukisu.ui.webui

import android.util.Base64
import android.util.Log
import java.io.File
import java.io.OutputStream
import java.util.UUID

internal class BlobDownloadHandler {
    internal data class CompletedDownload(val fileName: String, val mimeType: String?, val file: File)

    private data class PendingDownload(
        val download: CompletedDownload,
        val output: OutputStream,
        val createdAt: Long = System.nanoTime(),
    )

    private val pendingDownloads = mutableMapOf<String, PendingDownload>()
    private var closed = false

    @Synchronized
    fun registerBlobDownload(fileName: String, mimeType: String?, cacheDir: File): String {
        check(!closed) { "WebUI download session is closed" }
        val staleIds = pendingDownloads.filterValues {
            System.nanoTime() - it.createdAt > 5L * 60 * 1_000_000_000
        }.keys.toList()
        staleIds.forEach(::cancelDownload)
        val file = File.createTempFile("ksu-blob-", ".download", cacheDir)
        val output = try {
            file.outputStream().buffered(64 * 1024)
        } catch (error: Exception) {
            file.delete()
            throw error
        }
        val id = UUID.randomUUID().toString()
        pendingDownloads[id] = PendingDownload(CompletedDownload(fileName, mimeType, file), output)
        return id
    }

    @Synchronized
    fun writeChunk(id: String, base64Chunk: String): Boolean {
        val download = pendingDownloads[id] ?: return false
        return try {
            download.output.write(Base64.decode(base64Chunk, Base64.DEFAULT))
            true
        } catch (error: Exception) {
            Log.e("BlobDownloadHandler", "Failed to write download chunk", error)
            cancelDownload(id)
            false
        }
    }

    @Synchronized
    fun completeDownload(id: String): CompletedDownload? {
        val download = pendingDownloads[id] ?: return null
        return try {
            download.output.close()
            pendingDownloads.remove(id)
            download.download
        } catch (error: Exception) {
            Log.e("BlobDownloadHandler", "Failed to finish download", error)
            cancelDownload(id)
            null
        }
    }

    @Synchronized
    fun cancelDownload(id: String) {
        pendingDownloads.remove(id)?.let {
            runCatching { it.output.close() }
            it.download.file.delete()
        }
    }

    @Synchronized
    fun cleanup() {
        closed = true
        pendingDownloads.keys.toList().forEach(::cancelDownload)
    }
}
