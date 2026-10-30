package com.anatdx.yukisu.ui.webui

import android.app.Application
import android.util.Base64
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class WebUIDownloadTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun chunkedDownloadUsesTheReturnedIdAndPreservesBinaryData() {
        val handler = BlobDownloadHandler()
        val id = handler.registerBlobDownload("large.bin", "application/octet-stream", temporary.root)
        val payload = ByteArray(11 * 1024 * 1024 + 7) { it.toByte() }
        for (start in payload.indices step (512 * 1024)) {
            val length = minOf(512 * 1024, payload.size - start)
            assertTrue(handler.writeChunk(id, Base64.encodeToString(payload, start, length, Base64.NO_WRAP)))
        }
        val completed = assertNotNull(handler.completeDownload(id))
        assertContentEquals(payload, completed.file.readBytes())
        assertNull(handler.completeDownload(id))
        assertFalse(handler.writeChunk(id, ""))
        handler.cleanup()
        assertTrue(completed.file.exists())
        completed.file.delete()
    }

    @Test fun closingOneSessionLeavesOtherDownloadsAvailable() {
        val first = BlobDownloadHandler()
        val second = BlobDownloadHandler()
        val firstId = first.registerBlobDownload("one", null, temporary.root)
        val secondId = second.registerBlobDownload("two", null, temporary.root)
        first.cleanup()
        assertNull(first.completeDownload(firstId))
        assertFailsWith<IllegalStateException> { first.registerBlobDownload("closed", null, temporary.root) }
        assertTrue(second.writeChunk(secondId, "dHdv"))
        val completed = assertNotNull(second.completeDownload(secondId))
        assertEquals("two", completed.file.readText())
        completed.file.delete()
        assertTrue(temporary.root.listFiles().orEmpty().isEmpty())
    }

    @Test fun cancelAndMalformedChunksRemoveTemporaryFiles() {
        val handler = BlobDownloadHandler()
        val cancelled = handler.registerBlobDownload("one", null, temporary.root)
        handler.cancelDownload(cancelled)
        val malformed = handler.registerBlobDownload("two", null, temporary.root)
        assertFalse(handler.writeChunk(malformed, "a"))
        assertTrue(temporary.root.listFiles().orEmpty().isEmpty())
    }

    @Test fun fileNamesStayWithinDownloads() {
        assertEquals("payload.bin", sanitizeWebUIDownloadFileName("../../payload.bin"))
        assertEquals("payload.bin", sanitizeWebUIDownloadFileName("C:\\tmp\\payload.bin"))
        assertEquals("download.bin", sanitizeWebUIDownloadFileName(".."))
        assertEquals("download.bin", sanitizeWebUIDownloadFileName(null))
        assertEquals("file", sanitizeWebUIDownloadFileName("\u0000file\r\n"))
    }

    @Test fun outputClosesWhenCopyIsCancelled() {
        var closed = false
        val output = object : ByteArrayOutputStream() {
            override fun close() { closed = true; super.close() }
        }
        assertFailsWith<IOException> {
            writeWebUIDownload(ByteArrayInputStream(ByteArray(16384)), output) { throw IOException("cancelled") }
        }
        assertTrue(closed)
    }
}
