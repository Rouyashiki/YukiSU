package org.rouyashiki.yukisu.ui.util

import android.os.Parcelable
import kotlinx.parcelize.Parcelize

val bootForceFeatureNames = listOf(
    "su_compat", "kernel_umount", "sulog", "adb_root", "selinux_hide", "shell_su_adb_only",
)

@Parcelize
data class BootConfigOptions(
    val noInstall: Boolean = false,
    val noPreinit: Boolean = false,
    val noCustomRc: Boolean = false,
    val forcedFeatures: List<String> = emptyList(),
) : Parcelable {
    fun commandOptions(directLkm: Boolean = false): String? {
        if (noInstall && directLkm) return null
        val names = mutableSetOf<String>()
        for (entry in forcedFeatures) {
            val parts = entry.split('=')
            if (parts.size != 2 || parts[0] !in bootForceFeatureNames ||
                parts[1] !in listOf("0", "1") || !names.add(parts[0])
            ) return null
        }
        return buildString {
            if (noInstall) append(" --no-install")
            if (noPreinit) append(" --no-preinit")
            if (noCustomRc) append(" --no-custom-rc")
            forcedFeatures.forEach { append(" --force-feature $it") }
        }
    }
}
