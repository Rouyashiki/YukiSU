package com.anatdx.yukisu.ui.webui

import android.content.ContentValues
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import android.util.Base64
import android.util.Log
import android.webkit.JavascriptInterface
import android.webkit.MimeTypeMap
import android.widget.Toast
import androidx.core.net.toUri
import com.anatdx.yukisu.R
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import java.io.ByteArrayInputStream
import java.io.InputStream
import java.util.Locale

class WebUIDownloadInterface(private val state: WebUIState) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val blobs = BlobDownloadHandler()
    private var closed = false

    @JavascriptInterface
    fun openExternal(url: String) {
        val view = state.webView ?: return
        val uri = url.toUri()
        if (uri.scheme != "http" && uri.scheme != "https") return
        view.post {
            runCatching { view.context.startActivity(Intent(Intent.ACTION_VIEW, uri)) }
                .onFailure { Log.e("WebUIDownload", "Failed to open $url", it) }
        }
    }

    @JavascriptInterface
    fun save(base64: String, fileName: String?) {
        enqueueSave(fileName, null, { ByteArrayInputStream(Base64.decode(base64, Base64.DEFAULT)) })
    }

    @JavascriptInterface
    @Synchronized
    fun startChunkedDownload(fileName: String?, mimeType: String?): String {
        if (closed) return ""
        val view = state.webView ?: return ""
        return runCatching {
            blobs.registerBlobDownload(sanitizeWebUIDownloadFileName(fileName), mimeType, view.context.cacheDir)
        }.onFailure { Log.e("WebUIDownload", "Failed to start chunked download", it) }.getOrDefault("")
    }

    @JavascriptInterface
    fun writeDownloadChunk(downloadId: String, base64Chunk: String): Boolean =
        blobs.writeChunk(downloadId, base64Chunk)

    @JavascriptInterface
    fun completeChunkedDownload(downloadId: String): Boolean {
        val download = blobs.completeDownload(downloadId) ?: return false
        return enqueueSave(download.fileName, download.mimeType, { download.file.inputStream() }) {
            download.file.delete()
        }
    }

    @JavascriptInterface
    fun cancelChunkedDownload(downloadId: String) = blobs.cancelDownload(downloadId)

    @Synchronized
    private fun enqueueSave(
        fileName: String?,
        requestedMimeType: String?,
        source: () -> InputStream,
        cleanup: () -> Unit = {},
    ): Boolean {
        val view = state.webView
        if (closed || view == null) {
            cleanup()
            return false
        }
        val context = view.context.applicationContext
        val displayName = sanitizeWebUIDownloadFileName(fileName)
        val mimeType = requestedMimeType?.takeIf { it.isNotBlank() }
            ?: MimeTypeMap.getSingleton().getMimeTypeFromExtension(displayName.substringAfterLast('.', "").lowercase(Locale.ROOT))
            ?: "application/octet-stream"
        scope.launch {
            var pendingUri: Uri? = null
            try {
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, displayName)
                    put(MediaStore.MediaColumns.MIME_TYPE, mimeType)
                    put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                }
                val uri = context.contentResolver.insert(
                    MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values
                ) ?: error("Failed to create download")
                pendingUri = uri
                source().use { input ->
                    val output = context.contentResolver.openOutputStream(uri) ?: error("Failed to open download")
                    writeWebUIDownload(input, output) { ensureActive() }
                }
                ensureActive()
                val published = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
                check(context.contentResolver.update(uri, published, null, null) == 1) { "Failed to publish download" }
                pendingUri = null
                view.post {
                    Toast.makeText(context, context.getString(R.string.webui_download_saved, displayName), Toast.LENGTH_LONG).show()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.e("WebUIDownload", "Failed to save $displayName", error)
                view.post {
                    Toast.makeText(context, context.getString(R.string.webui_download_failed, displayName), Toast.LENGTH_LONG).show()
                }
            } finally {
                pendingUri?.let { uri -> runCatching { context.contentResolver.delete(uri, null, null) } }
            }
        }.invokeOnCompletion { cleanup() }
        return true
    }

    @Synchronized
    fun destroy() {
        closed = true
        scope.cancel()
        blobs.cleanup()
    }
}
