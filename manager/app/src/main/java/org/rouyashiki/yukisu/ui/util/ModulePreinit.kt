package org.rouyashiki.yukisu.ui.util

import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ksuApp
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONException
import org.json.JSONObject

internal data class ModulePreinitStatus(
    val supported: Boolean,
    val bootCount: Long,
    val bootloop: Boolean,
    val modules: List<Pair<String, String>>,
)

private fun runModulePreinitCommand(arguments: String): String {
    val stdout = ArrayList<String>()
    val stderr = ArrayList<String>()
    val result = getRootShell().newJob()
        .add(ksudCmd("module preinit $arguments"))
        .to(stdout, stderr)
        .exec()
    if (!result.isSuccess) {
        val details = stderr.ifEmpty { stdout }.joinToString("\n").trim().take(4096)
        throw IOException(listOf(
            ksuApp.getString(R.string.module_preinit_command_failed, result.code),
            details,
        ).filter { it.isNotEmpty() }.joinToString("\n"))
    }
    return stdout.joinToString("\n").trim()
}

internal suspend fun readModulePreinitStatus(): ModulePreinitStatus = withContext(Dispatchers.IO) {
    val output = runModulePreinitCommand("status --json")
    if (output.isEmpty()) throw IOException(ksuApp.getString(R.string.module_preinit_status_empty))
    try {
        val json = JSONObject(output)
        val modules = json.getJSONArray("modules")
        ModulePreinitStatus(
            json.getBoolean("supported"),
            json.getLong("boot_count"),
            json.getBoolean("bootloop"),
            List(modules.length()) { index ->
                val entry = modules.getJSONObject(index)
                entry.getString("id") to entry.getString("state")
            },
        )
    } catch (error: JSONException) {
        throw IOException(ksuApp.getString(R.string.module_preinit_status_invalid, error.message), error)
    }
}

internal suspend fun resetModulePreinitBootCount() = withContext(Dispatchers.IO) {
    runModulePreinitCommand("reset")
    Unit
}
