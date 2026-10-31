package com.anatdx.yukisu.ui.webui.file

import android.util.Base64
import com.anatdx.yukisu.ui.util.shellArg
import com.topjohnwu.superuser.Shell
import java.io.File
import java.io.IOException
import java.io.OutputStream

internal class RootFileOutputStream(
    private val shell: Shell,
    path: String,
    append: Boolean,
) : OutputStream() {
    private var closed = false
    private var failure: IOException? = null

    init {
        try {
            if ('\u0000' in path) throw IOException("Invalid file path")
            val target = shellArg(File(path).absolutePath)
            val open = if (append) {
                "if [ -b $target ] || [ -c $target ]; then exec 3>$target; else exec 3>>$target; fi"
            } else {
                "exec 3>$target"
            }
            runCommand(open)
        } catch (error: Throwable) {
            runCatching { shell.close() }.onFailure { error.addSuppressed(it) }
            throw error
        }
    }

    override fun write(value: Int) = write(byteArrayOf(value.toByte()), 0, 1)

    override fun write(bytes: ByteArray, offset: Int, length: Int) {
        if (offset < 0 || length < 0 || offset > bytes.size - length) {
            throw IndexOutOfBoundsException()
        }
        if (closed) throw IOException("File stream is closed")
        failure?.let { throw IOException("File stream write failed", it) }
        var position = offset
        val end = offset + length
        while (position < end) {
            val count = minOf(64 * 1024, end - position)
            val data = Base64.encodeToString(bytes, position, count, Base64.NO_WRAP)
            // Keep the file open across writes and wait for each decoder's exit status.
            runCommand("printf '%s' '$data' | base64 -d >&3")
            position += count
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        shell.use {
            failure?.let { throw IOException("File stream write failed", it) }
            runCommand("exec 3>&-")
        }
    }

    private fun runCommand(command: String) {
        try {
            // Avoid logging encoded file contents through libsu's command logger.
            val result = shell.newJob().add(command.byteInputStream()).exec()
            if (!result.isSuccess) throw IOException("Root file operation failed (${result.code})")
        } catch (error: Exception) {
            val ioError = error as? IOException ?: IOException("Root file operation failed", error)
            failure = ioError
            throw ioError
        }
    }
}
