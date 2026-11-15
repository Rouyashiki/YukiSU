package org.rouyashiki.yukisu.data.repository

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.net.URI

object PluginRepositoryParser {

    fun parse(
        sourceId: String,
        indexUrl: String,
        body: String,
    ): PluginRepositorySnapshot {
        val root = JsonParser.parseString(body)
        require(root.isJsonArray) { "Plugin repository must be a JSON array" }

        val plugins = root.asJsonArray.mapNotNull pluginLoop@ { element ->
            val obj = element.asObjectOrNull() ?: return@pluginLoop null
            val pluginId = (obj.string("id") ?: obj.string("plugin_id"))
                ?.trim()
                ?.takeIf(String::isNotEmpty)
            val rawUrl = obj.string("url") ?: obj.string("download_url")
            val downloadUrl = rawUrl
                ?.let { resolveDownloadUrl(indexUrl, it) }
                .orEmpty()
            if (downloadUrl.isEmpty()) return@pluginLoop null

            val descriptions = buildMap {
                obj.obj("descriptions")?.entrySet()?.forEach { (key, value) ->
                    value.stringValue()?.takeIf(String::isNotBlank)?.let { put(key, it) }
                }
                obj.string("description")?.takeIf(String::isNotBlank)?.let { putIfAbsent("zh", it) }
                obj.string("description_en")?.takeIf(String::isNotBlank)?.let { putIfAbsent("en", it) }
            }
            val name = obj.string("name")
                ?.trim()
                .orEmpty()
                .ifBlank { pluginId ?: downloadUrl.substringAfterLast('/').substringBefore('?') }

            RepositoryPlugin(
                sourceId = sourceId,
                pluginId = pluginId,
                name = name,
                author = obj.string("author").orEmpty(),
                version = obj.string("version").orEmpty(),
                description = obj.string("description").orEmpty()
                    .ifBlank { obj.string("description_en").orEmpty() },
                descriptions = descriptions,
                downloadUrl = downloadUrl,
                homepage = obj.string("homepage")?.let { resolvePageUrl(indexUrl, it) },
                sourceUrl = (obj.string("source") ?: obj.string("source_url"))
                    ?.let { resolvePageUrl(indexUrl, it) },
            )
        }.distinctBy { plugin -> plugin.pluginId?.let { "id:$it" } ?: "url:${plugin.downloadUrl}" }

        return PluginRepositorySnapshot(sourceId = sourceId, plugins = plugins)
    }

    private fun resolveDownloadUrl(indexUrl: String, value: String): String {
        val resolved = resolveUrl(indexUrl, value)
        val uri = runCatching { URI(resolved) }.getOrNull() ?: return ""
        val loopbackHttp = uri.scheme.equals("http", true) &&
            uri.host in setOf("127.0.0.1", "0.0.0.0", "::1", "[::1]")
        return resolved.takeIf { uri.scheme.equals("https", true) || loopbackHttp }.orEmpty()
    }

    private fun resolvePageUrl(indexUrl: String, value: String): String? {
        val resolved = resolveUrl(indexUrl, value)
        val uri = runCatching { URI(resolved) }.getOrNull() ?: return null
        val loopbackHttp = uri.scheme.equals("http", true) &&
            uri.host in setOf("127.0.0.1", "0.0.0.0", "::1", "[::1]")
        return resolved.takeIf { uri.scheme.equals("https", true) || loopbackHttp }
    }

    private fun resolveUrl(indexUrl: String, value: String): String =
        runCatching { URI(indexUrl).resolve(value.trim()).toString() }
            .getOrDefault(value.trim())
}

private fun JsonElement.asObjectOrNull(): JsonObject? =
    takeIf(JsonElement::isJsonObject)?.asJsonObject

private fun JsonObject.string(name: String): String? =
    get(name)?.stringValue()

private fun JsonObject.obj(name: String): JsonObject? =
    get(name)?.takeIf(JsonElement::isJsonObject)?.asJsonObject

private fun JsonElement.stringValue(): String? =
    takeUnless(JsonElement::isJsonNull)?.runCatching { asString }?.getOrNull()
