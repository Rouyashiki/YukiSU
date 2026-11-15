package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.hex.backend.AsciiBinaryTextCodec
import com.anatdx.yukifb.hex.backend.BinaryBytes
import com.anatdx.yukifb.hex.backend.BinaryCharacter
import com.anatdx.yukifb.hex.backend.BinaryTextCodec
import java.nio.CharBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction

internal val ramdiskHexCodecs: List<BinaryTextCodec> = listOf(
    RamdiskHexCodec("UTF-8", 1, false),
    AsciiBinaryTextCodec,
    RamdiskHexCodec("UTF-16LE", 2, true),
    RamdiskHexCodec("UTF-16BE", 2, false),
    RamdiskHexCodec("UTF-32LE", 4, true),
    RamdiskHexCodec("UTF-32BE", 4, false),
).filter { Charset.isSupported(it.id) }

internal class RamdiskHexCodec(
    override val id: String,
    private val unit: Int,
    private val littleEndian: Boolean,
) : BinaryTextCodec {
    override val label: String = id
    override val contextBytes: Int = 8
    private val charset: Charset = Charset.forName(id)

    override fun encode(text: String): BinaryBytes {
        try {
            val encoded = charset.newEncoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .encode(CharBuffer.wrap(text))
            return BinaryBytes.copyOf(ByteArray(encoded.remaining()).also(encoded::get))
        } catch (error: CharacterCodingException) {
            throw IllegalArgumentException("Text cannot be represented as " + id, error)
        }
    }

    override fun decodeText(bytes: BinaryBytes): String = bytes.toByteArray().toString(charset)

    override fun decode(bytes: BinaryBytes, offset: Long): List<BinaryCharacter> {
        val result = ArrayList<BinaryCharacter>()
        var index = 0
        fun byte(at: Int): Int = bytes[at].toInt() and 255
        fun word(at: Int, count: Int): Long {
            var value = 0L
            for (position in 0 until count) {
                val shift = (if (littleEndian) position else count - position - 1) * 8
                value = value or (byte(at + position).toLong() shl shift)
            }
            return value
        }
        fun add(codePoint: Int?, count: Int) {
            val display = if (codePoint == null || Character.isISOControl(codePoint) ||
                Character.getType(codePoint) == Character.FORMAT.toInt()) "\u00b7"
            else String(Character.toChars(codePoint))
            result += BinaryCharacter(offset + index, count, display)
            index += count
        }
        while (index < bytes.size) {
            if ((offset + index) % unit != 0L || bytes.size - index < unit) {
                add(null, 1)
                continue
            }
            if (unit == 1) {
                val first = byte(index)
                val count = when (first) {
                    in 0..0x7f -> 1
                    in 0xc2..0xdf -> 2
                    in 0xe0..0xef -> 3
                    in 0xf0..0xf4 -> 4
                    else -> 0
                }
                if (count == 0 || count > bytes.size - index ||
                    (1 until count).any { byte(index + it) !in 0x80..0xbf }) {
                    add(null, 1)
                    continue
                }
                var scalar = first and when (count) { 1 -> 0x7f; 2 -> 0x1f; 3 -> 0x0f; else -> 7 }
                for (part in 1 until count) scalar = (scalar shl 6) or (byte(index + part) and 0x3f)
                val minimum = when (count) { 1 -> 0; 2 -> 0x80; 3 -> 0x800; else -> 0x10000 }
                if (scalar < minimum || scalar > 0x10ffff || scalar in 0xd800..0xdfff) add(null, 1)
                else add(scalar, count)
            } else if (unit == 2) {
                val first = word(index, 2).toInt()
                when {
                    first in 0xd800..0xdbff -> {
                        val second = if (bytes.size - index >= 4) word(index + 2, 2).toInt() else 0
                        if (second in 0xdc00..0xdfff) add(0x10000 + ((first - 0xd800) shl 10) + second - 0xdc00, 4)
                        else add(null, 2)
                    }
                    first in 0xdc00..0xdfff -> add(null, 2)
                    else -> add(first, 2)
                }
            } else {
                val scalar = word(index, 4)
                add(scalar.takeIf { it <= 0x10ffff && it !in 0xd800..0xdfff }?.toInt(), 4)
            }
        }
        return result
    }
}
