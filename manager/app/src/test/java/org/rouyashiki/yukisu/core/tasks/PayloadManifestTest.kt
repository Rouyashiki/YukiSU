package org.rouyashiki.yukisu.core.tasks

import java.io.ByteArrayOutputStream
import java.io.IOException
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull

class PayloadManifestTest {

    @Test
    fun parsesAManifestTheWayUpdateEngineWritesOne() {
        val bytes = ProtoWriter()
            .varint(FIELD_BLOCK_SIZE, 4096L)
            .message(FIELD_PARTITIONS) {
                string(1, "boot")
                message(7) {
                    varint(1, 100663296L)
                    bytes(2, HASH)
                }
                message(8) {
                    varint(1, 8L)
                    varint(2, 0L)
                    varint(3, 262144L)
                    message(6) {
                        varint(1, 0L)
                        varint(2, 64L)
                    }
                }
                message(8) {
                    varint(1, 6L)
                    message(6) {
                        varint(1, 64L)
                        varint(2, 8L)
                    }
                }
            }
            .message(FIELD_PARTITIONS) {
                string(1, "init_boot")
            }
            .toByteArray()

        val manifest = DeltaArchiveManifest.parseFrom(bytes)

        assertEquals(4096L, manifest.blockSize)
        assertEquals(listOf("boot", "init_boot"), manifest.partitions.map { it.partitionName })

        val boot = manifest.partitions[0]
        assertEquals(100663296L, boot.newPartitionInfo.size)
        assertContentEquals(HASH, boot.newPartitionInfo.hash)
        assertEquals(2, boot.operations.size)

        val replace = boot.operations[0]
        assertEquals(InstallOperationType.REPLACE_XZ, replace.type)
        assertEquals(0L, replace.dataOffset)
        assertEquals(262144L, replace.dataLength)
        assertEquals(1, replace.dstExtents.size)
        assertEquals(0L, replace.dstExtents[0].startBlock)
        assertEquals(64L, replace.dstExtents[0].numBlocks)

        val zero = boot.operations[1]
        assertEquals(InstallOperationType.ZERO, zero.type)
        assertEquals(64L, zero.dstExtents[0].startBlock)
    }

    @Test
    fun missingBlockSizeFallsBackToTheSchemaDefault() {
        val bytes = ProtoWriter()
            .message(FIELD_PARTITIONS) { string(1, "boot") }
            .toByteArray()

        assertEquals(4096L, DeltaArchiveManifest.parseFrom(bytes).blockSize)
    }

    @Test
    fun absentPartitionInfoReadsAsEmpty() {
        val bytes = ProtoWriter()
            .message(FIELD_PARTITIONS) { string(1, "boot") }
            .toByteArray()

        val info = DeltaArchiveManifest.parseFrom(bytes).partitions[0].newPartitionInfo
        assertEquals(0L, info.size)
        assertNull(info.hash)
    }

    @Test
    fun unknownFieldsOfEveryWireTypeAreSkipped() {
        val bytes = ProtoWriter()
            .varint(99, 1L shl 40)
            .fixed64(98, -1L)
            .bytes(97, ByteArray(300) { 0x7F })
            .fixed32(96, 0x0BADF00D)
            .varint(FIELD_BLOCK_SIZE, 8192L)
            .message(FIELD_PARTITIONS) {
                varint(50, 7L)
                string(1, "boot")
                bytes(51, byteArrayOf(1, 2, 3))
            }
            .toByteArray()

        val manifest = DeltaArchiveManifest.parseFrom(bytes)
        assertEquals(8192L, manifest.blockSize)
        assertEquals(listOf("boot"), manifest.partitions.map { it.partitionName })
    }

    @Test
    fun aKnownFieldWithTheWrongWireTypeIsSkippedNotMisread() {
        // block_size arriving length-delimited rather than as a varint. Matching
        // on the whole tag means this is skipped like any unknown field, so the
        // default survives and the fields after it still line up.
        val bytes = ProtoWriter()
            .bytes(FIELD_BLOCK_SIZE, byteArrayOf(0x00, 0x20))
            .message(FIELD_PARTITIONS) { string(1, "boot") }
            .toByteArray()

        val manifest = DeltaArchiveManifest.parseFrom(bytes)
        assertEquals(4096L, manifest.blockSize)
        assertEquals(listOf("boot"), manifest.partitions.map { it.partitionName })
    }

    @Test
    fun operationOffsetsSpanTheFull64BitRange() {
        val bytes = ProtoWriter()
            .message(FIELD_PARTITIONS) {
                string(1, "boot")
                message(8) {
                    varint(1, 0L)
                    varint(2, 5_000_000_000L)
                    varint(3, 4_294_967_296L)
                }
            }
            .toByteArray()

        val operation = DeltaArchiveManifest.parseFrom(bytes).partitions[0].operations[0]
        assertEquals(5_000_000_000L, operation.dataOffset)
        assertEquals(4_294_967_296L, operation.dataLength)
    }

    @Test
    fun operationTypesWeCannotApplyBecomeUnsupported() {
        // SOURCE_COPY, PUFFDIFF, REPLACE_ZSTD, and an id newer than the schema.
        for (id in listOf(4L, 9L, 14L, 99L)) {
            val bytes = ProtoWriter()
                .message(FIELD_PARTITIONS) {
                    string(1, "boot")
                    message(8) { varint(1, id) }
                }
                .toByteArray()

            assertEquals(
                InstallOperationType.UNSUPPORTED,
                DeltaArchiveManifest.parseFrom(bytes).partitions[0].operations[0].type,
                "operation type $id",
            )
        }
    }

    @Test
    fun aNestedMessageLongerThanTheBufferIsRejected() {
        // partitions[0] declares 64 bytes; four follow.
        val bytes = byteArrayOf(((13 shl 3) or 2).toByte(), 64, 1, 2, 3, 4)

        assertFailsWith<IOException> { DeltaArchiveManifest.parseFrom(bytes) }
    }

    @Test
    fun everyTruncationFailsCleanly() {
        val bytes = ProtoWriter()
            .varint(FIELD_BLOCK_SIZE, 4096L)
            .message(FIELD_PARTITIONS) {
                string(1, "boot")
                message(7) { bytes(2, HASH) }
                message(8) {
                    varint(1, 8L)
                    varint(2, 5_000_000_000L)
                    message(6) { varint(2, 64L) }
                }
            }
            .toByteArray()

        for (length in 0 until bytes.size) {
            try {
                DeltaArchiveManifest.parseFrom(bytes.copyOf(length))
            } catch (_: IOException) {
                // Expected wherever the cut lands mid-field. Anything else --
                // an index out of bounds, say -- fails the test.
            }
        }
    }

    private companion object {
        const val FIELD_BLOCK_SIZE = 3
        const val FIELD_PARTITIONS = 13

        val HASH = ByteArray(32) { (it * 7).toByte() }
    }
}

/** Writes protobuf wire format so the tests can feed the parser real encodings. */
private class ProtoWriter {
    private val out = ByteArrayOutputStream()

    fun varint(fieldNumber: Int, value: Long) = apply {
        tag(fieldNumber, 0)
        writeVarint(value)
    }

    fun fixed64(fieldNumber: Int, value: Long) = apply {
        tag(fieldNumber, 1)
        for (shift in 0 until 64 step 8) {
            out.write(((value ushr shift) and 0xFF).toInt())
        }
    }

    fun bytes(fieldNumber: Int, value: ByteArray) = apply {
        tag(fieldNumber, 2)
        writeVarint(value.size.toLong())
        out.write(value, 0, value.size)
    }

    fun fixed32(fieldNumber: Int, value: Int) = apply {
        tag(fieldNumber, 5)
        for (shift in 0 until 32 step 8) {
            out.write((value ushr shift) and 0xFF)
        }
    }

    fun string(fieldNumber: Int, value: String) = bytes(fieldNumber, value.encodeToByteArray())

    fun message(fieldNumber: Int, block: ProtoWriter.() -> Unit) =
        bytes(fieldNumber, ProtoWriter().apply(block).toByteArray())

    fun toByteArray(): ByteArray = out.toByteArray()

    private fun tag(fieldNumber: Int, wireType: Int) =
        writeVarint(((fieldNumber shl 3) or wireType).toLong())

    private fun writeVarint(value: Long) {
        var remaining = value
        while (true) {
            val chunk = (remaining and 0x7F).toInt()
            remaining = remaining ushr 7
            if (remaining == 0L) {
                out.write(chunk)
                return
            }
            out.write(chunk or 0x80)
        }
    }
}
