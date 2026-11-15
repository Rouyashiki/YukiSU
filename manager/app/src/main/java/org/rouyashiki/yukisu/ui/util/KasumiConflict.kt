package org.rouyashiki.yukisu.ui.util

import androidx.annotation.Keep
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

@Keep
data class KasumiConflict(val sequence: Long, val kind: Int, val target: String) {
    companion object {
        const val LOADED_MODULE = 1
        const val BUILTIN_CONFIG = 2
        const val MODULE_LOAD = 3
    }
}

data class KasumiConflictWarningState(
    val events: List<KasumiConflict> = emptyList(),
    val confirming: Boolean = false,
    val confirmationFailed: Boolean = false,
)

class KasumiConflictController(
    private val read: suspend () -> List<KasumiConflict>?,
    private val acknowledge: suspend (Long) -> Boolean,
) {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(KasumiConflictWarningState())
    val state = mutableState.asStateFlow()

    suspend fun refresh() = mutex.withLock {
        if (mutableState.value.events.isNotEmpty()) return@withLock
        val events = read() ?: return@withLock
        mutableState.value = KasumiConflictWarningState(events.sortedBy { it.sequence })
    }

    suspend fun confirm() = mutex.withLock {
        val snapshot = mutableState.value
        if (snapshot.events.isEmpty()) return@withLock
        mutableState.value = snapshot.copy(confirming = true, confirmationFailed = false)
        try {
            mutableState.value = if (acknowledge(snapshot.events.maxOf { it.sequence })) {
                KasumiConflictWarningState()
            } else {
                snapshot.copy(confirmationFailed = true)
            }
        } finally {
            mutableState.value = mutableState.value.copy(confirming = false)
        }
    }
}
