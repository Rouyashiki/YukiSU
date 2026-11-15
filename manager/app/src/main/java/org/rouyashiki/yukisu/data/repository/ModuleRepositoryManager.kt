package org.rouyashiki.yukisu.data.repository

import android.content.Context
import android.util.AtomicFile
import android.util.Log
import org.rouyashiki.yukisu.data.registry.FileRegistryCache
import org.rouyashiki.yukisu.data.registry.RegistryClient
import org.rouyashiki.yukisu.data.registry.RegistryFetcher
import org.rouyashiki.yukisu.data.registry.RegistryUpdatePolicy
import org.rouyashiki.yukisu.data.registry.RegistryUrls
import org.rouyashiki.yukisu.data.registry.UpdateVerification
import org.rouyashiki.yukisu.ksuApp
import com.google.gson.Gson
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.annotations.SerializedName
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.supervisorScope
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import okio.Buffer
import okio.BufferedSource
import java.io.File
import java.net.URI
import java.net.URLEncoder
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

class ModuleRepositoryManager(
    context: Context,
    private val client: OkHttpClient,
) {
    companion object {
        private const val TAG = "ModuleRepository"
        const val KERNEL_SU_SOURCE_ID = "builtin-kernelsu-modules"
        const val MMRL_DIRECTORY_URL = "https://mmrl.dev/api/repositories.json"
        private const val MAX_INDEX_BYTES = 16L * 1024L * 1024L
    }

    private data class PersistedState(
        @field:SerializedName(value = "sources")
        val sources: List<RepositorySource> = emptyList(),
        @field:SerializedName(value = "bindings")
        val bindings: List<InstalledModuleBinding> = emptyList(),
    )

    private val appContext = context.applicationContext
    private val gson = Gson()
    private val mutex = Mutex()
    private val repositoryDir = File(appContext.filesDir, "module_repositories")
    private val stateFile = File(repositoryDir, "state.json")
    private val mmrlDirectoryFile = File(repositoryDir, "mmrl-directory.json")

    private val _sources = MutableStateFlow<List<RepositorySource>>(emptyList())
    val sources: StateFlow<List<RepositorySource>> = _sources.asStateFlow()

    private val _catalog = MutableStateFlow<List<RepositoryModule>>(emptyList())
    val catalog: StateFlow<List<RepositoryModule>> = _catalog.asStateFlow()

    private val _bindings = MutableStateFlow<Map<String, InstalledModuleBinding>>(emptyMap())
    val bindings: StateFlow<Map<String, InstalledModuleBinding>> = _bindings.asStateFlow()

    private val _mmrlDirectory = MutableStateFlow(MmrlRepositoryDirectoryState())
    val mmrlDirectory: StateFlow<MmrlRepositoryDirectoryState> = _mmrlDirectory.asStateFlow()

    private val snapshots = ConcurrentHashMap<String, RepositorySnapshot>()

    private val sourceLocks = ConcurrentHashMap<String, Mutex>()
    private val registries = ConcurrentHashMap<String, RegistryClient>()

    private fun sourceLock(sourceId: String): Mutex = sourceLocks.getOrPut(sourceId) { Mutex() }

    private fun registry(source: RepositorySource): RegistryClient = registries.getOrPut(source.id) {
        val baseUrl = registryBaseUrl(source.url)
        RegistryClient(
            baseUrl = baseUrl,
            fetcher = RegistryFetcher { fetchBody(it, baseUrl).body },
            cache = FileRegistryCache(File(repositoryDir, "registry-${source.id}")),
        )
    }

    init {
        loadFromDisk()
    }

    suspend fun addSource(
        name: String,
        url: String,
        format: RepositoryFormat,
    ): RepositorySource = withContext(Dispatchers.IO) {
        val normalizedUrl = normalizeRepositoryUrl(url)
        require(_sources.value.none { canonicalUrl(it.url) == canonicalUrl(normalizedUrl) }) {
            "This repository source already exists"
        }
        val effectiveFormat = if (format != RepositoryFormat.MMRL &&
            RegistryUrls.isSameOrigin(normalizedUrl, RegistryUrls.DEFAULT_BASE_URL)
        ) RepositoryFormat.KERNEL_SU_V1 else format
        val provisional = RepositorySource(
            id = UUID.randomUUID().toString(),
            name = name.trim().ifBlank { normalizedUrl },
            url = normalizedUrl,
            format = effectiveFormat,
            priority = (_sources.value.maxOfOrNull(RepositorySource::priority) ?: -1) + 1,
            nameOverridden = name.isNotBlank(),
        )
        val snapshot = fetchSnapshot(provisional)
        val source = provisional.copy(
            name = name.trim().ifBlank { snapshot.remoteName ?: normalizedUrl },
            format = snapshot.format,
            remoteId = snapshot.remoteId,
            remoteName = snapshot.remoteName,
            lastSyncAt = System.currentTimeMillis(),
            lastError = null,
        )
        mutex.withLock {
            _sources.value = (_sources.value + source).sortedBy(RepositorySource::priority)
            snapshots[source.id] = snapshot
            persistSnapshot(snapshot)
            publishCatalog()
            persistState()
        }
        source
    }

    suspend fun refreshAll(): List<Result<Unit>> = supervisorScope {
        _sources.value.filter(RepositorySource::enabled).map { source ->
            async { refreshSource(source.id) }
        }.awaitAll()
    }

    suspend fun refreshMmrlDirectory(): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val fetched = fetchBody(MMRL_DIRECTORY_URL)
            val entries = parseMmrlDirectory(fetched.body)
            require(entries.isNotEmpty()) { "MMRL directory contains no repositories" }
            val now = System.currentTimeMillis()
            mutex.withLock {
                writeAtomic(mmrlDirectoryFile, fetched.body)
                _mmrlDirectory.value = MmrlRepositoryDirectoryState(
                    entries = entries,
                    lastSyncAt = now,
                    lastError = null,
                )
            }
        }.onFailure { error ->
            Log.e(TAG, "Failed to refresh MMRL repository directory", error)
            mutex.withLock {
                _mmrlDirectory.value = _mmrlDirectory.value.copy(
                    lastError = error.describeFailure(),
                )
            }
        }
    }

    suspend fun refreshSource(sourceId: String): Result<Unit> = withContext(Dispatchers.IO) {
        sourceLock(sourceId).withLock { refreshSourceLocked(sourceId) }
    }

    private suspend fun refreshSourceLocked(sourceId: String): Result<Unit> {
        val source = _sources.value.firstOrNull { it.id == sourceId }
            ?: return Result.failure(IllegalArgumentException("Repository source not found"))
        return runCatching {
            val snapshot = fetchSnapshot(source)
            mutex.withLock {
                val current = _sources.value.firstOrNull { it.id == sourceId } ?: return@withLock
                val updated = current.copy(
                    name = if (current.nameOverridden) current.name else snapshot.remoteName ?: current.name,
                    format = snapshot.format,
                    remoteId = snapshot.remoteId,
                    remoteName = snapshot.remoteName,
                    lastSyncAt = System.currentTimeMillis(),
                    lastError = null,
                )
                replaceSource(updated)
                snapshots[source.id] = snapshot
                persistSnapshot(snapshot)
                publishCatalog()
                persistState()
            }
        }.onFailure { error ->
            Log.e(TAG, "Failed to refresh repository ${source.id} (${source.url})", error)
            mutex.withLock {
                _sources.value.firstOrNull { it.id == sourceId }?.let {
                    replaceSource(it.copy(lastError = error.describeFailure()))
                    persistState()
                }
            }
        }
    }

    suspend fun loadModuleDetails(
        sourceId: String,
        moduleId: String,
    ): Result<RepositoryModule> = withContext(Dispatchers.IO) {
        sourceLock(sourceId).withLock { loadModuleDetailsLocked(sourceId, moduleId) }
    }

    private suspend fun loadModuleDetailsLocked(sourceId: String, moduleId: String): Result<RepositoryModule> {
        val source = _sources.value.firstOrNull { it.id == sourceId }
            ?: return Result.failure(IllegalArgumentException("Repository source not found"))
        val module = module(sourceId, moduleId)
            ?: return Result.failure(IllegalArgumentException("Repository module not found"))
        if (source.format == RepositoryFormat.KERNEL_SU_V1) {
            return runCatching {
                val registry = registry(source)
                val loaded = try {
                    registry.loadDetail(moduleId)
                } catch (error: Exception) {
                    registry.currentCatalog()?.let { catalog ->
                        mutex.withLock {
                            snapshots[sourceId] = catalog.toRepositorySnapshot(sourceId, registry.baseUrl, snapshots[sourceId])
                            publishCatalog()
                        }
                    }
                    throw error
                }
                mutex.withLock {
                    val snapshot = loaded.catalog.toRepositorySnapshot(
                        sourceId, registry.baseUrl, snapshots[sourceId], loaded.detail,
                    )
                    snapshots[sourceId] = snapshot
                    publishCatalog()
                    snapshot.modules.first { it.moduleId == moduleId }
                }
            }
        }
        if (source.format != RepositoryFormat.KERNEL_SU || module.detailsLoaded) {
            return Result.success(module)
        }
        return runCatching {
            val indexUrl = candidateIndexUrls(source).first()
            val encodedId = URLEncoder.encode(moduleId, Charsets.UTF_8.name()).replace("+", "%20")
            val detailUrl = URI(indexUrl).resolve("module/$encodedId.json").toString()
            val fetched = fetchBody(detailUrl)
            mutex.withLock {
                val currentSnapshot = snapshots[sourceId] ?: error("Repository cache disappeared")
                val currentModule = currentSnapshot.modules.firstOrNull { it.moduleId == moduleId }
                    ?: error("Repository module disappeared")
                val enriched = ModuleRepositoryParser.enrichKernelSuModule(
                    module = currentModule,
                    detailUrl = fetched.finalUrl,
                    body = fetched.body,
                )
                val updatedSnapshot = currentSnapshot.copy(
                    modules = currentSnapshot.modules.map {
                        if (it.moduleId == moduleId) enriched else it
                    }
                )
                snapshots[sourceId] = updatedSnapshot
                persistSnapshot(updatedSnapshot)
                publishCatalog()
                enriched
            }
        }
    }

    suspend fun verifyUpdate(
        moduleId: String,
        updateJsonUrl: String,
        zipUrl: String,
        versionCode: Int,
    ): UpdateVerification = withContext(Dispatchers.IO) {
        if (RegistryUpdatePolicy.isUnrelatedToRegistry(RegistryUrls.DEFAULT_BASE_URL, updateJsonUrl, zipUrl)) {
            return@withContext UpdateVerification.Legacy
        }
        sourceLock(KERNEL_SU_SOURCE_ID).withLock {
            val source = _sources.value.first { it.id == KERNEL_SU_SOURCE_ID }
            val registry = registry(source)
            val catalog = runCatching { registry.catalog() }.getOrNull()
            val inIndex = catalog?.find(moduleId) != null
            val loaded = if (inIndex) runCatching { registry.loadDetail(moduleId) }.getOrNull() else null
            RegistryUpdatePolicy.decide(
                registry.baseUrl, updateJsonUrl, zipUrl, versionCode,
                registry.currentCatalog()?.let { it.find(moduleId) != null }, loaded?.detail,
            )
        }
    }

    suspend fun setSourceEnabled(sourceId: String, enabled: Boolean) = mutex.withLock {
        val source = _sources.value.firstOrNull { it.id == sourceId } ?: return@withLock
        replaceSource(source.copy(enabled = enabled))
        publishCatalog()
        persistState()
    }

    suspend fun moveSource(sourceId: String, direction: Int) = mutex.withLock {
        if (direction == 0) return@withLock
        val ordered = _sources.value.sortedBy(RepositorySource::priority).toMutableList()
        val from = ordered.indexOfFirst { it.id == sourceId }
        if (from < 0) return@withLock
        val to = (from + direction).coerceIn(0, ordered.lastIndex)
        if (from == to) return@withLock
        val item = ordered.removeAt(from)
        ordered.add(to, item)
        _sources.value = ordered.mapIndexed { index, source -> source.copy(priority = index) }
        publishCatalog()
        persistState()
    }

    suspend fun removeSource(sourceId: String) = mutex.withLock {
        val source = _sources.value.firstOrNull { it.id == sourceId } ?: return@withLock
        require(!source.builtIn) { "Built-in repository sources cannot be removed" }
        _sources.value = _sources.value.filterNot { it.id == sourceId }
            .mapIndexed { index, item -> item.copy(priority = index) }
        snapshots.remove(sourceId)
        snapshotFile(sourceId).delete()
        // Bindings deliberately survive source removal so installed modules retain provenance.
        publishCatalog()
        persistState()
    }

    suspend fun recordInstalledBinding(
        moduleId: String,
        sourceId: String,
        version: String,
        versionCode: Long,
    ) = mutex.withLock {
        val old = _bindings.value[moduleId]
        _bindings.value = _bindings.value + (
            moduleId to InstalledModuleBinding(
                moduleId = moduleId,
                sourceId = sourceId,
                installedVersion = version,
                installedVersionCode = versionCode,
                pinnedVersionCode = if (old?.sourceId == sourceId && old.pinnedVersionCode != null) {
                    versionCode
                } else {
                    null
                },
            )
        )
        persistState()
    }

    suspend fun setPinnedVersion(moduleId: String, versionCode: Long?) = mutex.withLock {
        val binding = _bindings.value[moduleId] ?: return@withLock
        _bindings.value = _bindings.value + (moduleId to binding.copy(pinnedVersionCode = versionCode))
        persistState()
    }

    suspend fun clearInstalledBinding(moduleId: String) = mutex.withLock {
        if (moduleId !in _bindings.value) return@withLock
        _bindings.value = _bindings.value - moduleId
        persistState()
    }

    suspend fun reconcileBindings(installedModuleIds: Set<String>) = mutex.withLock {
        val reconciled = _bindings.value.filterKeys { it in installedModuleIds }
        if (reconciled.size != _bindings.value.size) {
            _bindings.value = reconciled
            persistState()
        }
    }

    fun module(sourceId: String, moduleId: String): RepositoryModule? =
        snapshots[sourceId]?.modules?.firstOrNull { it.moduleId == moduleId }

    fun modulesById(moduleId: String): List<RepositoryModule> =
        _catalog.value.filter { it.moduleId == moduleId }

    fun moduleCount(sourceId: String): Int = snapshots[sourceId]?.modules?.size ?: 0

    fun sourceForUrl(url: String): RepositorySource? {
        val canonical = canonicalUrl(url)
        return _sources.value.firstOrNull { canonicalUrl(it.url) == canonical }
    }

    private fun loadFromDisk() {
        repositoryDir.mkdirs()
        val state = readAtomic(stateFile)?.let {
            runCatching { gson.fromJson(it, PersistedState::class.java) }.getOrNull()
        }
        val loadedSources = restoreKernelSuSource(state?.sources.orEmpty())
        AtomicFile(snapshotFile(KERNEL_SU_SOURCE_ID)).delete()
        _sources.value = loadedSources.sortedBy(RepositorySource::priority)
            .mapIndexed { index, source -> source.copy(priority = index) }
        _bindings.value = state?.bindings.orEmpty().associateBy(InstalledModuleBinding::moduleId)
        _sources.value.forEach { source ->
            if (source.format == RepositoryFormat.KERNEL_SU_V1) {
                val registry = registry(source)
                registry.cachedCatalog()?.let {
                    snapshots[source.id] = it.toRepositorySnapshot(source.id, registry.baseUrl)
                }
                return@forEach
            }
            readAtomic(snapshotFile(source.id))?.let { json ->
                runCatching { gson.fromJson(json, RepositorySnapshot::class.java) }
                    .getOrNull()
                    ?.let { snapshots[source.id] = it }
            }
        }
        readAtomic(mmrlDirectoryFile)?.let { json ->
            runCatching { parseMmrlDirectory(json) }
                .onFailure { Log.e(TAG, "Failed to read cached MMRL directory", it) }
                .getOrNull()
                ?.takeIf { it.isNotEmpty() }
                ?.let { entries ->
                    _mmrlDirectory.value = MmrlRepositoryDirectoryState(
                        entries = entries,
                        lastSyncAt = mmrlDirectoryFile.lastModified().takeIf { it > 0L },
                    )
                }
        }
        publishCatalog()
        persistState()
    }

    private fun parseMmrlDirectory(body: String): List<MmrlRepositoryDirectoryEntry> {
        val root = JsonParser.parseString(body)
        require(root.isJsonArray) { "Invalid MMRL repository directory" }
        return root.asJsonArray.mapNotNull { element ->
            val obj = element.asObjectOrNull() ?: return@mapNotNull null
            val rawUrl = obj.string("url")?.trim().orEmpty()
            val url = runCatching { normalizeRepositoryUrl(rawUrl) }.getOrNull()
                ?: return@mapNotNull null
            MmrlRepositoryDirectoryEntry(
                name = obj.string("name")?.trim().orEmpty().ifBlank { url },
                url = url,
                modulesCount = obj.int("modules_count"),
                description = obj.string("description")?.takeIf(String::isNotBlank),
                cover = obj.string("cover")?.takeIf(String::isNotBlank),
                submission = obj.string("submission")?.takeIf(String::isNotBlank),
                updatedAt = obj.double("timestamp")?.times(1_000)?.toLong(),
            )
        }.distinctBy { canonicalUrl(it.url) }
    }

    private fun fetchSnapshot(source: RepositorySource): RepositorySnapshot {
        if (source.format == RepositoryFormat.KERNEL_SU_V1) {
            val registry = registry(source)
            val loaded = registry.loadCatalog()
            loaded.error?.let { throw it }
            return loaded.catalog.toRepositorySnapshot(source.id, registry.baseUrl, snapshots[source.id])
        }
        val failures = mutableListOf<String>()
        for (url in candidateIndexUrls(source)) {
            val result = runCatching {
                val fetched = fetchBody(url)
                ModuleRepositoryParser.parse(
                    sourceId = source.id,
                    expectedFormat = source.format,
                    indexUrl = fetched.finalUrl,
                    body = fetched.body,
                )
            }
            result.onSuccess { return it }
            val error = checkNotNull(result.exceptionOrNull())
            Log.e(TAG, "Failed repository candidate $url for ${source.id}", error)
            failures += error.describeFailure()
        }
        error(failures.distinct().joinToString("; "))
    }

    private fun Throwable.describeFailure(): String {
        val detail = generateSequence(this) { it.cause }
            .mapNotNull { it.message?.trim()?.takeIf(String::isNotEmpty) }
            .firstOrNull()
        return if (detail == null) javaClass.simpleName else "${javaClass.simpleName}: $detail"
    }

    private data class FetchedBody(val finalUrl: String, val body: String)

    private fun fetchBody(url: String, registryOrigin: String? = null): FetchedBody {
        val request = Request.Builder().url(url).build()
        val transport = if (registryOrigin == null) client else client.newBuilder()
            .addNetworkInterceptor { chain ->
                if (!RegistryUrls.isAllowedRestrictedResource(chain.request().url.toString(), registryOrigin)) {
                    throw java.io.IOException("Registry redirected outside its HTTPS origin")
                }
                chain.proceed(chain.request())
            }.build()
        return transport.newCall(request).execute().use { response ->
            check(response.isSuccessful) { "HTTP ${response.code} for $url" }
            check(registryOrigin == null ||
                RegistryUrls.isAllowedRestrictedResource(response.request.url.toString(), registryOrigin)
            ) { "Registry redirected outside its HTTPS origin" }
            val body = checkNotNull(response.body) { "Empty response body for $url" }
            val contentLength = body.contentLength()
            check(contentLength <= MAX_INDEX_BYTES || contentLength == -1L) {
                "Repository response exceeds 16 MiB"
            }
            val bytes = body.source().readByteArrayUpTo(MAX_INDEX_BYTES + 1)
            check(bytes.size <= MAX_INDEX_BYTES) { "Repository response exceeds 16 MiB" }
            FetchedBody(
                finalUrl = response.request.url.toString(),
                body = bytes.toString(Charsets.UTF_8),
            )
        }
    }

    private fun candidateIndexUrls(source: RepositorySource): List<String> {
        val url = source.url
        if (url.substringBefore('?').endsWith(".json", ignoreCase = true)) return listOf(url)
        val base = url.trimEnd('/') + "/"
        return when (source.format) {
            RepositoryFormat.KERNEL_SU_V1 -> listOf(RegistryUrls.resolve(base, RegistryUrls.INDEX_PATH))
            RepositoryFormat.KERNEL_SU -> listOf(URI(base).resolve("modules.json").toString())
            RepositoryFormat.MMRL -> listOf(URI(base).resolve("json/modules.json").toString())
            RepositoryFormat.AUTO -> listOf(
                URI(base).resolve("json/modules.json").toString(),
                URI(base).resolve("modules.json").toString(),
                url,
            ).distinct()
        }
    }

    private fun normalizeRepositoryUrl(raw: String): String {
        val value = raw.trim()
        require(value.isNotEmpty()) { "Repository URL is required" }
        val uri = runCatching { URI(value) }.getOrElse { error("Invalid repository URL") }
        require(!uri.host.isNullOrBlank()) { "Repository URL must include a host" }
        val loopbackHttp = uri.scheme.equals("http", true) &&
            uri.host in setOf("127.0.0.1", "0.0.0.0", "::1", "[::1]")
        require(uri.scheme.equals("https", true) || loopbackHttp) {
            "Repository URL must use HTTPS (HTTP is only available for loopback testing)"
        }
        return uri.normalize().toString()
    }

    private fun canonicalUrl(value: String): String =
        if (RegistryUrls.isSameOrigin(value, RegistryUrls.DEFAULT_BASE_URL)) RegistryUrls.DEFAULT_BASE_URL else runCatching { URI(value.trim()).normalize().toString().trimEnd('/') }
            .getOrDefault(value.trim().trimEnd('/'))

    private fun replaceSource(source: RepositorySource) {
        _sources.value = _sources.value.map { if (it.id == source.id) source else it }
            .sortedBy(RepositorySource::priority)
    }

    private fun publishCatalog() {
        _catalog.value = _sources.value
            .filter(RepositorySource::enabled)
            .sortedBy(RepositorySource::priority)
            .flatMap { snapshots[it.id]?.modules.orEmpty() }
    }

    private fun persistState() {
        writeAtomic(
            stateFile,
            gson.toJson(PersistedState(_sources.value, _bindings.value.values.toList()))
        )
    }

    private fun persistSnapshot(snapshot: RepositorySnapshot) {
        if (snapshot.format == RepositoryFormat.KERNEL_SU_V1) return
        writeAtomic(snapshotFile(snapshot.sourceId), gson.toJson(snapshot))
    }

    private fun snapshotFile(sourceId: String): File = File(repositoryDir, "catalog-$sourceId.json")

    private fun readAtomic(file: File): String? {
        if (!file.exists()) return null
        return runCatching { AtomicFile(file).openRead().bufferedReader().use { it.readText() } }
            .getOrNull()
    }

    private fun writeAtomic(file: File, content: String) {
        file.parentFile?.mkdirs()
        val atomic = AtomicFile(file)
        val stream = atomic.startWrite()
        try {
            stream.write(content.toByteArray(Charsets.UTF_8))
            atomic.finishWrite(stream)
        } catch (error: Throwable) {
            atomic.failWrite(stream)
            throw error
        }
    }
}

private fun JsonElement.asObjectOrNull(): JsonObject? =
    takeIf(JsonElement::isJsonObject)?.asJsonObject

private fun JsonObject.string(name: String): String? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.runCatching { asString }?.getOrNull()

private fun JsonObject.int(name: String): Int? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.runCatching { asInt }?.getOrNull()

private fun JsonObject.double(name: String): Double? =
    get(name)?.takeUnless(JsonElement::isJsonNull)?.runCatching { asDouble }?.getOrNull()

internal fun BufferedSource.readByteArrayUpTo(byteCount: Long): ByteArray {
    require(byteCount >= 0L) { "byteCount must be non-negative" }
    val buffer = Buffer()
    while (buffer.size < byteCount) {
        val read = read(buffer, byteCount - buffer.size)
        if (read == -1L) break
    }
    return buffer.readByteArray()
}

object ModuleRepositoryProvider {
    @Volatile
    private var instance: ModuleRepositoryManager? = null

    fun get(context: Context = ksuApp): ModuleRepositoryManager {
        return instance ?: synchronized(this) {
            instance ?: ModuleRepositoryManager(
                context = context,
                client = (context.applicationContext as? org.rouyashiki.yukisu.KernelSUApplication)
                    ?.okhttpClient
                    ?: ksuApp.okhttpClient,
            ).also { instance = it }
        }
    }
}
