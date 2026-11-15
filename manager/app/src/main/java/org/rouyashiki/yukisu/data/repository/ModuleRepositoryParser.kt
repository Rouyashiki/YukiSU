package org.rouyashiki.yukisu.data.repository

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.net.URI

object ModuleRepositoryParser {

    fun parse(
        sourceId: String,
        expectedFormat: RepositoryFormat,
        indexUrl: String,
        body: String,
    ): RepositorySnapshot {
        val root = JsonParser.parseString(body)
        val detected = when {
            root.isJsonArray -> RepositoryFormat.KERNEL_SU
            root.isJsonObject && root.asJsonObject.array("modules") != null -> RepositoryFormat.MMRL
            else -> error("Unsupported module repository format")
        }
        require(expectedFormat == RepositoryFormat.AUTO || expectedFormat == detected) {
            "Expected ${expectedFormat.displayName()}, but received ${detected.displayName()}"
        }
        return when (detected) {
            RepositoryFormat.KERNEL_SU -> parseKernelSu(sourceId, indexUrl, root.asJsonArray)
            RepositoryFormat.MMRL -> parseMmrl(sourceId, indexUrl, root.asJsonObject)
            RepositoryFormat.AUTO -> error("AUTO is not a concrete repository format")
            RepositoryFormat.KERNEL_SU_V1 -> error("Registry documents require the versioned API client")
        }
    }

    private fun parseKernelSu(
        sourceId: String,
        indexUrl: String,
        root: JsonArray,
    ): RepositorySnapshot {
        val modules = root.mapNotNull kernelSuLoop@ { item ->
            val obj = item.asObjectOrNull() ?: return@kernelSuLoop null
            val moduleId = obj.string("moduleId").orEmpty().trim()
            if (moduleId.isEmpty()) return@kernelSuLoop null
            val release = obj.obj("latestRelease")
            val versionCode = release?.long("versionCode") ?: 0L
            val version = release?.string("version")
                ?: release?.string("name")
                ?: ""
            val downloadUrl = release?.string("downloadUrl")
                ?.let { resolveDownloadUrl(indexUrl, it) }
                .orEmpty()
            val versions = if (downloadUrl.isNotEmpty()) {
                listOf(
                    RepositoryModuleVersion(
                        version = version,
                        versionCode = versionCode,
                        downloadUrl = downloadUrl,
                        assetName = downloadUrl.substringAfterLast('/'),
                        timestamp = release?.long("time") ?: parseIsoTimestamp(release?.string("time")),
                    )
                )
            } else {
                emptyList()
            }
            val author = obj.array("authors")
                ?.mapNotNull { it.asObjectOrNull()?.string("name")?.takeIf(String::isNotBlank) }
                ?.joinToString(", ")
                ?.takeIf(String::isNotBlank)
                ?: obj.string("authors").orEmpty()

            RepositoryModule(
                sourceId = sourceId,
                moduleId = moduleId,
                name = obj.string("moduleName").orEmpty().ifBlank { moduleId },
                author = author,
                description = obj.string("summary").orEmpty(),
                declaredVersion = version,
                declaredVersionCode = versionCode,
                versions = versions,
                sourceUrl = obj.string("sourceUrl") ?: obj.string("url"),
                metamodule = obj.bool("metamodule") ?: false,
                stars = obj.int("stargazerCount"),
            )
        }
        return RepositorySnapshot(
            sourceId = sourceId,
            format = RepositoryFormat.KERNEL_SU,
            remoteId = "kernelsu-modules-repo",
            remoteName = "KernelSU Modules Repo",
            modules = modules,
        )
    }

    private fun parseMmrl(
        sourceId: String,
        indexUrl: String,
        root: JsonObject,
    ): RepositorySnapshot {
        val metadataVersion = root.obj("metadata")?.int("version")
        require(metadataVersion == 1) {
            "Unsupported MMRL repository metadata version: ${metadataVersion ?: "missing"}"
        }
        val modules = (root.array("modules") ?: JsonArray()).mapNotNull moduleLoop@ { item ->
            val obj = item.asObjectOrNull() ?: return@moduleLoop null
            val moduleId = obj.string("id").orEmpty().trim()
            if (moduleId.isEmpty()) return@moduleLoop null
            val declaredVersion = obj.string("version").orEmpty()
            val declaredVersionCode = obj.long("versionCode") ?: 0L
            val versions = (obj.array("versions") ?: JsonArray()).mapNotNull versionLoop@ { versionItem ->
                val versionObj = versionItem.asObjectOrNull() ?: return@versionLoop null
                val url = versionObj.string("zipUrl")
                    ?.let { resolveDownloadUrl(indexUrl, it) }
                    .orEmpty()
                if (url.isEmpty()) return@versionLoop null
                RepositoryModuleVersion(
                    version = versionObj.string("version").orEmpty(),
                    versionCode = versionObj.long("versionCode") ?: 0L,
                    downloadUrl = url,
                    assetName = url.substringAfterLast('/'),
                    changelogUrl = versionObj.string("changelog")
                        ?.takeIf(String::isNotBlank)
                        ?.let { resolveUrl(indexUrl, it) },
                    timestamp = versionObj.long("timestamp"),
                    size = versionObj.long("size"),
                )
            }.distinctBy { Triple(it.versionCode, it.version, it.downloadUrl) }

            val manager = obj.obj("manager")?.obj("kernelsu")
            val architectures = obj.stringList("arch").ifEmpty { manager.stringList("arch") }
            val devices = obj.stringList("devices").ifEmpty { manager.stringList("devices") }
            val dependencies = obj.stringList("require").ifEmpty { manager.stringList("require") }
            val track = obj.obj("track")

            RepositoryModule(
                sourceId = sourceId,
                moduleId = moduleId,
                name = obj.string("name").orEmpty().ifBlank { moduleId },
                author = obj.string("author").orEmpty(),
                description = obj.string("description").orEmpty(),
                declaredVersion = declaredVersion,
                declaredVersionCode = declaredVersionCode,
                versions = versions,
                homepage = obj.string("homepage")?.let { resolveUrl(indexUrl, it) },
                support = obj.string("support")?.let { resolveUrl(indexUrl, it) },
                readme = obj.string("readme")?.let { resolveUrl(indexUrl, it) },
                sourceUrl = track?.string("source")?.let { resolveUrl(indexUrl, it) },
                icon = obj.string("icon")?.let { resolveUrl(indexUrl, it) },
                cover = obj.string("cover")?.let { resolveUrl(indexUrl, it) },
                categories = obj.stringList("categories"),
                dependencies = dependencies,
                compatibility = ModuleCompatibility(
                    minApi = obj.int("minApi"),
                    maxApi = obj.int("maxApi"),
                    architectures = architectures,
                    devices = devices,
                    minKernelSuVersion = manager?.long("min"),
                ),
                metamodule = obj.bool("metamodule") ?: false,
                stars = obj.int("stars"),
                detailsLoaded = true,
            )
        }
        return RepositorySnapshot(
            sourceId = sourceId,
            format = RepositoryFormat.MMRL,
            remoteId = root.string("id"),
            remoteName = root.string("name"),
            modules = modules,
        )
    }

    fun enrichKernelSuModule(
        module: RepositoryModule,
        detailUrl: String,
        body: String,
    ): RepositoryModule {
        val root = JsonParser.parseString(body).asObjectOrNull()
            ?: error("Invalid KernelSU module detail")
        val releases = (root.array("releases") ?: JsonArray()).flatMap { releaseItem ->
            val release = releaseItem.asObjectOrNull() ?: return@flatMap emptyList()
            val version = release.string("version")
                ?: release.string("name")
                ?: release.string("tagName")
                ?: ""
            val versionCode = release.long("versionCode") ?: return@flatMap emptyList()
            val timestamp = parseIsoTimestamp(release.string("publishedAt"))
            (release.array("releaseAssets") ?: JsonArray()).mapNotNull assetLoop@ { assetItem ->
                val asset = assetItem.asObjectOrNull() ?: return@assetLoop null
                val assetName = asset.string("name").orEmpty()
                val downloadUrl = asset.string("downloadUrl")
                    ?.let { resolveDownloadUrl(detailUrl, it) }
                    .orEmpty()
                if (downloadUrl.isEmpty() ||
                    !(assetName.endsWith(".zip", true) || downloadUrl.substringBefore('?').endsWith(".zip", true))
                ) return@assetLoop null
                RepositoryModuleVersion(
                    version = version,
                    versionCode = versionCode,
                    downloadUrl = downloadUrl,
                    assetName = assetName.ifBlank { downloadUrl.substringAfterLast('/') },
                    timestamp = timestamp,
                    size = asset.long("size"),
                )
            }
        }
        val mergedVersions = (module.versions + releases)
            .distinctBy(RepositoryModuleVersion::downloadUrl)
        return module.copy(
            versions = mergedVersions,
            homepage = root.string("homepageUrl")
                ?.takeIf(String::isNotBlank)
                ?.let { resolveUrl(detailUrl, it) }
                ?: module.homepage,
            sourceUrl = root.string("sourceUrl")
                ?.takeIf(String::isNotBlank)
                ?.let { resolveUrl(detailUrl, it) }
                ?: module.sourceUrl,
            detailsLoaded = true,
        )
    }

    private fun resolveUrl(indexUrl: String, value: String): String {
        if (value.isBlank()) return ""
        return runCatching { URI(indexUrl).resolve(value.trim()).toString() }
            .getOrDefault(value.trim())
    }

    private fun resolveDownloadUrl(indexUrl: String, value: String): String {
        val resolved = resolveUrl(indexUrl, value)
        val scheme = runCatching { URI(resolved).scheme }.getOrNull()
        return resolved.takeIf { scheme.equals("https", true) || scheme.equals("http", true) }
            .orEmpty()
    }

    private fun parseIsoTimestamp(value: String?): Long? {
        if (value.isNullOrBlank()) return null
        return runCatching { java.time.Instant.parse(value).epochSecond }.getOrNull()
    }
}

fun RepositoryFormat.displayName(): String = when (this) {
    RepositoryFormat.AUTO -> "Auto"
    RepositoryFormat.KERNEL_SU -> "KernelSU Repo"
    RepositoryFormat.KERNEL_SU_V1 -> "KernelSU Modules"
    RepositoryFormat.MMRL -> "MMRL"
}

private fun JsonElement.asObjectOrNull(): JsonObject? =
    takeIf(JsonElement::isJsonObject)?.asJsonObject

private fun JsonObject.string(name: String): String? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.runCatching { asString }?.getOrNull()

private fun JsonObject.long(name: String): Long? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.let { element ->
        runCatching { element.asLong }.getOrElse {
            runCatching { element.asString.toLong() }.getOrNull()
        }
    }

private fun JsonObject.int(name: String): Int? = long(name)?.toInt()

private fun JsonObject.bool(name: String): Boolean? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.runCatching { asBoolean }?.getOrNull()

private fun JsonObject.obj(name: String): JsonObject? =
    get(name)?.takeIf(JsonElement::isJsonObject)?.asJsonObject

private fun JsonObject.array(name: String): JsonArray? =
    get(name)?.takeIf(JsonElement::isJsonArray)?.asJsonArray

private fun JsonObject?.stringList(name: String): List<String> =
    this?.array(name)?.mapNotNull {
        it.takeUnless(JsonElement::isJsonNull)?.runCatching { asString }?.getOrNull()
    }?.filter(String::isNotBlank).orEmpty()
