package org.rouyashiki.yukisu.ui.viewmodel

import android.os.Build
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.data.repository.CompatibilityResult
import org.rouyashiki.yukisu.data.repository.CompatibilityStatus
import org.rouyashiki.yukisu.data.repository.InstalledModuleState
import org.rouyashiki.yukisu.data.repository.ModuleRepositoryProvider
import org.rouyashiki.yukisu.data.repository.MmrlRepositoryDirectoryEntry
import org.rouyashiki.yukisu.data.repository.RepositoryFormat
import org.rouyashiki.yukisu.data.repository.RepositoryModule
import org.rouyashiki.yukisu.data.repository.RepositorySource
import org.rouyashiki.yukisu.ui.util.listModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray

class ModuleRepositoryViewModel : ViewModel() {
    private val manager = ModuleRepositoryProvider.get()

    val sources: StateFlow<List<RepositorySource>> = manager.sources
    val catalog: StateFlow<List<RepositoryModule>> = manager.catalog
    val bindings = manager.bindings
    val mmrlDirectory = manager.mmrlDirectory

    var search by mutableStateOf("")
    var selectedSourceId by mutableStateOf<String?>(null)
    var isRefreshing by mutableStateOf(false)
        private set
    var isAddingSource by mutableStateOf(false)
        private set
    var isRefreshingMmrlDirectory by mutableStateOf(false)
        private set
    var addingMmrlRepositoryUrl by mutableStateOf<String?>(null)
        private set
    var isLoadingDetails by mutableStateOf(false)
        private set
    var detailError by mutableStateOf<String?>(null)
        private set
    var installedModules by mutableStateOf<Map<String, InstalledModuleState>>(emptyMap())
        private set

    init {
        refreshInstalledModules()
    }

    fun visibleModules(catalog: List<RepositoryModule>): List<RepositoryModule> {
        val query = search.trim()
        return catalog.asSequence()
            .filter { selectedSourceId == null || it.sourceId == selectedSourceId }
            .filter {
                query.isEmpty() ||
                    it.moduleId.contains(query, ignoreCase = true) ||
                    it.name.contains(query, ignoreCase = true) ||
                    it.author.contains(query, ignoreCase = true) ||
                    it.description.contains(query, ignoreCase = true)
            }
            .sortedWith(
                compareByDescending<RepositoryModule> { !it.isScript && installedModules.containsKey(it.moduleId) }
                    .thenByDescending { it.updatedAt ?: it.declaredArtifact()?.timestamp ?: 0L }
                    .thenBy(String.CASE_INSENSITIVE_ORDER) { it.name }
                    .thenBy { it.sourceId }
            )
            .toList()
    }

    fun refreshAll() {
        if (isRefreshing) return
        viewModelScope.launch {
            isRefreshing = true
            manager.refreshAll()
            refreshInstalledModulesInternal()
            isRefreshing = false
        }
    }

    fun refreshSource(sourceId: String) {
        if (isRefreshing) return
        viewModelScope.launch {
            isRefreshing = true
            manager.refreshSource(sourceId)
            isRefreshing = false
        }
    }

    fun addSource(
        name: String,
        url: String,
        format: RepositoryFormat,
        onResult: (Result<RepositorySource>) -> Unit,
    ) {
        if (isAddingSource) return
        viewModelScope.launch {
            isAddingSource = true
            val result = runCatching { manager.addSource(name, url, format) }
            isAddingSource = false
            onResult(result)
        }
    }

    fun refreshMmrlDirectory() {
        if (isRefreshingMmrlDirectory) return
        viewModelScope.launch {
            isRefreshingMmrlDirectory = true
            manager.refreshMmrlDirectory()
            isRefreshingMmrlDirectory = false
        }
    }

    fun addMmrlRepository(
        entry: MmrlRepositoryDirectoryEntry,
        onResult: (Result<RepositorySource>) -> Unit,
    ) {
        if (isAddingSource) return
        addingMmrlRepositoryUrl = entry.url
        addSource(entry.name, entry.url, RepositoryFormat.MMRL) { result ->
            addingMmrlRepositoryUrl = null
            onResult(result)
        }
    }

    fun setSourceEnabled(sourceId: String, enabled: Boolean) {
        viewModelScope.launch { manager.setSourceEnabled(sourceId, enabled) }
    }

    fun moveSource(sourceId: String, direction: Int) {
        viewModelScope.launch { manager.moveSource(sourceId, direction) }
    }

    fun removeSource(sourceId: String, onResult: (Result<Unit>) -> Unit = {}) {
        viewModelScope.launch {
            onResult(runCatching { manager.removeSource(sourceId) })
        }
    }

    fun setPinned(moduleId: String, versionCode: Long?) {
        viewModelScope.launch { manager.setPinnedVersion(moduleId, versionCode) }
    }

    fun bindInstalledModule(sourceId: String, installed: InstalledModuleState) {
        viewModelScope.launch {
            manager.recordInstalledBinding(
                moduleId = installed.moduleId,
                sourceId = sourceId,
                version = installed.version,
                versionCode = installed.versionCode,
            )
        }
    }

    fun module(sourceId: String, moduleId: String): RepositoryModule? =
        manager.module(sourceId, moduleId)

    fun modulesById(moduleId: String): List<RepositoryModule> = manager.modulesById(moduleId)

    fun source(sourceId: String): RepositorySource? = sources.value.firstOrNull { it.id == sourceId }

    fun sourceForUrl(url: String): RepositorySource? = manager.sourceForUrl(url)

    fun moduleCount(sourceId: String): Int = manager.moduleCount(sourceId)

    fun loadModuleDetails(sourceId: String, moduleId: String) {
        if (isLoadingDetails) return
        viewModelScope.launch {
            isLoadingDetails = true
            detailError = null
            try {
                detailError = manager.loadModuleDetails(sourceId, moduleId).exceptionOrNull()?.message
            } finally {
                isLoadingDetails = false
            }
        }
    }

    fun compatibility(module: RepositoryModule): CompatibilityResult {
        val value = module.compatibility
        val constraintsPresent = value.minApi != null || value.maxApi != null ||
            value.architectures.isNotEmpty() || value.devices.isNotEmpty() ||
            value.minKernelSuVersion != null
        val reasons = mutableListOf<String>()
        var hasUnknownConstraint = false
        value.minApi?.takeIf { Build.VERSION.SDK_INT < it }?.let {
            reasons += "Requires Android API $it or newer"
        }
        value.maxApi?.takeIf { Build.VERSION.SDK_INT > it }?.let {
            reasons += "Supports Android API $it or older"
        }
        if (value.architectures.isNotEmpty() &&
            Build.SUPPORTED_ABIS.none { abi -> value.architectures.any { it.equals(abi, true) } }
        ) {
            reasons += "Unsupported architecture: ${Build.SUPPORTED_ABIS.joinToString()}"
        }
        if (value.devices.isNotEmpty() && value.devices.none { it.equals(Build.MODEL, true) }) {
            reasons += "Unsupported device: ${Build.MODEL}"
        }
        val kernelVersion = runCatching { Natives.version.toLong() }.getOrNull()
        value.minKernelSuVersion?.let { required ->
            if (kernelVersion == null) {
                hasUnknownConstraint = true
            } else if (kernelVersion < required) {
                reasons += "Requires KernelSU $required or newer"
            }
        }
        return when {
            reasons.isNotEmpty() -> CompatibilityResult(CompatibilityStatus.INCOMPATIBLE, reasons)
            hasUnknownConstraint -> CompatibilityResult(CompatibilityStatus.UNKNOWN)
            constraintsPresent -> CompatibilityResult(CompatibilityStatus.COMPATIBLE)
            else -> CompatibilityResult(CompatibilityStatus.UNKNOWN)
        }
    }

    fun duplicateCount(moduleId: String, catalog: List<RepositoryModule>): Int =
        catalog.count { it.moduleId == moduleId }

    fun refreshInstalledModules() {
        viewModelScope.launch { refreshInstalledModulesInternal() }
    }

    private suspend fun refreshInstalledModulesInternal() {
        val installedResult = withContext(Dispatchers.IO) {
            runCatching {
                val array = JSONArray(listModules())
                buildMap {
                    for (index in 0 until array.length()) {
                        val item = array.optJSONObject(index) ?: continue
                        val id = item.optString("id").takeIf(String::isNotBlank) ?: continue
                        val rawVersionCode = item.opt("versionCode")
                        val versionCode = if (rawVersionCode is Number) {
                            rawVersionCode.toLong()
                        } else if (rawVersionCode is String) {
                            rawVersionCode.toLongOrNull() ?: 0L
                        } else {
                            0L
                        }
                        put(
                            id,
                            InstalledModuleState(
                                moduleId = id,
                                name = item.optString("name", id),
                                version = item.optString("version"),
                                versionCode = versionCode,
                            )
                        )
                    }
                }
            }
        }
        installedResult.onSuccess { installed ->
            installedModules = installed
            manager.reconcileBindings(installed.keys)
        }
    }
}
