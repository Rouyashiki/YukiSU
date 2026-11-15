package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.hex.backend.BinaryBytes
import com.anatdx.yukifb.hex.backend.BinaryEditStorage
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.io.RandomAccessFile

internal class RamdiskHexStorage private constructor(
    private val file: File,
    private val content: RandomAccessFile,
) : BinaryEditStorage {
    private val mutex = Mutex()
    private var length = 0L
    private var closed = false

    override suspend fun append(bytes: BinaryBytes): Long = withContext(Dispatchers.IO) {
        mutex.withLock {
            check(!closed)
            require(bytes.size <= Long.MAX_VALUE - length)
            val offset = length
            content.seek(offset)
            content.write(bytes.toByteArray())
            length += bytes.size
            offset
        }
    }

    override suspend fun read(offset: Long, length: Int): BinaryBytes = withContext(Dispatchers.IO) {
        mutex.withLock {
            check(!closed)
            require(offset in 0..this@RamdiskHexStorage.length && length >= 0 &&
                length <= this@RamdiskHexStorage.length - offset)
            val bytes = ByteArray(length)
            content.seek(offset)
            content.readFully(bytes)
            BinaryBytes.copyOf(bytes)
        }
    }

    override suspend fun close(): Unit = withContext(NonCancellable + Dispatchers.IO) {
        mutex.withLock {
            if (!closed) {
                closed = true
                try { content.close() } finally { file.delete() }
            }
        }
    }

    companion object {
        suspend fun create(directory: File): RamdiskHexStorage {
            var created: RamdiskHexStorage? = null
            try {
                return withContext(Dispatchers.IO) {
                    val file = File.createTempFile("hex_", ".tmp", directory)
                    try {
                        RamdiskHexStorage(file, RandomAccessFile(file, "rw")).also { created = it }
                    } catch (error: Throwable) {
                        file.delete()
                        throw error
                    }
                }
            } catch (error: Throwable) {
                created?.close()
                throw error
            }
        }
    }
}
