package org.rouyashiki.yukisu.ui.util

import android.content.Context
import org.rouyashiki.yukisu.R
import com.topjohnwu.superuser.io.SuFile

fun getSELinuxStatus(context: Context) = SuFile("/sys/fs/selinux/enforce").run {
    when {
        !exists() -> context.getString(R.string.selinux_status_disabled)
        !isFile -> context.getString(R.string.selinux_status_unknown)
        !canRead() -> context.getString(R.string.selinux_status_enforcing)
        else -> when (runCatching { newInputStream() }.getOrNull()?.bufferedReader()
            ?.use { it.runCatching { readLine() }.getOrNull()?.trim()?.toIntOrNull() }) {
            1 -> context.getString(R.string.selinux_status_enforcing)
            0 -> context.getString(R.string.selinux_status_permissive)
            else -> context.getString(R.string.selinux_status_unknown)
        }
    }
}

/** True iff /sys/fs/selinux/enforce reads as 1. Cheaper than getSELinuxStatus
 *  when the caller only wants a boolean and doesn't care about i18n labels. */
fun isSELinuxEnforcing(): Boolean = readSELinuxEnforce() == 1

/** Raw SELinux mode label (Enforcing / Permissive / Disabled / Unknown).
 *  Equivalent to `getenforce` but avoids spawning a shell. */
fun getSELinuxLabel(): String = when (readSELinuxEnforce()) {
    1 -> "Enforcing"
    0 -> "Permissive"
    -1 -> "Disabled"
    else -> "Unknown"
}

/** Returns 1/0 from /sys/fs/selinux/enforce, -1 if missing, null if unreadable. */
private fun readSELinuxEnforce(): Int? = SuFile("/sys/fs/selinux/enforce").run {
    if (!exists()) return@run -1
    runCatching {
        newInputStream().bufferedReader().use { it.readLine()?.trim()?.toIntOrNull() }
    }.getOrNull()
}