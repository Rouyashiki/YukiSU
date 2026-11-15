package org.rouyashiki.yukisu.ui.activity.util

import android.content.Intent
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.ui.component.ZipFileDetector
import org.rouyashiki.yukisu.ui.component.ZipFileInfo
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update

internal class IntentQueue<T : Any>(
    restored: List<T>? = null,
    private val save: (List<T>) -> Unit = {},
) {
    private val ready = Channel<Unit>(Channel.CONFLATED)
    private val queued = ArrayDeque(restored.orEmpty())
    private var pending: T? = null
    private var initialized = restored != null

    fun initialize(value: T?) {
        if (initialized) return
        initialized = true
        if (value != null) add(value) else save(emptyList())
    }

    fun add(value: T) {
        queued.addLast(value)
        initialized = true
        save(listOfNotNull(pending) + queued)
        ready.trySend(Unit).getOrThrow()
    }

    suspend fun next(): T {
        pending?.let { return it }
        // Keep payloads outside the channel so cancellation during receive cannot discard them.
        while (queued.isEmpty()) ready.receive()
        return queued.removeFirst().also { pending = it }
    }

    fun complete() {
        if (pending == null) return
        pending = null
        save(queued.toList())
    }

    fun close() = ready.close()
}

internal class ManagerAccessGate(private val isManager: () -> Boolean) {
    private val changes = MutableStateFlow(0L)

    fun notifyChanged() {
        changes.update { it + 1 }
    }

    suspend fun awaitReady() {
        changes.first { isManager() }
    }
}

class IntentDispatcher(savedStateHandle: SavedStateHandle) : ViewModel() {
    internal val queue = IntentQueue<Intent>(savedStateHandle["pending_intents"]) {
        savedStateHandle["pending_intents"] = ArrayList(it)
    }
    internal val managerAccess = ManagerAccessGate { Natives.isManager }
    var zipFiles by mutableStateOf<List<ZipFileInfo>>(emptyList())
    var showConfirmation by mutableStateOf(false)

    fun initialize(intent: Intent?) = queue.initialize(intent?.let(::Intent))

    fun submit(intent: Intent) = queue.add(Intent(intent))

    override fun onCleared() {
        queue.close()
        ZipFileDetector.cleanupStagedFiles(zipFiles)
    }
}
