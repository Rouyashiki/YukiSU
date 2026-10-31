package com.anatdx.yukisu.ui.webui.file

import android.util.Log
import android.webkit.JavascriptInterface
import com.anatdx.yukisu.ui.util.createRootShell
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.ConcurrentHashMap

const val TAG = "KsuIO"

class KsuIO(private val shellFactory: () -> Shell = { createRootShell(true) }) {
    companion object {
        private const val KSU_IO_VERSION = 1
        private const val MAX_OPEN_STREAMS = 50
    }

    internal var closed = false
        private set

    internal val openInputStreams = ConcurrentHashMap<String, ManagedInputStream>()
    internal val openOutputStreams = ConcurrentHashMap<String, OutputStream>()
    private var fileShell: Shell? = null

    private val fileOutputStream = FileOutputStreamInterface(this)
    private val fileInputStream = FileInputStreamInterface(this)
    private val randomAccessFile = RandomAccessFileInterface(this)

    @JavascriptInterface
    fun version(): Int = KSU_IO_VERSION

    @JavascriptInterface
    fun File(path: String): FileInterface = FileInterface(this, path)

    @JavascriptInterface
    fun FileInputStream(): FileInputStreamInterface = fileInputStream

    @JavascriptInterface
    fun FileOutputStream(): FileOutputStreamInterface = fileOutputStream

    @JavascriptInterface
    fun RandomAccessFile(): RandomAccessFileInterface = randomAccessFile

    internal fun canOpenStream(): Boolean {
        return !closed && openInputStreams.size + openOutputStreams.size < MAX_OPEN_STREAMS
    }

    @Synchronized
    internal fun file(path: String): SuFile {
        if (closed) throw IOException("WebUI file session is closed")
        val shell = fileShell ?: shellFactory().also { fileShell = it }
        return SuFile(path).apply { setShell(shell) }
    }

    @Synchronized
    internal fun outputStream(path: String, append: Boolean): OutputStream {
        if (closed) throw IOException("WebUI file session is closed")
        return RootFileOutputStream(shellFactory(), path, append).buffered(64 * 1024)
    }

    @Synchronized
    fun destroy() {
        closed = true
        fileInputStream.closeAll()
        fileOutputStream.closeAll()
        randomAccessFile.closeAll()
        fileShell?.runCatching { close() }
            ?.onFailure { Log.e(TAG, "Failed to close file shell", it) }
        fileShell = null
    }
}
