package org.rouyashiki.yukisu.ui.webui

import android.content.Context
import org.rouyashiki.yukisu.ui.util.getRootShell
import com.google.gson.JsonElement
import com.google.gson.JsonParser
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream

data class ModuleConfig(private val engine: JsonElement? = null) {
    fun getWebuiEngine(context: Context): String? = resolveEngine(context.packageName)

    internal fun resolveEngine(packageName: String): String? {
        val value = when {
            engine == null -> null
            engine.isJsonObject -> engine.asJsonObject[packageName] ?: engine.asJsonObject["en"]
            else -> engine
        }
        return value?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }?.asString
    }

    companion object {
        internal fun parse(json: String): ModuleConfig =
            ModuleConfig(JsonParser.parseString(json).asJsonObject["webui-engine"])

        val String.asModuleConfig: ModuleConfig
            get() {
                require(isNotEmpty() && this != "." && this != ".." && none { it == '/' || it == '\\' || it == '\u0000' })
                val shell = getRootShell(true)
                for (name in listOf("config.json", "config.mmrl.json")) {
                    val file = SuFile("/data/adb/modules/$this/$name").apply { setShell(shell) }
                    if (file.isFile) {
                        return SuFileInputStream.open(file).bufferedReader().use { parse(it.readText()) }
                    }
                }
                return ModuleConfig()
            }
    }
}
