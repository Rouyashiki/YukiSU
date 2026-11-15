package org.rouyashiki.yukisu.ui.viewmodel

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import org.rouyashiki.yukisu.data.repository.PluginRepositoryProvider
import org.rouyashiki.yukisu.data.repository.PluginRepositorySource
import org.rouyashiki.yukisu.data.repository.RepositoryPlugin
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class PluginRepositoryViewModel : ViewModel() {
    private val manager = PluginRepositoryProvider.get()

    val sources: StateFlow<List<PluginRepositorySource>> = manager.sources
    val catalog: StateFlow<List<RepositoryPlugin>> = manager.catalog

    var search by mutableStateOf("")
    var selectedSourceId by mutableStateOf<String?>(null)
    var isRefreshing by mutableStateOf(false)
        private set
    var isAddingSource by mutableStateOf(false)
        private set

    fun visiblePlugins(catalog: List<RepositoryPlugin>): List<RepositoryPlugin> {
        val query = search.trim()
        return catalog.asSequence()
            .filter { selectedSourceId == null || it.sourceId == selectedSourceId }
            .filter { plugin ->
                query.isEmpty() || sequenceOf(
                    plugin.pluginId.orEmpty(),
                    plugin.name,
                    plugin.author,
                    plugin.version,
                    plugin.description,
                ).plus(plugin.descriptions.values.asSequence())
                    .any { it.contains(query, ignoreCase = true) }
            }
            .sortedWith(
                compareBy(String.CASE_INSENSITIVE_ORDER, RepositoryPlugin::name)
                    .thenBy(RepositoryPlugin::sourceId)
            )
            .toList()
    }

    fun refreshAll() {
        if (isRefreshing) return
        viewModelScope.launch {
            isRefreshing = true
            manager.refreshAll()
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
        onResult: (Result<PluginRepositorySource>) -> Unit,
    ) {
        if (isAddingSource) return
        viewModelScope.launch {
            isAddingSource = true
            val result = runCatching { manager.addSource(name, url) }
            isAddingSource = false
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

    fun pluginCount(sourceId: String): Int = manager.pluginCount(sourceId)
}
