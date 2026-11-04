package com.anatdx.yukisu.ui.viewmodel

data class PluginConfigChanges(
    val updates: Map<String, String>,
    val resets: Set<String>,
)

internal fun pluginConfigChanges(
    initial: Map<String, String>,
    edited: Map<String, String>,
    resets: Set<String>,
): PluginConfigChanges = PluginConfigChanges(
    updates = edited.filter { (key, value) -> key !in resets && initial[key] != value },
    resets = resets.toSet(),
)

internal fun applyPluginConfigChanges(
    changes: PluginConfigChanges,
    delete: (String) -> Boolean,
    write: (String, String) -> Boolean,
): List<String> = buildList {
    changes.resets.forEach { key ->
        if (!delete(key)) add(key)
    }
    changes.updates.forEach { (key, value) ->
        if (key !in changes.resets && !write(key, value)) add(key)
    }
}

data class PluginConfigSaveResult(
    val failedKeys: List<String>,
    val reloadedValues: Map<String, String>? = null,
) {
    val isSuccess: Boolean get() = failedKeys.isEmpty()
}
