package com.anatdx.yukisu.ui.viewmodel

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext

internal class ModuleUpdateState(
    private val scope: CoroutineScope,
    private val dispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val checkUpdate: suspend (ModuleViewModel.ModuleInfo) -> ModuleUpdateInfo,
) {
    private data class Signature(val id: String, val versionCode: Int, val updateJson: String)
    private data class Entry(val signature: Signature, val info: ModuleUpdateInfo)

    private val cache = mutableMapOf<String, Entry>()
    private val requestSlots = Semaphore(4)
    private var refreshJob: Job? = null

    var updateInfo by mutableStateOf<Map<String, ModuleUpdateInfo>>(emptyMap())
        private set

    val availableCount: Int
        get() = updateInfo.count { it.value.zipUrl.isNotBlank() }

    fun refresh(modules: List<ModuleViewModel.ModuleInfo>, force: Boolean = false) {
        refreshJob?.cancel()
        val candidates = modules.filter {
            it.enabled && !it.remove && !it.update && it.updateJson.isNotBlank()
        }
        val signatures = candidates.associate { module ->
            module.dirId to Signature(module.id, module.versionCode, module.updateJson)
        }
        cache.entries.removeAll { (id, entry) -> signatures[id] != entry.signature }
        updateInfo = cache.mapValues { it.value.info }

        refreshJob = scope.launch {
            candidates.filter { force || it.dirId !in cache }.forEach { module ->
                launch {
                    val info = try {
                        requestSlots.withPermit {
                            withContext(dispatcher) { checkUpdate(module) }
                        }
                    } catch (e: CancellationException) {
                        throw e
                    } catch (_: Exception) {
                        ModuleUpdateInfo()
                    }
                    ensureActive()
                    cache[module.dirId] = Entry(signatures.getValue(module.dirId), info)
                    updateInfo = updateInfo + (module.dirId to info)
                }
            }
        }
    }
}
