package org.rouyashiki.yukisu.core.utils

import java.io.IOException

/**
 * Minimal protobuf wire-format reader: enough to walk payload.bin's manifest,
 * and nothing more.
 *
 * Callers match on the whole tag rather than on the field number alone, so a
 * field arriving with an unexpected wire type falls through to [skip] exactly
 * like an unknown field would. Nested messages share the backing array instead
 * of copying it, which matters because a manifest carries tens of thousands of
 * install operations.
 */
internal class ProtoReader private constructor(
    private val buffer: ByteArray,
    private var pos: Int,
    private val end: Int,
) {
    constructor(buffer: ByteArray) : this(buffer, 0, buffer.size)

    fun hasNext(): Boolean = pos < end

    /** Field number shifted left by 3, or-ed with one of the `WIRE_*` types. */
    fun readTag(): Int = readVarint().toInt()

    fun readVarint(): Long {
        var result = 0L
        var shift = 0
        while (shift < Long.SIZE_BITS) {
            val byte = readByte().toInt()
            result = result or ((byte.toLong() and 0x7F) shl shift)
            if (byte and 0x80 == 0) return result
            shift += 7
        }
        throw IOException("Malformed varint")
    }

    fun readBytes(): ByteArray {
        val length = readLength()
        val bytes = buffer.copyOfRange(pos, pos + length)
        pos += length
        return bytes
    }

    fun readString(): String = readBytes().decodeToString()

    /** A reader bounded to the nested message, positioned past it on return. */
    fun readMessage(): ProtoReader {
        val length = readLength()
        val message = ProtoReader(buffer, pos, pos + length)
        pos += length
        return message
    }

    fun skip(tag: Int) {
        when (val wireType = tag and 0x7) {
            WIRE_VARINT -> readVarint()
            WIRE_FIXED64 -> advance(8)
            WIRE_LENGTH_DELIMITED -> advance(readLength())
            WIRE_FIXED32 -> advance(4)
            else -> throw IOException("Unsupported wire type $wireType")
        }
    }

    private fun readLength(): Int {
        val length = readVarint()
        if (length < 0 || length > end - pos) {
            throw IOException("Truncated field")
        }
        return length.toInt()
    }

    private fun readByte(): Byte {
        if (pos >= end) {
            throw IOException("Truncated message")
        }
        return buffer[pos++]
    }

    private fun advance(count: Int) {
        if (count > end - pos) {
            throw IOException("Truncated message")
        }
        pos += count
    }

    companion object {
        const val WIRE_VARINT = 0
        const val WIRE_FIXED64 = 1
        const val WIRE_LENGTH_DELIMITED = 2
        const val WIRE_FIXED32 = 5
    }
}
