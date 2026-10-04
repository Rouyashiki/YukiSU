package com.anatdx.yukisu.ui.util.module

import android.content.Context
import android.net.Uri
import org.apache.commons.compress.archivers.zip.ZipArchiveEntry
import org.apache.commons.compress.archivers.zip.ZipFile
import org.apache.commons.compress.archivers.zip.ZipMethod
import org.tukaani.xz.LZMAInputStream
import java.io.DataInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.CRC32

internal object ModuleArchive {
    fun <T> withZip(context: Context, uri: Uri, block: (ZipFile) -> T): T {
        val file = File.createTempFile("module-preview-", ".zip", context.cacheDir)
        try {
            val input = context.contentResolver.openInputStream(uri)
                ?: throw IOException("Unable to open module archive: $uri")
            input.use { source -> file.outputStream().use { source.copyTo(it) } }
            return ZipFile.builder().setFile(file).get().use(block)
        } finally {
            file.delete()
        }
    }

    fun readProperties(context: Context, uri: Uri): Map<String, String> =
        withZip(context, uri, ::readProperties)

    fun readProperties(zip: ZipFile): Map<String, String> {
        val entry = zip.getEntry("module.prop") ?: zip.entries.asSequence().firstOrNull {
            val name = it.name.replace('\\', '/').trimStart('/').lowercase()
            name == "module.prop" || name.endsWith("/module.prop")
        } ?: return emptyMap()
        if (entry.size !in 0..1024 * 1024) throw IOException("Module metadata is too large")
        val content = ByteArray(entry.size.toInt())
        openEntry(zip, entry).use { input ->
            DataInputStream(input).readFully(content)
            if (input.read() != -1) throw IOException("Invalid module metadata size")
        }
        if (CRC32().apply { update(content) }.value != entry.crc) {
            throw IOException("Invalid module metadata CRC")
        }
        return buildMap {
            content.toString(Charsets.UTF_8).lineSequence().forEach { line ->
                if ('=' in line && !line.trimStart().startsWith('#')) {
                    put(line.substringBefore('=').trim(), line.substringAfter('=').trim())
                }
            }
        }
    }

    private fun openEntry(zip: ZipFile, entry: ZipArchiveEntry): InputStream {
        if (entry.method != ZipMethod.LZMA.code) return zip.getInputStream(entry)
        if (entry.generalPurposeBit.usesEncryption()) throw IOException("Encrypted module metadata")
        val input = DataInputStream(zip.getRawInputStream(entry))
        try {
            input.readUnsignedShort() // LZMA SDK version.
            val propertiesSize = input.readUnsignedByte() or (input.readUnsignedByte() shl 8)
            if (propertiesSize != 5) throw IOException("Invalid ZIP LZMA properties")
            val properties = input.readByte()
            val dictionarySize = Integer.reverseBytes(input.readInt())
            if (dictionarySize !in 0..64 * 1024 * 1024) {
                throw IOException("ZIP LZMA dictionary is too large")
            }
            val size = if (entry.rawFlag and 2 != 0) -1L else entry.size
            return LZMAInputStream(input, size, properties, dictionarySize)
        } catch (error: Throwable) {
            input.close()
            throw error
        }
    }
}
