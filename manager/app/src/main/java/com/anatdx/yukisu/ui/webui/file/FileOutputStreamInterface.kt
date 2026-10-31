package com.anatdx.yukisu.ui.webui.file

import android.util.Base64
import android.util.Log
import android.webkit.JavascriptInterface
import java.util.UUID

class FileOutputStreamInterface(private val io: KsuIO) {
    @JavascriptInterface
    fun open(path: String, append: Boolean): String = synchronized(io) {
        runCatching {
            if (!io.canOpenStream()) {
                Log.w(TAG, "FileOutputStream open rejected: too many open streams")
                return ""
            }
            val bos = io.outputStream(path, append)
            val id = UUID.randomUUID().toString()
            io.openOutputStreams[id] = bos
            id
        }.onFailure { Log.e(TAG, "FileOutputStream open failed", it) }.getOrElse { "" }
    }

    @JavascriptInterface
    fun open(path: String): String = open(path, false)

    @JavascriptInterface
    fun writeByte(id: String, b: Int): Boolean = runCatching {
        val bos = io.openOutputStreams[id] ?: return false
        synchronized(bos) { bos.write(b) }
        true
    }.onFailure { Log.e(TAG, "writeByte failed", it) }.getOrElse { false }

    @JavascriptInterface
    fun write(id: String, base64: String): Boolean = runCatching {
        val bos = io.openOutputStreams[id] ?: return false
        val data = Base64.decode(base64, Base64.NO_WRAP)
        synchronized(bos) { bos.write(data) }
        true
    }.onFailure { Log.e(TAG, "write failed", it) }.getOrElse { false }

    @JavascriptInterface
    fun flush(id: String): Boolean = runCatching {
        val bos = io.openOutputStreams[id] ?: return false
        synchronized(bos) { bos.flush() }
        true
    }.onFailure { Log.e(TAG, "flush failed", it) }.getOrElse { false }

    @JavascriptInterface
    fun close(id: String): Boolean {
        val bos = io.openOutputStreams.remove(id) ?: return false
        return runCatching {
            synchronized(bos) { bos.close() }
            true
        }.onFailure { Log.e(TAG, "close failed", it) }.getOrElse { false }
    }

    fun closeAll() {
        while (true) {
            val entry = io.openOutputStreams.entries.firstOrNull() ?: break
            val id = entry.key
            val bos = entry.value
            if (!io.openOutputStreams.remove(id, bos)) {
                continue
            }
            runCatching {
                synchronized(bos) { bos.close() }
            }.onFailure { Log.e(TAG, "closeAll failed for $id", it) }
        }
    }
}
