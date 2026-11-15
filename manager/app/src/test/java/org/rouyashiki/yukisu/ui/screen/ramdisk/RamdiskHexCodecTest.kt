package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.hex.backend.BinaryBytes
import kotlin.test.*

class RamdiskHexCodecTest {
    @Test fun unicodeScalarsKeepExactByteRangesAndRoundTripWithoutAddingBom() {
        val text = "A\u4e2d\ud83d\ude00Z"
        for (codec in ramdiskHexCodecs.filter { it.id != "ASCII" }) {
            val encoded = codec.encode(text)
            val characters = codec.decode(encoded, 0)
            assertEquals(listOf("A", "\u4e2d", "\ud83d\ude00", "Z"), characters.map { it.text }, codec.id)
            assertEquals(encoded.size, characters.sumOf { it.byteCount })
            assertEquals(0L, characters.first().offset)
            assertEquals(text, codec.decodeText(encoded))
            assertEquals(text.toByteArray(charset(codec.id)).toList(), encoded.toByteArray().toList())
        }
    }

    @Test fun utf8MultibyteMappingSupportsPartialSelectionAndInvalidBytes() {
        val codec = ramdiskHexCodecs.first { it.id == "UTF-8" }
        val characters = codec.decode(BinaryBytes.copyOf(byteArrayOf(0xe4.toByte(), 0xb8.toByte(), 0xad.toByte())), 65535)
        assertEquals(1, characters.size)
        assertEquals(65535L, characters[0].offset)
        assertEquals(3, characters[0].byteCount)
        assertEquals("\u4e2d", characters[0].text)
        val invalid = codec.decode(BinaryBytes.copyOf(byteArrayOf(0xc0.toByte(), 0xaf.toByte(), 0xf0.toByte(), 0x9f.toByte())), 0)
        assertEquals(listOf(0L, 1L, 2L, 3L), invalid.map { it.offset })
        assertTrue(invalid.all { it.text == "\u00b7" && it.byteCount == 1 })
    }

    @Test fun utf16WindowAlignmentAndSurrogatesRemainByteBased() {
        val codec = ramdiskHexCodecs.first { it.id == "UTF-16LE" }
        val bytes = codec.encode("A\u4e2d\ud83d\ude00Z").toByteArray()
        val characters = codec.decode(BinaryBytes.copyOf(bytes.copyOfRange(3, bytes.size)), 3)
        assertEquals(3L, characters[0].offset)
        assertEquals(1, characters[0].byteCount)
        assertEquals(4L, characters[1].offset)
        assertEquals(4, characters[1].byteCount)
        assertEquals("\ud83d\ude00", characters[1].text)
        assertEquals(2, codec.decode(BinaryBytes.copyOf(byteArrayOf(0, 0xd8.toByte())), 0).single().byteCount)
    }

    @Test fun utf32RejectsNonUnicodeValuesAndPreservesTruncatedTail() {
        val codec = ramdiskHexCodecs.first { it.id == "UTF-32LE" }
        val characters = codec.decode(BinaryBytes.copyOf(byteArrayOf(0, 0, 0x11, 0, 0x41)), 0)
        assertEquals(listOf(4, 1), characters.map { it.byteCount })
        assertTrue(characters.all { it.text == "\u00b7" })
    }
}
