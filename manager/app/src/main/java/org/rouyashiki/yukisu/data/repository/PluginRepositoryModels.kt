package org.rouyashiki.yukisu.data.repository

import com.google.gson.annotations.SerializedName

data class PluginRepositorySource(
    @field:SerializedName("id")
    val id: String,
    @field:SerializedName("name")
    val name: String,
    @field:SerializedName("url")
    val url: String,
    @field:SerializedName("enabled")
    val enabled: Boolean = true,
    @field:SerializedName("builtIn")
    val builtIn: Boolean = false,
    @field:SerializedName("priority")
    val priority: Int = 0,
    @field:SerializedName("nameOverridden")
    val nameOverridden: Boolean = false,
    @field:SerializedName("addedAt")
    val addedAt: Long = System.currentTimeMillis(),
    @field:SerializedName("lastSyncAt")
    val lastSyncAt: Long? = null,
    @field:SerializedName("lastError")
    val lastError: String? = null,
)

data class RepositoryPlugin(
    @field:SerializedName("sourceId")
    val sourceId: String,
    @field:SerializedName("pluginId")
    val pluginId: String? = null,
    @field:SerializedName("name")
    val name: String,
    @field:SerializedName("author")
    val author: String = "",
    @field:SerializedName("version")
    val version: String = "",
    @field:SerializedName("description")
    val description: String = "",
    @field:SerializedName("descriptions")
    val descriptions: Map<String, String> = emptyMap(),
    @field:SerializedName("downloadUrl")
    val downloadUrl: String,
    @field:SerializedName("homepage")
    val homepage: String? = null,
    @field:SerializedName("sourceUrl")
    val sourceUrl: String? = null,
) {
    val stableId: String
        get() = pluginId?.takeIf(String::isNotBlank) ?: downloadUrl
}

data class PluginRepositorySnapshot(
    @field:SerializedName("sourceId")
    val sourceId: String,
    @field:SerializedName("plugins")
    val plugins: List<RepositoryPlugin>,
)
