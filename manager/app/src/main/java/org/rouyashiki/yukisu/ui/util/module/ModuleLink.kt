package org.rouyashiki.yukisu.ui.util.module

import java.net.URI
import java.net.URLDecoder
import java.net.URLEncoder

data class ModuleLink(val moduleId: String, val type: Type) {
    enum class Type(val host: String) {
        Action("action"),
        WebUI("webui"),
    }

    fun toUriString(token: String): String {
        require(isValidModuleId(moduleId) && token.isNotBlank())
        return "ksu://${type.host}?id=${URLEncoder.encode(moduleId, "UTF-8")}&token=${URLEncoder.encode(token, "UTF-8")}"
    }

    companion object {
        private val moduleIdPattern = Regex("[a-zA-Z][a-zA-Z0-9._-]+")

        fun isValidModuleId(moduleId: String): Boolean = moduleIdPattern.matches(moduleId)

        fun parse(value: String?, expectedToken: String): ModuleLink? = runCatching {
            if (value == null || expectedToken.isBlank()) return null
            val uri = URI(value)
            if (uri.scheme != "ksu" || uri.rawUserInfo != null || uri.port != -1 ||
                !uri.rawPath.isNullOrEmpty() || uri.rawFragment != null
            ) return null
            val type = Type.entries.firstOrNull { it.host == uri.host } ?: return null
            val parameters = (uri.rawQuery ?: return null).split('&').map { parameter ->
                val pair = parameter.split('=', limit = 2)
                URLDecoder.decode(pair[0], "UTF-8") to URLDecoder.decode(pair.getOrElse(1) { "" }, "UTF-8")
            }.groupBy({ it.first }, { it.second })
            val token = parameters["token"]?.singleOrNull() ?: return null
            if (token != expectedToken) return null
            val moduleId = parameters["id"]?.singleOrNull() ?: return null
            if (!isValidModuleId(moduleId)) return null
            ModuleLink(moduleId, type)
        }.getOrNull()
    }
}
