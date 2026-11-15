package org.rouyashiki.yukisu.data.repository

import android.content.Context
import android.util.AtomicFile
import android.util.Log
import org.rouyashiki.yukisu.KernelSUApplication
import org.rouyashiki.yukisu.ksuApp
import com.google.gson.Gson
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
import okhttp3.CacheControl
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.net.URI
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

class PluginRepositoryManager(
    context: Context,
    private val client: OkHttpClient,
) {
    companion object {
        private const val TAG = "PluginRepository"
        private const val MAX_INDEX_BYTES = 4L * 1024L * 1024L
        const val BUILTIN_FOLKPATCH_SOURCE_ID = "builtin-folkpatch-plugins"
        const val BUILTIN_FOLKPATCH_URL =
            "https://raw.githubusercontent.com/Anatdx/YukiPluginRepo/main/index/plugin.json"
    }

    private data class PersistedState(
        @field:SerializedName("sources")
        val sources: List<PluginRepositorySource> = emptyList(),
    )

    private val appContext = context.applicationContext
    private val gson = Gson()
    private val mutex = Mutex()
    private val repositoryClient = client.newBuilder()
        .followSslRedirects(false)
        .build()
    private val repositoryDir = File(appContext.filesDir, "plugin_repositories")
    private val stateFile = File(repositoryDir, "state.json")
    private val snapshots = ConcurrentHashMap<String, PluginRepositorySnapshot>()

    private val _sources = MutableStateFlow<List<PluginRepositorySource>>(emptyList())
    val sources: StateFlow<List<PluginRepositorySource>> = _sources.asStateFlow()

    private val _catalog = MutableStateFlow<List<RepositoryPlugin>>(emptyList())
    val catalog: StateFlow<List<RepositoryPlugin>> = _catalog.asStateFlow()

    init {
        loadFromDisk()
    }

    suspend fun addSource(name: String, url: String): PluginRepositorySource =
        withContext(Dispatchers.IO) {
            val normalizedUrl = normalizeRepositoryUrl(url)
            require(_sources.value.none { canonicalUrl(it.url) == canonicalUrl(normalizedUrl) }) {
                "This repository source already exists"
            }
            val provisional = PluginRepositorySource(
                id = UUID.randomUUID().toString(),
                name = name.trim().ifBlank { normalizedUrl },
                url = normalizedUrl,
                priority = (_sources.value.maxOfOrNull(PluginRepositorySource::priority) ?: -1) + 1,
                nameOverridden = name.isNotBlank(),
            )
            val snapshot = fetchSnapshot(provisional)
            val source = provisional.copy(
                lastSyncAt = System.currentTimeMillis(),
                lastError = null,
            )
            mutex.withLock {
                _sources.value = (_sources.value + source).sortedBy(PluginRepositorySource::priority)
                snapshots[source.id] = snapshot
                persistSnapshot(snapshot)
                publishCatalog()
                persistState()
            }
            source
        }

    suspend fun refreshAll(): List<Result<Unit>> = supervisorScope {
        _sources.value.filter(PluginRepositorySource::enabled).map { source ->
            async { refreshSource(source.id) }
        }.awaitAll()
    }

    suspend fun refreshSource(sourceId: String): Result<Unit> = withContext(Dispatchers.IO) {
        val source = _sources.value.firstOrNull { it.id == sourceId }
            ?: return@withContext Result.failure(IllegalArgumentException("Repository source not found"))
        runCatching {
            val snapshot = fetchSnapshot(source)
            mutex.withLock {
                replaceSource(
                    source.copy(
                        lastSyncAt = System.currentTimeMillis(),
                        lastError = null,
                    )
                )
                snapshots[source.id] = snapshot
                persistSnapshot(snapshot)
                publishCatalog()
                persistState()
            }
        }.onFailure { error ->
            Log.e(TAG, "Failed to refresh plugin repository ${source.id}", error)
            mutex.withLock {
                replaceSource(source.copy(lastError = error.describeFailure()))
                persistState()
            }
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
        val ordered = _sources.value.sortedBy(PluginRepositorySource::priority).toMutableList()
        val from = ordered.indexOfFirst { it.id == sourceId }
        if (from < 0) return@withLock
        val to = (from + direction).coerceIn(0, ordered.lastIndex)
        if (from == to) return@withLock
        val source = ordered.removeAt(from)
        ordered.add(to, source)
        _sources.value = ordered.mapIndexed { index, item -> item.copy(priority = index) }
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
        publishCatalog()
        persistState()
    }

    fun source(sourceId: String): PluginRepositorySource? =
        _sources.value.firstOrNull { it.id == sourceId }

    fun pluginCount(sourceId: String): Int = snapshots[sourceId]?.plugins?.size ?: 0

    private fun loadFromDisk() {
        repositoryDir.mkdirs()
        val state = readAtomic(stateFile)?.let { json ->
            runCatching { gson.fromJson(json, PersistedState::class.java) }.getOrNull()
        }
        val loaded = state?.sources.orEmpty().toMutableList()
        val builtInUrl = canonicalUrl(BUILTIN_FOLKPATCH_URL)
        if (loaded.none { it.id == BUILTIN_FOLKPATCH_SOURCE_ID || canonicalUrl(it.url) == builtInUrl }) {
            loaded.add(
                PluginRepositorySource(
                    id = BUILTIN_FOLKPATCH_SOURCE_ID,
                    name = "FolkPatch Plugin Mirror",
                    url = BUILTIN_FOLKPATCH_URL,
                    builtIn = true,
                    priority = 0,
                    nameOverridden = true,
                )
            )
        }
        _sources.value = loaded.sortedBy(PluginRepositorySource::priority)
            .mapIndexed { index, source -> source.copy(priority = index) }
        _sources.value.forEach { source ->
            readAtomic(snapshotFile(source.id))?.let { json ->
                runCatching { gson.fromJson(json, PluginRepositorySnapshot::class.java) }
                    .onFailure { Log.e(TAG, "Failed to load repository cache ${source.id}", it) }
                    .getOrNull()
                    ?.let { snapshots[source.id] = it }
            }
        }
        publishCatalog()
        persistState()
    }

    private fun fetchSnapshot(source: PluginRepositorySource): PluginRepositorySnapshot {
        val request = Request.Builder()
            .url(source.url)
            .cacheControl(CacheControl.Builder().noStore().build())
            .header("Accept", "application/json")
            .build()
        return repositoryClient.newCall(request).execute().use { response ->
            check(response.isSuccessful) { "HTTP ${response.code}" }
            val body = checkNotNull(response.body) { "Empty response body" }
            val contentLength = body.contentLength()
            check(contentLength <= MAX_INDEX_BYTES || contentLength == -1L) {
                "Repository response exceeds 4 MiB"
            }
            val bytes = body.source().readByteArrayUpTo(MAX_INDEX_BYTES + 1)
            check(bytes.size <= MAX_INDEX_BYTES) { "Repository response exceeds 4 MiB" }
            PluginRepositoryParser.parse(
                sourceId = source.id,
                indexUrl = response.request.url.toString(),
                body = bytes.toString(Charsets.UTF_8),
            )
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
        runCatching { URI(value.trim()).normalize().toString().trimEnd('/') }
            .getOrDefault(value.trim().trimEnd('/'))

    private fun Throwable.describeFailure(): String {
        val detail = generateSequence(this) { it.cause }
            .mapNotNull { it.message?.trim()?.takeIf(String::isNotEmpty) }
            .firstOrNull()
        return if (detail == null) javaClass.simpleName else "${javaClass.simpleName}: $detail"
    }

    private fun replaceSource(source: PluginRepositorySource) {
        _sources.value = _sources.value.map { if (it.id == source.id) source else it }
            .sortedBy(PluginRepositorySource::priority)
    }

    private fun publishCatalog() {
        _catalog.value = _sources.value
            .filter(PluginRepositorySource::enabled)
            .sortedBy(PluginRepositorySource::priority)
            .flatMap { snapshots[it.id]?.plugins.orEmpty() }
    }

    private fun persistState() {
        writeAtomic(stateFile, gson.toJson(PersistedState(_sources.value)))
    }

    private fun persistSnapshot(snapshot: PluginRepositorySnapshot) {
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

object PluginRepositoryProvider {
    @Volatile
    private var instance: PluginRepositoryManager? = null

    fun get(context: Context = ksuApp): PluginRepositoryManager {
        return instance ?: synchronized(this) {
            instance ?: PluginRepositoryManager(
                context = context,
                client = (context.applicationContext as? KernelSUApplication)?.okhttpClient
                    ?: ksuApp.okhttpClient,
            ).also { instance = it }
        }
    }
}
