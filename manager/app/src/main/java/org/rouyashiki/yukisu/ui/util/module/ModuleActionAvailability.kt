package org.rouyashiki.yukisu.ui.util.module

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

internal data class ModuleActionAvailability(val name: String, val available: Boolean)

internal fun moduleActionAvailability(modules: String, moduleId: String): ModuleActionAvailability? {
    val module = Json.parseToJsonElement(modules).jsonArray
        .map { it.jsonObject }
        .firstOrNull { (it["dir_id"] ?: it["id"])?.jsonPrimitive?.content == moduleId }
        ?: return null
    fun flag(name: String) = module[name]?.jsonPrimitive?.booleanOrNull
    return ModuleActionAvailability(
        name = module["name"]?.jsonPrimitive?.content ?: moduleId,
        available = flag("action") == true && flag("enabled") == true &&
            flag("update") == false && flag("remove") == false,
    )
}
