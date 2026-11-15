package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.model.TextFileEncoding
import java.io.ByteArrayInputStream
import java.io.IOException
import java.nio.charset.Charset
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class RamdiskTextCodecTest {
    @Test
    fun roundTripsEverySupportedEncodingWithoutChangingBomPolicy() {
        val text = "first\r\nsecond\nthird\r"

        TextFileEncoding.entries.forEach { encoding ->
            if (!Charset.isSupported(encoding.charsetName)) return@forEach

            val encoded = encodeRamdiskText(text, encoding)
            val decoded = decodeRamdiskText(encoded.bytes, encoding)

            assertEquals(text, decoded.text, encoding.displayName)
            assertEquals(encoding, decoded.encoding)
            assertEquals(
                encoding == TextFileEncoding.UTF_8_BOM,
                encoded.byteOrderMark != null,
                encoding.displayName,
            )
        }
    }

    @Test
    fun automaticallyDetectsBomlessUtf16AndUtf32AsciiText() {
        val text = "on boot\nsetprop test true\n"
        val encodings = listOf(
            TextFileEncoding.UTF_16_BE,
            TextFileEncoding.UTF_16_LE,
            TextFileEncoding.UTF_32_BE,
            TextFileEncoding.UTF_32_LE,
        )

        encodings.forEach { encoding ->
            if (!Charset.isSupported(encoding.charsetName)) return@forEach
            val bytes = encodeRamdiskText(text, encoding).bytes

            val decoded = decodeInitialRamdiskText(bytes)

            assertEquals(encoding, decoded.encoding)
            assertEquals(text, decoded.text)
            assertEquals(null, decoded.byteOrderMark)
        }
    }

    @Test
    fun consumesAndWritesUtf8BomExactlyOnce() {
        val textBytes = "hello".toByteArray(Charsets.UTF_8)
        val source = UnicodeByteOrderMark.UTF_8.bytes + textBytes

        val decoded = decodeInitialRamdiskText(source)
        val encoded = encodeRamdiskText(
            text = decoded.text,
            encoding = decoded.encoding,
            preservedByteOrderMark = decoded.byteOrderMark,
        )

        assertEquals(TextFileEncoding.UTF_8_BOM, decoded.encoding)
        assertEquals("hello", decoded.text)
        assertEquals(UnicodeByteOrderMark.UTF_8, encoded.byteOrderMark)
        assertContentEquals(source, encoded.bytes)
    }

    @Test
    fun preservesExistingUtf16BomOnlyForSameEncoding() {
        val littleEndian = Charset.forName(TextFileEncoding.UTF_16_LE.charsetName)
        val source = UnicodeByteOrderMark.UTF_16_LE.bytes +
            "alpha".toByteArray(littleEndian)
        val decoded = decodeInitialRamdiskText(source)

        val preserved = encodeRamdiskText(
            text = "alpha!",
            encoding = decoded.encoding,
            preservedByteOrderMark = decoded.byteOrderMark,
        )
        val switched = encodeRamdiskText(
            text = "alpha!",
            encoding = TextFileEncoding.UTF_16_BE,
            preservedByteOrderMark = decoded.byteOrderMark,
        )

        assertEquals(TextFileEncoding.UTF_16_LE, decoded.encoding)
        assertEquals(UnicodeByteOrderMark.UTF_16_LE, preserved.byteOrderMark)
        assertTrue(preserved.bytes.startsWith(UnicodeByteOrderMark.UTF_16_LE.bytes))
        assertEquals(null, switched.byteOrderMark)
        assertFalse(switched.bytes.startsWith(UnicodeByteOrderMark.UTF_16_BE.bytes))
        assertFalse(switched.bytes.startsWith(UnicodeByteOrderMark.UTF_16_LE.bytes))
    }

    @Test
    fun selectingUtf8ForSaveRemovesExistingUtf8Bom() {
        val source = UnicodeByteOrderMark.UTF_8.bytes +
            "hello".toByteArray(Charsets.UTF_8)
        val decoded = decodeRamdiskText(source, TextFileEncoding.UTF_8)

        val encoded = encodeRamdiskText(
            text = decoded.text,
            encoding = TextFileEncoding.UTF_8,
            preservedByteOrderMark = decoded.byteOrderMark,
        )

        assertEquals("hello", decoded.text)
        assertEquals(TextFileEncoding.UTF_8_BOM, decoded.encoding)
        assertEquals(null, encoded.byteOrderMark)
        assertContentEquals("hello".toByteArray(Charsets.UTF_8), encoded.bytes)
    }

    @Test
    fun reopeningBomlessUtf8WithBomDecoderKeepsPersistedFormatAccurate() {
        val source = "hello".toByteArray(Charsets.UTF_8)

        val decoded = decodeRamdiskText(source, TextFileEncoding.UTF_8_BOM)

        assertEquals("hello", decoded.text)
        assertEquals(TextFileEncoding.UTF_8, decoded.encoding)
        assertEquals(null, decoded.byteOrderMark)
    }

    @Test
    fun asksForConfirmationWhenUtf16CjkBytesAlsoLookLikeUtf8() {
        val source = "你好".toByteArray(Charset.forName("UTF-16LE"))
        val shortBigEndianSource = "中".toByteArray(Charset.forName("UTF-16BE"))

        val ambiguous = decodeInitialRamdiskText(source)
        val shortBigEndianAmbiguous = decodeInitialRamdiskText(shortBigEndianSource)
        val ordinaryUtf8 = decodeInitialRamdiskText(
            "on boot\nsetprop test true\n".toByteArray(Charsets.UTF_8)
        )

        assertEquals(TextFileEncoding.UTF_8, ambiguous.encoding)
        assertTrue(ambiguous.requiresEncodingConfirmation)
        assertTrue(shortBigEndianAmbiguous.requiresEncodingConfirmation)
        assertFalse(ordinaryUtf8.requiresEncodingConfirmation)
        assertEquals(
            "你好",
            decodeRamdiskText(source, TextFileEncoding.UTF_16_LE).text,
        )
    }

    @Test
    fun rejectsConflictingBomMalformedInputAndDecodedNul() {
        val utf16Le = UnicodeByteOrderMark.UTF_16_LE.bytes +
            "hello".toByteArray(Charset.forName("UTF-16LE"))

        assertFailsWith<IOException> {
            decodeRamdiskText(utf16Le, TextFileEncoding.UTF_16_BE)
        }
        assertFailsWith<IOException> {
            decodeRamdiskText(byteArrayOf(0xc3.toByte(), 0x28), TextFileEncoding.UTF_8)
        }
        assertFailsWith<IOException> {
            decodeRamdiskText(byteArrayOf('a'.code.toByte(), 0), TextFileEncoding.UTF_8)
        }
    }

    @Test
    fun rejectsUnpairedSurrogateDuringEncoding() {
        assertFailsWith<IOException> {
            encodeRamdiskText("\ud800", TextFileEncoding.UTF_8)
        }
    }

    @Test
    fun enforcesReadLimit() {
        val oversized = ByteArray(MAX_RAMDISK_TEXT_FILE_SIZE + 1) { 'a'.code.toByte() }

        assertFailsWith<IOException> {
            readRamdiskTextBytes(ByteArrayInputStream(oversized))
        }
    }

    private fun ByteArray.startsWith(prefix: ByteArray): Boolean =
        size >= prefix.size && prefix.indices.all { index -> this[index] == prefix[index] }
}
