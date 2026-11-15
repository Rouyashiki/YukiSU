package org.rouyashiki.yukisu.core.tasks

import org.rouyashiki.yukisu.core.utils.ProtoReader

// The slice of chromeos_update_engine's update_metadata.proto that extracting a
// boot partition needs, read straight off the wire. Field numbers are frozen by
// that schema, and everything not named here is skipped as an unknown field, so
// a payload from a newer update_engine still parses.

/**
 * Only the ops that carry their own data are modelled. The delta ops all need a
 * source partition we do not have, and REPLACE_ZSTD needs a decoder we do not
 * ship; both land in [UNSUPPORTED].
 */
internal enum class InstallOperationType {
    REPLACE,
    REPLACE_BZ,
    ZERO,
    REPLACE_XZ,
    UNSUPPORTED,
    ;

    companion object {
        fun of(id: Long): InstallOperationType = when (id) {
            0L -> REPLACE
            1L -> REPLACE_BZ
            6L -> ZERO
            8L -> REPLACE_XZ
            else -> UNSUPPORTED
        }
    }
}

internal class Extent(val startBlock: Long, val numBlocks: Long) {
    companion object {
        private const val TAG_START_BLOCK = (1 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_NUM_BLOCKS = (2 shl 3) or ProtoReader.WIRE_VARINT

        fun parseFrom(reader: ProtoReader): Extent {
            var startBlock = 0L
            var numBlocks = 0L
            while (reader.hasNext()) {
                when (val tag = reader.readTag()) {
                    TAG_START_BLOCK -> startBlock = reader.readVarint()
                    TAG_NUM_BLOCKS -> numBlocks = reader.readVarint()
                    else -> reader.skip(tag)
                }
            }
            return Extent(startBlock, numBlocks)
        }
    }
}

internal class PartitionInfo(val size: Long, val hash: ByteArray?) {
    companion object {
        private const val TAG_SIZE = (1 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_HASH = (2 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED

        /** Stands in for an absent `new_partition_info`, as the schema default would. */
        val EMPTY = PartitionInfo(size = 0, hash = null)

        fun parseFrom(reader: ProtoReader): PartitionInfo {
            var size = 0L
            var hash: ByteArray? = null
            while (reader.hasNext()) {
                when (val tag = reader.readTag()) {
                    TAG_SIZE -> size = reader.readVarint()
                    TAG_HASH -> hash = reader.readBytes()
                    else -> reader.skip(tag)
                }
            }
            return PartitionInfo(size, hash)
        }
    }
}

internal class InstallOperation(
    val type: InstallOperationType,
    val dataOffset: Long,
    val dataLength: Long,
    val dstExtents: List<Extent>,
) {
    companion object {
        private const val TAG_TYPE = (1 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_DATA_OFFSET = (2 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_DATA_LENGTH = (3 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_DST_EXTENTS = (6 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED

        fun parseFrom(reader: ProtoReader): InstallOperation {
            var type = InstallOperationType.UNSUPPORTED
            var dataOffset = 0L
            var dataLength = 0L
            val dstExtents = mutableListOf<Extent>()
            while (reader.hasNext()) {
                when (val tag = reader.readTag()) {
                    TAG_TYPE -> type = InstallOperationType.of(reader.readVarint())
                    TAG_DATA_OFFSET -> dataOffset = reader.readVarint()
                    TAG_DATA_LENGTH -> dataLength = reader.readVarint()
                    TAG_DST_EXTENTS -> dstExtents += Extent.parseFrom(reader.readMessage())
                    else -> reader.skip(tag)
                }
            }
            return InstallOperation(type, dataOffset, dataLength, dstExtents)
        }
    }
}

internal class PartitionUpdate(
    val partitionName: String,
    val newPartitionInfo: PartitionInfo,
    val operations: List<InstallOperation>,
) {
    companion object {
        private const val TAG_PARTITION_NAME = (1 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED
        private const val TAG_NEW_PARTITION_INFO = (7 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED
        private const val TAG_OPERATIONS = (8 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED

        fun parseFrom(reader: ProtoReader): PartitionUpdate {
            var partitionName = ""
            var newPartitionInfo = PartitionInfo.EMPTY
            val operations = mutableListOf<InstallOperation>()
            while (reader.hasNext()) {
                when (val tag = reader.readTag()) {
                    TAG_PARTITION_NAME -> partitionName = reader.readString()
                    TAG_NEW_PARTITION_INFO ->
                        newPartitionInfo = PartitionInfo.parseFrom(reader.readMessage())

                    TAG_OPERATIONS -> operations += InstallOperation.parseFrom(reader.readMessage())
                    else -> reader.skip(tag)
                }
            }
            return PartitionUpdate(partitionName, newPartitionInfo, operations)
        }
    }
}

internal class DeltaArchiveManifest(
    val blockSize: Long,
    val partitions: List<PartitionUpdate>,
) {
    companion object {
        private const val TAG_BLOCK_SIZE = (3 shl 3) or ProtoReader.WIRE_VARINT
        private const val TAG_PARTITIONS = (13 shl 3) or ProtoReader.WIRE_LENGTH_DELIMITED

        // The schema declares block_size as [default = 4096].
        private const val DEFAULT_BLOCK_SIZE = 4096L

        fun parseFrom(bytes: ByteArray): DeltaArchiveManifest {
            var blockSize = DEFAULT_BLOCK_SIZE
            val partitions = mutableListOf<PartitionUpdate>()
            val reader = ProtoReader(bytes)
            while (reader.hasNext()) {
                when (val tag = reader.readTag()) {
                    TAG_BLOCK_SIZE -> blockSize = reader.readVarint()
                    TAG_PARTITIONS -> partitions += PartitionUpdate.parseFrom(reader.readMessage())
                    else -> reader.skip(tag)
                }
            }
            return DeltaArchiveManifest(blockSize, partitions)
        }
    }
}
