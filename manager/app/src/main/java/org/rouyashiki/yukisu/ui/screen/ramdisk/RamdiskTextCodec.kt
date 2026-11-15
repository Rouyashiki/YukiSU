package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.model.TextFileEncoding
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import kotlin.math.min

internal data class DecodedRamdiskText(
    val text: String,
    val encoding: TextFileEncoding,
    val byteOrderMark: UnicodeByteOrderMark?,
    val requiresEncodingConfirmation: Boolean = false,
)

internal data class EncodedRamdiskText(
    val bytes: ByteArray,
    val byteOrderMark: UnicodeByteOrderMark?,
)

internal enum class UnicodeByteOrderMark(
    val canonicalEncoding: TextFileEncoding,
    vararg byteValues: Int,
) {
    UTF_32_BE(TextFileEncoding.UTF_32_BE, 0x00, 0x00, 0xfe, 0xff),
    UTF_32_LE(TextFileEncoding.UTF_32_LE, 0xff, 0xfe, 0x00, 0x00),
    UTF_8(TextFileEncoding.UTF_8_BOM, 0xef, 0xbb, 0xbf),
    UTF_16_BE(TextFileEncoding.UTF_16_BE, 0xfe, 0xff),
    UTF_16_LE(TextFileEncoding.UTF_16_LE, 0xff, 0xfe),
    ;

    val bytes: ByteArray = byteValues.map(Int::toByte).toByteArray()
}

internal val supportedRamdiskTextEncodings: List<TextFileEncoding> by lazy {
    TextFileEncoding.entries.filter { encoding ->
        runCatching { Charset.isSupported(encoding.charsetName) }.getOrDefault(false)
    }
}

internal fun readRamdiskTextBytes(input: InputStream): ByteArray {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var total = 0
    while (true) {
        val count = input.read(buffer)
        if (count < 0) break
        if (count == 0) continue
        total += count
        if (total > MAX_RAMDISK_TEXT_FILE_SIZE) {
            throw IOException("The file exceeds the text editor limit")
        }
        output.write(buffer, 0, count)
    }
    return output.toByteArray()
}

internal fun decodeInitialRamdiskText(bytes: ByteArray): DecodedRamdiskText {
    detectUnicodeByteOrderMark(bytes)?.let { byteOrderMark ->
        return decodeRamdiskText(bytes, byteOrderMark.canonicalEncoding)
    }

    runCatching {
        decodeRamdiskText(bytes, TextFileEncoding.UTF_8)
    }.getOrNull()?.let { decoded ->
        return decoded.copy(
            requiresEncodingConfirmation =
                hasHigherConfidenceUnicodeInterpretation(bytes, decoded.text),
        )
    }

    detectBomlessUnicodeEncoding(bytes)?.let { encoding ->
        runCatching {
            decodeRamdiskText(bytes, encoding)
        }.getOrNull()?.takeIf { decoded ->
            decoded.text.isPlausibleAutomaticallyDetectedText()
        }?.let { return it }
    }

    throw IOException("The selected file is not valid supported text")
}

internal fun decodeRamdiskText(
    bytes: ByteArray,
    encoding: TextFileEncoding,
): DecodedRamdiskText {
    val byteOrderMark = detectUnicodeByteOrderMark(bytes)
    if (byteOrderMark != null && !byteOrderMark.isCompatibleWith(encoding)) {
        throw IOException(
            "${byteOrderMark.canonicalEncoding.displayName} byte-order mark " +
                "does not match ${encoding.displayName}"
        )
    }

    val offset = byteOrderMark?.bytes?.size ?: 0
    val charset = charsetFor(encoding)
    val text = try {
        charset.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes, offset, bytes.size - offset))
            .toString()
    } catch (error: IOException) {
        throw IOException("The file is not valid ${encoding.displayName} text", error)
    }
    if ('\u0000' in text) {
        throw IOException("The selected file contains binary data")
    }
    val persistedEncoding = when {
        byteOrderMark == UnicodeByteOrderMark.UTF_8 -> TextFileEncoding.UTF_8_BOM
        encoding == TextFileEncoding.UTF_8_BOM -> TextFileEncoding.UTF_8
        else -> encoding
    }
    return DecodedRamdiskText(
        text = text,
        encoding = persistedEncoding,
        byteOrderMark = byteOrderMark,
    )
}

internal fun encodeRamdiskText(
    text: String,
    encoding: TextFileEncoding,
    preservedByteOrderMark: UnicodeByteOrderMark? = null,
): EncodedRamdiskText {
    val charset = charsetFor(encoding)
    val encoded = try {
        charset.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .encode(CharBuffer.wrap(text))
            .toByteArray()
    } catch (error: IOException) {
        throw IOException("The text cannot be encoded as ${encoding.displayName}", error)
    }
    val byteOrderMark = when {
        encoding == TextFileEncoding.UTF_8_BOM -> UnicodeByteOrderMark.UTF_8
        encoding == TextFileEncoding.UTF_8 -> null
        preservedByteOrderMark?.canonicalEncoding == encoding -> preservedByteOrderMark
        else -> null
    }
    if (byteOrderMark == null) {
        return EncodedRamdiskText(encoded, byteOrderMark = null)
    }
    return EncodedRamdiskText(
        bytes = byteOrderMark.bytes + encoded,
        byteOrderMark = byteOrderMark,
    )
}

private fun charsetFor(encoding: TextFileEncoding): Charset =
    try {
        Charset.forName(encoding.charsetName)
    } catch (error: IllegalArgumentException) {
        throw IOException("${encoding.displayName} is not supported on this device", error)
    }

private fun ByteBuffer.toByteArray(): ByteArray =
    ByteArray(remaining()).also(::get)

private fun detectUnicodeByteOrderMark(bytes: ByteArray): UnicodeByteOrderMark? =
    UnicodeByteOrderMark.entries.firstOrNull { byteOrderMark ->
        bytes.startsWith(byteOrderMark.bytes)
    }

private fun ByteArray.startsWith(prefix: ByteArray): Boolean {
    if (size < prefix.size) return false
    return prefix.indices.all { index -> this[index] == prefix[index] }
}

private fun UnicodeByteOrderMark.isCompatibleWith(encoding: TextFileEncoding): Boolean =
    canonicalEncoding == encoding ||
        this == UnicodeByteOrderMark.UTF_8 &&
        encoding == TextFileEncoding.UTF_8

private fun detectBomlessUnicodeEncoding(bytes: ByteArray): TextFileEncoding? {
    val sampleSize = min(bytes.size, UNICODE_DETECTION_SAMPLE_SIZE)
    if (sampleSize >= 4 && bytes.size % 4 == 0) {
        val zeroRatios = bytes.zeroRatios(sampleSize, laneCount = 4)
        if (
            zeroRatios[0] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[1] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[2] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[3] <= DATA_LANE_ZERO_RATIO
        ) {
            return TextFileEncoding.UTF_32_BE
        }
        if (
            zeroRatios[0] <= DATA_LANE_ZERO_RATIO &&
            zeroRatios[1] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[2] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[3] >= EXPECTED_ZERO_LANE_RATIO
        ) {
            return TextFileEncoding.UTF_32_LE
        }
    }

    if (sampleSize >= 2 && bytes.size % 2 == 0) {
        val zeroRatios = bytes.zeroRatios(sampleSize, laneCount = 2)
        if (
            zeroRatios[0] >= EXPECTED_ZERO_LANE_RATIO &&
            zeroRatios[1] <= DATA_LANE_ZERO_RATIO
        ) {
            return TextFileEncoding.UTF_16_BE
        }
        if (
            zeroRatios[0] <= DATA_LANE_ZERO_RATIO &&
            zeroRatios[1] >= EXPECTED_ZERO_LANE_RATIO
        ) {
            return TextFileEncoding.UTF_16_LE
        }
    }
    return null
}

private fun ByteArray.zeroRatios(sampleSize: Int, laneCount: Int): DoubleArray {
    val zeroCounts = IntArray(laneCount)
    val laneCounts = IntArray(laneCount)
    repeat(sampleSize) { index ->
        val lane = index % laneCount
        laneCounts[lane] += 1
        if (this[index] == 0.toByte()) {
            zeroCounts[lane] += 1
        }
    }
    return DoubleArray(laneCount) { lane ->
        if (laneCounts[lane] == 0) 0.0 else zeroCounts[lane].toDouble() / laneCounts[lane]
    }
}

private fun String.isPlausibleAutomaticallyDetectedText(): Boolean =
    none { character ->
        character == '\u0000' ||
            character.code < 0x20 && character !in AUTOMATIC_TEXT_CONTROL_CHARACTERS ||
            character.code in 0x7f..0x9f
    }

private fun hasHigherConfidenceUnicodeInterpretation(
    bytes: ByteArray,
    utf8Text: String,
): Boolean {
    val utf8Score = utf8Text.automaticTextQuality()
    return supportedRamdiskTextEncodings
        .asSequence()
        .filter { encoding ->
            encoding != TextFileEncoding.UTF_8 &&
                encoding != TextFileEncoding.UTF_8_BOM &&
                when (encoding) {
                    TextFileEncoding.UTF_16_BE,
                    TextFileEncoding.UTF_16_LE,
                    -> bytes.size % 2 == 0

                    TextFileEncoding.UTF_32_BE,
                    TextFileEncoding.UTF_32_LE,
                    -> bytes.size % 4 == 0
                }
        }
        .mapNotNull { encoding ->
            runCatching {
                decodeRamdiskText(bytes, encoding).text
            }.getOrNull()
        }
        .filter { alternative ->
            alternative != utf8Text &&
                alternative.any { it.code > ASCII_MAX_CODE_POINT } &&
                alternative.isPlausibleAutomaticallyDetectedText()
        }
        .any { alternative ->
            alternative.automaticTextQuality() - utf8Score >=
                AMBIGUOUS_TEXT_QUALITY_MARGIN
        }
}

private fun String.automaticTextQuality(): Double {
    if (isEmpty()) return 1.0
    val total = sumOf { character ->
        when {
            character.isLetterOrDigit() || character.isWhitespace() -> 1.0
            character in COMMON_TEXT_PUNCTUATION -> 0.6
            character.category in PUNCTUATION_CATEGORIES -> 0.35
            character.category in SYMBOL_CATEGORIES -> 0.25
            else -> 0.0
        }
    }
    return total / length
}

private val AUTOMATIC_TEXT_CONTROL_CHARACTERS = setOf('\t', '\n', '\r')
private val COMMON_TEXT_PUNCTUATION = setOf(
    '.',
    ',',
    ';',
    ':',
    '!',
    '?',
    '-',
    '_',
    '/',
    '\\',
    '=',
    '+',
    '#',
    '@',
    '$',
    '%',
    '&',
    '*',
    '"',
    '\'',
)
private val PUNCTUATION_CATEGORIES = setOf(
    CharCategory.CONNECTOR_PUNCTUATION,
    CharCategory.DASH_PUNCTUATION,
    CharCategory.START_PUNCTUATION,
    CharCategory.END_PUNCTUATION,
    CharCategory.INITIAL_QUOTE_PUNCTUATION,
    CharCategory.FINAL_QUOTE_PUNCTUATION,
    CharCategory.OTHER_PUNCTUATION,
)
private val SYMBOL_CATEGORIES = setOf(
    CharCategory.MATH_SYMBOL,
    CharCategory.CURRENCY_SYMBOL,
    CharCategory.MODIFIER_SYMBOL,
    CharCategory.OTHER_SYMBOL,
)
private const val UNICODE_DETECTION_SAMPLE_SIZE = 4 * 1024
private const val EXPECTED_ZERO_LANE_RATIO = 0.60
private const val DATA_LANE_ZERO_RATIO = 0.20
private const val ASCII_MAX_CODE_POINT = 0x7f
private const val AMBIGUOUS_TEXT_QUALITY_MARGIN = 0.15
internal const val MAX_RAMDISK_TEXT_FILE_SIZE = 2 * 1024 * 1024
