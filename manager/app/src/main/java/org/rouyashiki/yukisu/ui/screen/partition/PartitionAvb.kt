package org.rouyashiki.yukisu.ui.screen.partition

import java.io.IOException

internal fun isAvbControlPartition(name: String): Boolean =
    name == "vbmeta" || name == "vbmeta_a" || name == "vbmeta_b"

internal enum class AvbBootloaderState {
    Locked, Unlocked, Unknown,
}

internal enum class AvbFeature(val commandName: String) {
    Verification("verification"), Verity("verity"),
}

internal data class AvbChange(val feature: AvbFeature, val enabled: Boolean) {
    val command: String
        get() = "${if (enabled) "enable" else "disable"}-${feature.commandName}"
}

internal data class PartitionAvbStatus(
    val flags: Long,
    val bootloader: AvbBootloaderState,
) {
    val verityEnabled: Boolean get() = flags and 1L == 0L
    val verificationEnabled: Boolean get() = flags and 2L == 0L

    fun matches(change: AvbChange): Boolean = when (change.feature) {
        AvbFeature.Verification -> verificationEnabled == change.enabled
        AvbFeature.Verity -> verityEnabled == change.enabled
    }
}

internal fun parsePartitionAvbStatus(lines: List<String>, target: String): PartitionAvbStatus {
    fun field(name: String): String {
        val prefix = "$name: "
        return lines.filter { it.startsWith(prefix) }.singleOrNull()
            ?.removePrefix(prefix)
            ?: throw IOException("Missing or ambiguous avbctl field: $name")
    }

    if (field("Target") != target) throw IOException("avbctl returned a different partition")
    val flagsText = field("Stored flags")
    val flags = flagsText.takeIf { it.matches(Regex("0x[0-9a-fA-F]{8}")) }
        ?.removePrefix("0x")?.toLongOrNull(16)
        ?: throw IOException("Invalid vbmeta flags")
    val bootloader = when (field("Bootloader")) {
        "locked" -> AvbBootloaderState.Locked
        "unlocked" -> AvbBootloaderState.Unlocked
        "unknown" -> AvbBootloaderState.Unknown
        else -> throw IOException("Invalid bootloader state")
    }
    val status = PartitionAvbStatus(flags, bootloader)
    if (field("Verity flag") != if (status.verityEnabled) "enabled" else "disabled") {
        throw IOException("Inconsistent dm-verity flag")
    }
    if (field("Verification flag") != if (status.verificationEnabled) "enabled" else "disabled") {
        throw IOException("Inconsistent AVB verification flag")
    }
    return status
}
