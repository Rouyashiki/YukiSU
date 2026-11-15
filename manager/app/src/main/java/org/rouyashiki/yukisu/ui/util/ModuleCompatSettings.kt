package org.rouyashiki.yukisu.ui.util

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

data class ModuleCompatConfig(
    val enabled: Boolean = false,
    val ksudVersionCode: Long = 0,
    val kernelVersionCode: Long = 0,
    val uapiVersion: Long = 0,
    val ksudVersion: String = "",
)

data class ModuleCompatSettings(
    val config: ModuleCompatConfig,
    val actual: ModuleCompatConfig,
)

private suspend fun moduleCompatCommand(args: String): String = withContext(Dispatchers.IO) {
    val stdout = ArrayList<String>()
    val stderr = ArrayList<String>()
    val result = getRootShell().newJob().add(ksudCmd("module compat $args"))
        .to(stdout, stderr).exec()
    check(result.isSuccess) {
        (stderr + stdout).joinToString("\n").ifBlank { "Exit ${result.code}" }
    }
    stdout.joinToString("\n")
}

suspend fun getModuleCompatSettings(): ModuleCompatSettings {
    val value = JSONObject(moduleCompatCommand("get"))
    fun parse(json: JSONObject) = ModuleCompatConfig(
        enabled = json.optBoolean("enabled", false),
        ksudVersionCode = json.getLong("ksud_version_code"),
        kernelVersionCode = json.getLong("kernel_version_code"),
        uapiVersion = json.getLong("uapi_version"),
        ksudVersion = json.getString("ksud_version"),
    )
    return ModuleCompatSettings(parse(value), parse(value.getJSONObject("actual")))
}

suspend fun setModuleCompatSettings(config: ModuleCompatConfig) {
    moduleCompatCommand(
        "set --enabled ${config.enabled} --ksud-version-code ${config.ksudVersionCode}" +
            " --kernel-version-code ${config.kernelVersionCode}" +
            " --uapi ${config.uapiVersion}" +
            " --ksud-version ${shellArg(config.ksudVersion)}"
    )
}

internal fun moduleCompatShellInit(): String = "eval \"\$(${ksudCmd("module compat env")})\""
