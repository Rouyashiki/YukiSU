package org.rouyashiki.yukisu.ui.util

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

internal const val MAX_PLUGIN_PACKAGE_BYTES = 272L * 1024L * 1024L

internal fun InputStream.copyPluginPackageTo(output: OutputStream) {
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var copied = 0L
    while (true) {
        val read = read(buffer)
        if (read < 0) return
        if (copied + read > MAX_PLUGIN_PACKAGE_BYTES) {
            throw IOException("Plugin package exceeds the size limit")
        }
        output.write(buffer, 0, read)
        copied += read
    }
}
