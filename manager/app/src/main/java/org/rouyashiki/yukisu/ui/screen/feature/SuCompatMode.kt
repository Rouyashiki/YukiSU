package org.rouyashiki.yukisu.ui.screen.feature

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

internal enum class SuCompatMode { OFF, TRADITIONAL, KSM }

internal data class SuCompatSnapshot(
    val traditional: Boolean?,
    val ksm: Boolean?,
    val magisk: Boolean?,
) {
    val mode: SuCompatMode?
        get() = when {
            traditional == null || ksm == null -> null
            ksm -> SuCompatMode.KSM
            traditional -> SuCompatMode.TRADITIONAL
            else -> SuCompatMode.OFF
        }

    fun matches(target: SuCompatMode, expectedMagisk: Boolean? = null): Boolean? {
        val wantMagisk = expectedMagisk ?: false.takeIf { target != SuCompatMode.KSM }
        if (traditional != null && traditional != (target == SuCompatMode.TRADITIONAL)) return false
        if (ksm != null && ksm != (target == SuCompatMode.KSM)) return false
        if (wantMagisk != null && magisk != null && magisk != wantMagisk) return false
        if (traditional == null || ksm == null || (wantMagisk != null && magisk == null)) return null
        return true
    }
}

internal class SuCompatController(
    private val write: suspend (String, Boolean) -> Boolean,
) {
    private val mutex = Mutex()

    suspend fun change(
        mode: SuCompatMode,
        magisk: Boolean?,
        read: () -> SuCompatSnapshot,
    ): Pair<Boolean, SuCompatSnapshot> = mutex.withLock {
        val previous = read()
        val previousMode = previous.mode ?: return@withLock false to previous
        val previousMagisk = previous.magisk ?: return@withLock false to previous
        if (magisk != null && previous.matches(SuCompatMode.KSM) != true) return@withLock false to previous

        val saved = runCatching {
            if (magisk != null) write("magisk_compat", magisk) else applyMode(mode, previous, read)
        }.getOrDefault(false)
        val current = read()
        // A missing readback cannot disprove an acknowledged write.
        if (saved && current.matches(mode, magisk) != false) return@withLock true to current

        // set-save persists both mutually exclusive features, including rollback.
        runCatching {
            if (applyMode(previousMode, current, read) && read().magisk != previousMagisk) {
                write("magisk_compat", previousMagisk)
            }
        }
        false to read()
    }

    private suspend fun applyMode(
        mode: SuCompatMode,
        initial: SuCompatSnapshot,
        read: () -> SuCompatSnapshot,
    ): Boolean {
        var current = initial
        if (mode != SuCompatMode.KSM && current.magisk != false) {
            if (!write("magisk_compat", false)) return false
            current = read()
        }
        if (current.matches(mode) == true) return true
        return when (mode) {
            SuCompatMode.TRADITIONAL -> write("su_compat", true)
            SuCompatMode.KSM -> write("kasumi_sucompat", true)
            SuCompatMode.OFF -> {
                if (current.ksm != false) {
                    if (!write("kasumi_sucompat", false)) return false
                    current = read()
                }
                current.traditional == false || write("su_compat", false)
            }
        }
    }
}
