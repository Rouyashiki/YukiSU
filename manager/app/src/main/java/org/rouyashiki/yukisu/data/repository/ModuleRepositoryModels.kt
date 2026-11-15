package org.rouyashiki.yukisu.data.repository

import org.rouyashiki.yukisu.data.registry.ExpectedFile
import com.google.gson.annotations.SerializedName

enum class RepositoryFormat {
    AUTO,
    KERNEL_SU,
    KERNEL_SU_V1,
    MMRL,
}

/**
 * A repository advertised by MMRL's central directory. Directory entries are deliberately not
 * [RepositorySource]s: the user must explicitly add one before its modules participate in the
 * local catalog.
 */
data class MmrlRepositoryDirectoryEntry(
    val name: String,
    val url: String,
    val modulesCount: Int? = null,
    val description: String? = null,
    val cover: String? = null,
    val submission: String? = null,
    val updatedAt: Long? = null,
)

data class MmrlRepositoryDirectoryState(
    val entries: List<MmrlRepositoryDirectoryEntry> = emptyList(),
    val lastSyncAt: Long? = null,
    val lastError: String? = null,
)

data class RepositorySource(
    @field:SerializedName(value = "id")
    val id: String,
    @field:SerializedName(value = "name")
    val name: String,
    @field:SerializedName(value = "url")
    val url: String,
    @field:SerializedName(value = "format")
    val format: RepositoryFormat,
    @field:SerializedName(value = "enabled")
    val enabled: Boolean = true,
    @field:SerializedName(value = "builtIn")
    val builtIn: Boolean = false,
    @field:SerializedName(value = "priority")
    val priority: Int = 0,
    @field:SerializedName(value = "nameOverridden")
    val nameOverridden: Boolean = false,
    @field:SerializedName(value = "remoteId")
    val remoteId: String? = null,
    @field:SerializedName(value = "remoteName")
    val remoteName: String? = null,
    @field:SerializedName(value = "addedAt")
    val addedAt: Long = System.currentTimeMillis(),
    @field:SerializedName(value = "lastSyncAt")
    val lastSyncAt: Long? = null,
    @field:SerializedName(value = "lastError")
    val lastError: String? = null,
)

data class ModuleCompatibility(
    @field:SerializedName(value = "minApi")
    val minApi: Int? = null,
    @field:SerializedName(value = "maxApi")
    val maxApi: Int? = null,
    @field:SerializedName(value = "architectures")
    val architectures: List<String> = emptyList(),
    @field:SerializedName(value = "devices")
    val devices: List<String> = emptyList(),
    @field:SerializedName(value = "minKernelSuVersion")
    val minKernelSuVersion: Long? = null,
)

data class RepositoryModuleVersion(
    @field:SerializedName(value = "version")
    val version: String,
    @field:SerializedName(value = "versionCode")
    val versionCode: Long,
    @field:SerializedName(value = "downloadUrl")
    val downloadUrl: String,
    @field:SerializedName(value = "assetName")
    val assetName: String? = null,
    @field:SerializedName(value = "changelogUrl")
    val changelogUrl: String? = null,
    @field:SerializedName(value = "timestamp")
    val timestamp: Long? = null,
    @field:SerializedName(value = "size")
    val size: Long? = null,
    @field:SerializedName(value = "sha256")
    val sha256: String? = null,
) {
    fun expectedFile(): ExpectedFile? = sha256?.let { ExpectedFile(checkNotNull(size), it) }
}

data class RepositoryModule(
    @field:SerializedName(value = "sourceId")
    val sourceId: String,
    @field:SerializedName(value = "moduleId")
    val moduleId: String,
    @field:SerializedName(value = "name")
    val name: String,
    @field:SerializedName(value = "author")
    val author: String,
    @field:SerializedName(value = "description")
    val description: String,
    @field:SerializedName(value = "declaredVersion")
    val declaredVersion: String,
    @field:SerializedName(value = "declaredVersionCode")
    val declaredVersionCode: Long,
    @field:SerializedName(value = "versions")
    val versions: List<RepositoryModuleVersion>,
    @field:SerializedName(value = "homepage")
    val homepage: String? = null,
    @field:SerializedName(value = "support")
    val support: String? = null,
    @field:SerializedName(value = "readme")
    val readme: String? = null,
    @field:SerializedName(value = "sourceUrl")
    val sourceUrl: String? = null,
    @field:SerializedName(value = "icon")
    val icon: String? = null,
    @field:SerializedName(value = "cover")
    val cover: String? = null,
    @field:SerializedName(value = "categories")
    val categories: List<String> = emptyList(),
    @field:SerializedName(value = "dependencies")
    val dependencies: List<String> = emptyList(),
    @field:SerializedName(value = "compatibility")
    val compatibility: ModuleCompatibility = ModuleCompatibility(),
    @field:SerializedName(value = "kind")
    val kind: String = "module",
    @field:SerializedName(value = "metamodule")
    val metamodule: Boolean = false,
    @field:SerializedName(value = "stars")
    val stars: Int? = null,
    @field:SerializedName(value = "detailsLoaded")
    val detailsLoaded: Boolean = false,
    @field:SerializedName(value = "updatedAt")
    val updatedAt: Long? = null,
) {
    val isScript: Boolean get() = kind == "script"

    /**
     * The module declaration is authoritative. MMRL history is not guaranteed to be ordered or
     * internally consistent, so choosing max(versionCode) here would silently select another
     * module's artifact when a repository index is broken.
     */
    fun declaredArtifact(): RepositoryModuleVersion? {
        return versions.firstOrNull {
            it.versionCode == declaredVersionCode && it.version == declaredVersion
        } ?: versions.singleOrNull { it.versionCode == declaredVersionCode }
    }

    val hasConsistentLatestArtifact: Boolean
        get() = declaredArtifact() != null
}

data class RepositorySnapshot(
    @field:SerializedName(value = "sourceId")
    val sourceId: String,
    @field:SerializedName(value = "format")
    val format: RepositoryFormat,
    @field:SerializedName(value = "remoteId")
    val remoteId: String? = null,
    @field:SerializedName(value = "remoteName")
    val remoteName: String? = null,
    @field:SerializedName(value = "modules")
    val modules: List<RepositoryModule>,
    @field:SerializedName(value = "registryBuildId")
    val registryBuildId: String? = null,
)

data class InstalledModuleBinding(
    @field:SerializedName(value = "moduleId")
    val moduleId: String,
    @field:SerializedName(value = "sourceId")
    val sourceId: String,
    @field:SerializedName(value = "installedVersion")
    val installedVersion: String,
    @field:SerializedName(value = "installedVersionCode")
    val installedVersionCode: Long,
    @field:SerializedName(value = "installedAt")
    val installedAt: Long = System.currentTimeMillis(),
    @field:SerializedName(value = "pinnedVersionCode")
    val pinnedVersionCode: Long? = null,
)

data class InstalledModuleState(
    val moduleId: String,
    val name: String,
    val version: String,
    val versionCode: Long,
)

enum class CompatibilityStatus {
    COMPATIBLE,
    INCOMPATIBLE,
    UNKNOWN,
}

data class CompatibilityResult(
    val status: CompatibilityStatus,
    val reasons: List<String> = emptyList(),
)
