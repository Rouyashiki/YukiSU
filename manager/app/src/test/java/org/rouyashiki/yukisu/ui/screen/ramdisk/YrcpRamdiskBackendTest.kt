package org.rouyashiki.yukisu.ui.screen.ramdisk

import java.io.ByteArrayOutputStream
import java.io.IOException
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull

class YrcpRamdiskBackendTest {
    @Test
    fun protocolV3NodeMapsElfContentKindToMimeType() {
        val node = nodeRecord(contentKind = 1).decodeSingleNode()

        assertEquals(YrcpContentKind.ELF, node.contentKind)
        assertEquals("module.ko", node.name)
        assertEquals(ELF_MIME_TYPE, node.toFileEntry(0L, "root").mimeType)
    }

    @Test
    fun unknownFutureContentKindRemainsUnclassified() {
        val node = nodeRecord(contentKind = 99).decodeSingleNode()

        assertEquals(YrcpContentKind.UNKNOWN, node.contentKind)
        assertNull(node.toFileEntry(0L, "root").mimeType)
    }

    @Test
    fun structuredElfHeaderPreservesTypedAndUnsignedFields() {
        val header = elfHeaderRecord().decodeElfHeaderInfo()

        assertEquals(1u, header.apiVersion)
        assertEquals(2, header.elfClass)
        assertEquals(1, header.dataEncoding)
        assertEquals(183, header.machine)
        assertEquals(0xfedc_ba98_7654_3210uL, header.entry)
        assertEquals(7776uL, header.sectionHeaderOffset)
        assertEquals(32, header.sectionHeaderCount)
        assertContentEquals(
            byteArrayOf(
                0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte(),
                2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            ),
            header.ident,
        )
    }

    @Test
    fun structuredElfHeaderRejectsTrailingBytes() {
        assertFailsWith<IOException> {
            wirePayload {
                bytes(elfHeaderRecord())
                byte(0)
            }.decodeElfHeaderInfo()
        }
    }


    @Test
    fun binaryInfoRequiresCompletePositiveRevisionRecord() {
        assertEquals(YrcpBinaryInfo(9, 0), wirePayload { u64(9L); u64(0L) }.decodeBinaryInfo())
        assertFailsWith<IOException> { wirePayload { u64(0L); u64(1L) }.decodeBinaryInfo() }
        assertFailsWith<IOException> { wirePayload { u64(9L); u64(1L); byte(0) }.decodeBinaryInfo() }
        assertFailsWith<IOException> { wirePayload { u64(9L) }.decodeBinaryInfo() }
    }

    private fun nodeRecord(contentKind: Int): ByteArray =
        wirePayload {
            u64(0x0001_0000_0000_002aL)
            u64(0x0001_0000_0000_0000L)
            u64(567_888L)
            repeat(10) { index -> u32(index.toLong()) }
            byte(0) // synthetic
            byte(contentKind)
            string("module.ko")
            string("lib/modules/module.ko")
            u32(0xffff_ffffL) // absent symbolic-link target
        }

    private fun elfHeaderRecord(): ByteArray =
        wirePayload {
            u16(1) // Schema version.
            u16(96) // Fixed structure size.
            u32(1) // readelf API version.
            u32(0) // Header flags.
            u32(0) // Reserved.
            u64(9_824L)
            bytes(
                byteArrayOf(
                    0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte(),
                    2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                )
            )
            byte(2) // ELF64.
            byte(1) // Little endian.
            byte(1) // Identification version.
            byte(0) // System V ABI.
            byte(0) // ABI version.
            repeat(3) { byte(0) }
            u16(3) // ET_DYN.
            u16(183) // AArch64.
            u32(1)
            u64(0xfedc_ba98_7654_3210uL)
            u64(64L)
            u64(7_776L)
            u32(0)
            u16(64)
            u16(56)
            u16(9)
            u16(64)
            u16(32)
            u16(29)
        }
}

private fun wirePayload(block: WireWriter.() -> Unit): ByteArray =
    WireWriter().apply(block).toByteArray()

private class WireWriter {
    private val output = ByteArrayOutputStream()

    fun byte(value: Int) {
        output.write(value and 0xff)
    }

    fun bytes(value: ByteArray) {
        output.write(value)
    }

    fun u16(value: Int) {
        repeat(2) { index -> byte(value ushr (index * 8)) }
    }

    fun u32(value: Long) {
        repeat(4) { index -> byte((value ushr (index * 8)).toInt()) }
    }

    fun u64(value: Long) {
        repeat(8) { index -> byte((value ushr (index * 8)).toInt()) }
    }

    fun u64(value: ULong) {
        repeat(8) { index -> byte((value shr (index * 8)).toInt()) }
    }

    fun string(value: String) {
        val bytes = value.toByteArray(Charsets.UTF_8)
        u32(bytes.size.toLong())
        output.write(bytes)
    }

    fun toByteArray(): ByteArray = output.toByteArray()
}
