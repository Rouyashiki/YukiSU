package org.rouyashiki.yukisu.data.repository

import org.rouyashiki.yukisu.data.registry.ApiModuleDetail
import org.rouyashiki.yukisu.data.registry.RegistryCatalog
import org.rouyashiki.yukisu.data.registry.RegistryUrls
import java.time.Instant

internal fun restoreKernelSuSource(sources: List<RepositorySource>): List<RepositorySource> {
    val builtin = RepositorySource(
        id = ModuleRepositoryManager.KERNEL_SU_SOURCE_ID,
        name = "KernelSU Modules",
        url = RegistryUrls.DEFAULT_BASE_URL + RegistryUrls.INDEX_PATH,
        format = RepositoryFormat.KERNEL_SU_V1,
        builtIn = true,
    )
    return if (sources.none { it.id == builtin.id }) {
        listOf(builtin) + sources.map { it.copy(priority = it.priority + 1) }
    } else sources.map {
        if (it.id == builtin.id) builtin.copy(
            enabled = it.enabled,
            priority = it.priority,
            addedAt = it.addedAt,
            lastSyncAt = if (it.format == RepositoryFormat.KERNEL_SU_V1) it.lastSyncAt else null,
        ) else it
    }
}

internal fun registryBaseUrl(url: String): String {
    val uri = java.net.URI(url)
    require(uri.rawUserInfo == null && uri.rawQuery == null && uri.rawFragment == null) {
        "Registry URL must not contain credentials, a query or a fragment"
    }
    val path = uri.path.trimEnd('/').removeSuffix("/index.json").removeSuffix("/modules.json")
        .removeSuffix("/api/v1").trimEnd('/') + "/"
    return RegistryUrls.normalizeBaseUrl(java.net.URI(uri.scheme, null, uri.host, uri.port, path, null, null).toString())
}

internal fun RegistryCatalog.toRepositorySnapshot(
    sourceId: String,
    baseUrl: String,
    previous: RepositorySnapshot? = null,
    detail: ApiModuleDetail? = null,
): RepositorySnapshot {
    val previousModules = previous?.takeIf { it.registryBuildId == buildId }
        ?.modules.orEmpty().associateBy { it.moduleId }
    return RepositorySnapshot(
        sourceId = sourceId,
        format = RepositoryFormat.KERNEL_SU_V1,
        remoteName = "KernelSU Modules",
        registryBuildId = buildId,
        modules = modules.map { summary ->
            val loaded = detail?.takeIf { it.id == summary.id && it.buildId == buildId }
            if (loaded == null) previousModules[summary.id]?.takeIf { it.kind == summary.kind }?.let { return@map it }
            RepositoryModule(
                sourceId = sourceId,
                moduleId = summary.id,
                name = summary.name.ifBlank { summary.id },
                author = summary.authors.joinToString(", ") { it.name },
                description = summary.summary,
                declaredVersion = summary.stable.version,
                declaredVersionCode = summary.stable.versionCode.toLong(),
                versions = loaded?.releases.orEmpty().map { release ->
                    RepositoryModuleVersion(
                        version = release.version,
                        versionCode = release.versionCode.toLong(),
                        downloadUrl = release.zip.url,
                        assetName = release.zip.name,
                        changelogUrl = release.releaseUrl,
                        timestamp = epochSeconds(release.publishedAt),
                        size = release.zip.size,
                        sha256 = release.zip.sha256,
                    )
                }.distinctBy { Triple(it.versionCode, it.version, it.downloadUrl) },
                homepage = loaded?.homepage,
                support = loaded?.support,
                readme = loaded?.readme?.let { RegistryUrls.resolve(baseUrl, it) },
                sourceUrl = loaded?.source?.url,
                icon = summary.icon?.let { RegistryUrls.resolve(baseUrl, it) },
                categories = summary.tags,
                compatibility = ModuleCompatibility(
                    minApi = loaded?.compatibility?.androidMinApi,
                    maxApi = loaded?.compatibility?.androidMaxApi,
                    architectures = loaded?.compatibility?.architectures.orEmpty(),
                ),
                kind = summary.kind,
                metamodule = summary.metamodule,
                stars = summary.stars,
                detailsLoaded = loaded != null,
                updatedAt = epochSeconds(summary.stable.publishedAt),
            )
        },
    )
}

private fun epochSeconds(value: String): Long? =
    runCatching { Instant.parse(value).epochSecond }.getOrNull()
