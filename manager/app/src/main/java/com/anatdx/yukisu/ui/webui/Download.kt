package com.anatdx.yukisu.ui.webui

import java.io.InputStream
import java.io.OutputStream

internal fun sanitizeWebUIDownloadFileName(requestedFileName: String?): String {
    val candidate = requestedFileName.orEmpty()
        .substringAfterLast('/')
        .substringAfterLast('\\')
        .filterNot { it.isISOControl() }
        .trim()
    return candidate.takeUnless { it.isEmpty() || it == "." || it == ".." } ?: "download.bin"
}

internal fun writeWebUIDownload(
    source: InputStream,
    output: OutputStream,
    onBytesWritten: (Long) -> Unit = {},
): Long = output.buffered(64 * 1024).use { bufferedOutput ->
    val buffer = ByteArray(8 * 1024)
    var total = 0L
    while (true) {
        val count = source.read(buffer)
        if (count == -1) break
        bufferedOutput.write(buffer, 0, count)
        total += count
        onBytesWritten(total)
    }
    total
}
