package ui.screen.feature

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

internal enum class SuCompatMode { OFF, TRADITIONAL, KSM }

internal data class SuCompatSnapshot(
    val traditional: Boolean = true,
    val ksm: Boolean = false,
    val magisk: Boolean = false,
) {
    val mode: SuCompatMode
        get() = when {
            ksm -> SuCompatMode.KSM
            traditional -> SuCompatMode.TRADITIONAL
            else -> SuCompatMode.OFF
        }

    fun matches(mode: SuCompatMode): Boolean =
        traditional == (mode == SuCompatMode.TRADITIONAL) &&
            ksm == (mode == SuCompatMode.KSM) &&
            (mode == SuCompatMode.KSM || !magisk)
}

internal class SuCompatController(
    val read: () -> SuCompatSnapshot,
    private val write: suspend (String, Boolean) -> Boolean,
) {
    private val mutex = Mutex()

    suspend fun select(mode: SuCompatMode): Boolean = mutex.withLock {
        transaction { applyMode(mode) && read().matches(mode) }
    }

    suspend fun setMagisk(enabled: Boolean): Boolean = mutex.withLock {
        if (!read().matches(SuCompatMode.KSM)) return@withLock false
        transaction {
            write("magisk_compat", enabled) &&
                read().let { it.matches(SuCompatMode.KSM) && it.magisk == enabled }
        }
    }

    private suspend fun applyMode(mode: SuCompatMode): Boolean {
        if (mode != SuCompatMode.KSM && read().magisk && !write("magisk_compat", false)) {
            return false
        }
        if (read().matches(mode)) return true
        return when (mode) {
            SuCompatMode.TRADITIONAL -> write("su_compat", true)
            SuCompatMode.KSM -> write("kasumi_sucompat", true)
            SuCompatMode.OFF -> {
                if (read().ksm && !write("kasumi_sucompat", false)) return false
                !read().traditional || write("su_compat", false)
            }
        }
    }

    private suspend fun transaction(update: suspend () -> Boolean): Boolean {
        val previous = read()
        if (runCatching { update() }.getOrDefault(false)) return true
        // set-save persists both mutually exclusive features, including rollback.
        runCatching {
            if (read().magisk && !previous.magisk) write("magisk_compat", false)
            if (applyMode(previous.mode) && read().magisk != previous.magisk) {
                write("magisk_compat", previous.magisk)
            }
        }
        return false
    }
}
