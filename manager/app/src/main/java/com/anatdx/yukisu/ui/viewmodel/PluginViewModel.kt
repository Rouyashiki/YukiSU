package com.anatdx.yukisu.ui.viewmodel

import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.anatdx.yukisu.ui.util.PluginCommandResult
import com.anatdx.yukisu.ui.util.clearPluginLog
import com.anatdx.yukisu.ui.util.deletePluginConfig
import com.anatdx.yukisu.ui.util.getPluginLog
import com.anatdx.yukisu.ui.util.installPluginZip
import com.anatdx.yukisu.ui.util.listPluginConfig
import com.anatdx.yukisu.ui.util.listPlugins
import com.anatdx.yukisu.ui.util.runPluginAction
import com.anatdx.yukisu.ui.util.runPluginCallback
import com.anatdx.yukisu.ui.util.savePluginConfig
import com.anatdx.yukisu.ui.util.togglePlugin
import com.anatdx.yukisu.ui.util.uninstallPlugin
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

data class PluginConfigField(
    val key: String,
    val label: String,
    val labels: Map<String, String>,
    val type: String,
    val default: String,
    val options: List<String>,
)

data class PluginQuickAction(
    val function: String,
    val label: String,
    val labels: Map<String, String>,
)

data class PluginInfo(
    val id: String,
    val name: String,
    val author: String,
    val version: String,
    val description: String,
    val descriptions: Map<String, String>,
    val license: String,
    val enabled: Boolean,
    val hasManifest: Boolean,
    val hasAction: Boolean,
    val actionUnknown: Boolean,
    val callbacks: List<String>,
    val quickAction: PluginQuickAction?,
    val config: List<PluginConfigField>,
    val depends: List<String>,
    val error: String,
)

class PluginViewModel : ViewModel() {

    companion object {
        private const val TAG = "PluginViewModel"
    }

    var plugins by mutableStateOf<List<PluginInfo>>(emptyList())
        private set

    var isRefreshing by mutableStateOf(false)
        private set

    var errorMessage by mutableStateOf<String?>(null)
        private set

    private var refreshJob: Job? = null
    private var refreshPending = false
    private val configSaveMutex = Mutex()

    fun fetchPlugins() {
        refreshPending = true
        if (refreshJob?.isActive == true) return
        refreshJob = viewModelScope.launch {
            isRefreshing = true
            try {
                while (refreshPending) {
                    refreshPending = false
                    errorMessage = null
                    val result = withContext(Dispatchers.IO) { listPlugins() }
                    if (!result.isSuccess) {
                        errorMessage = result.output.ifBlank {
                            "plugin list exited with ${result.exitCode}"
                        }
                        continue
                    }
                    if (result.stdout.isBlank()) {
                        errorMessage = "plugin list returned no JSON"
                        continue
                    }

                    runCatching { parsePlugins(result.stdout.trim()) }
                        .onSuccess { plugins = it }
                        .onFailure { error ->
                            Log.e(TAG, "Failed to parse plugin list", error)
                            errorMessage = error.message ?: "Invalid plugin list"
                        }
                }
            } finally {
                isRefreshing = false
            }
        }
    }

    private suspend fun reconcilePlugins() {
        fetchPlugins()
        refreshJob?.join()
    }

    suspend fun setPluginEnabled(id: String, enabled: Boolean): Boolean =
        withContext(NonCancellable) {
            val success = withContext(Dispatchers.IO) { togglePlugin(id, enabled) }
            if (success) {
                plugins = plugins.map { plugin ->
                    if (plugin.id == id) plugin.copy(enabled = enabled) else plugin
                }
            }
            reconcilePlugins()
            success
        }

    suspend fun removePlugin(id: String): Boolean = withContext(NonCancellable) {
        val success = withContext(Dispatchers.IO) { uninstallPlugin(id) }
        if (success) {
            plugins = plugins.filterNot { it.id == id }
        }
        reconcilePlugins()
        success
    }

    suspend fun installPlugin(zipPath: String): PluginCommandResult =
        withContext(NonCancellable) {
            val result = withContext(Dispatchers.IO) { installPluginZip(zipPath) }
            reconcilePlugins()
            result
        }

    suspend fun runCallback(id: String, function: String): PluginCommandResult =
        withContext(Dispatchers.IO) { runPluginCallback(id, function) }

    suspend fun runAction(id: String): PluginCommandResult =
        withContext(Dispatchers.IO) { runPluginAction(id) }

    suspend fun fetchLog(id: String): PluginCommandResult =
        withContext(Dispatchers.IO) { getPluginLog(id) }

    suspend fun clearLog(id: String): Boolean =
        withContext(Dispatchers.IO) { clearPluginLog(id) }

    suspend fun loadConfigValues(plugin: PluginInfo): Map<String, String>? =
        withContext(Dispatchers.IO) {
            val result = listPluginConfig(plugin.id)
            if (!result.isSuccess) return@withContext null

            val savedValues = runCatching {
                JSONObject(result.stdout.ifBlank { "{}" })
            }.getOrNull() ?: return@withContext null
            plugin.config.filter { savedValues.has(it.key) }.associate { field ->
                field.key to if (savedValues.isNull(field.key)) "" else savedValues.optString(field.key, "")
            }
        }

    suspend fun saveConfigValues(plugin: PluginInfo, changes: PluginConfigChanges): PluginConfigSaveResult =
        configSaveMutex.withLock {
            withContext(NonCancellable + Dispatchers.IO) {
                val failed = applyPluginConfigChanges(
                    changes,
                    delete = { deletePluginConfig(plugin.id, it) },
                    write = { key, value -> savePluginConfig(plugin.id, key, value) },
                )
                PluginConfigSaveResult(
                    failedKeys = failed,
                    reloadedValues = if (failed.isEmpty()) null else loadConfigValues(plugin),
                )
            }
        }

    internal fun parsePlugins(raw: String): List<PluginInfo> {
        val array = JSONArray(raw)
        return buildList {
            for (index in 0 until array.length()) {
                val item = array.optJSONObject(index) ?: continue
                val id = item.optString("id", "").trim()
                if (id.isEmpty()) continue

                add(
                    PluginInfo(
                        id = id,
                        name = item.optString("name", "").ifBlank { id },
                        author = item.optString("author", ""),
                        version = item.optString("version", ""),
                        description = item.optString("description", ""),
                        descriptions = parseLabels(item.optJSONObject("descriptions")),
                        license = item.optString("license", ""),
                        enabled = item.optBoolean("enabled", true),
                        hasManifest = item.optBoolean("has_manifest", false),
                        hasAction = item.optBoolean("has_action", false),
                        actionUnknown = item.optBoolean("action_unknown", false),
                        callbacks = parseStringArray(item.optJSONArray("callbacks")),
                        quickAction = parseQuickAction(item.opt("quick_action")),
                        config = parseConfig(item.optJSONArray("config")),
                        depends = parseStringArray(item.optJSONArray("depends")),
                        error = item.optString("error", ""),
                    )
                )
            }
        }
    }

    private fun parseQuickAction(value: Any?): PluginQuickAction? = when (value) {
        is JSONObject -> {
            val function = value.optString("function", "").ifBlank { "action" }
            PluginQuickAction(
                function = function,
                label = value.optString("label", "").ifBlank { function },
                labels = parseLabels(value.optJSONObject("labels")),
            )
        }

        is String -> value.takeIf { it.isNotBlank() }?.let {
            PluginQuickAction(function = it, label = it, labels = emptyMap())
        }

        else -> null
    }

    private fun parseConfig(array: JSONArray?): List<PluginConfigField> {
        if (array == null) return emptyList()
        return buildList {
            for (index in 0 until array.length()) {
                val item = array.optJSONObject(index) ?: continue
                val key = item.optString("key", "")
                if (key.isEmpty() || key.toByteArray(Charsets.UTF_8).size > 64 ||
                    '/' in key || '\\' in key) continue
                val rawDefault = item.opt("default")
                add(
                    PluginConfigField(
                        key = key,
                        label = item.optString("label", "").ifBlank { key },
                        labels = parseLabels(item.optJSONObject("labels")),
                        type = item.optString("type", "text").lowercase(),
                        default = if (rawDefault == null || rawDefault == JSONObject.NULL) {
                            ""
                        } else {
                            rawDefault.toString()
                        },
                        options = parseStringArray(item.optJSONArray("options")),
                    )
                )
            }
        }
    }

    private fun parseStringArray(array: JSONArray?): List<String> {
        if (array == null) return emptyList()
        return buildList {
            for (index in 0 until array.length()) {
                val value = array.opt(index)
                if (value != null && value != JSONObject.NULL) {
                    value.toString().takeIf { it.isNotBlank() }?.let(::add)
                }
            }
        }
    }

    private fun parseLabels(obj: JSONObject?): Map<String, String> {
        if (obj == null) return emptyMap()
        return buildMap {
            obj.keys().forEach { key ->
                obj.optString(key, "").takeIf { it.isNotBlank() }?.let { put(key, it) }
            }
        }
    }
}
