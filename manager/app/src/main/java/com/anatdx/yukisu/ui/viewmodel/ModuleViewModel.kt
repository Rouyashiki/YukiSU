package com.anatdx.yukisu.ui.viewmodel

import android.content.Context
import android.os.SystemClock
import android.util.Log
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.LocalViewModelStoreOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import com.anatdx.yukisu.data.registry.UpdateVerification
import com.anatdx.yukisu.data.repository.ModuleRepositoryProvider
import com.anatdx.yukisu.ui.webui.ModuleConfig
import com.anatdx.yukisu.ui.webui.ModuleConfig.Companion.asModuleConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.ui.util.HanziToPinyin
import com.anatdx.yukisu.ui.util.listModules
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.module.ModuleRuntimeKind
import com.anatdx.yukisu.ui.util.module.parseModuleRuntimeKinds
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.text.Collator
import java.text.DecimalFormat
import java.util.Locale
import java.util.concurrent.TimeUnit
import kotlin.math.log10
import kotlin.math.pow
import androidx.core.content.edit

/**
 * @author ShirkNeko
 * @date 2025/5/31.
 */
data class ModuleUpdateInfo(
    val zipUrl: String = "",
    val version: String = "",
    val changelog: String = "",
    val verification: UpdateVerification = UpdateVerification.Legacy,
)

@Composable
fun rememberModuleViewModel(): ModuleViewModel = viewModel(
    viewModelStoreOwner = LocalActivity.current as? ViewModelStoreOwner
        ?: checkNotNull(LocalViewModelStoreOwner.current),
)

class ModuleViewModel : ViewModel() {

    companion object {
        private const val TAG = "ModuleViewModel"
        private val CUSTOM_USER_AGENT = "YukiSU/${BuildConfig.VERSION_NAME}"
    }

    private var modules by mutableStateOf<List<ModuleInfo>>(emptyList())
    private val moduleUpdates = ModuleUpdateState(viewModelScope, checkUpdate = ::checkUpdate)
    private val fetchMutex = Mutex()
    private var fetchJob: Job? = null
    private var hasLoaded = false
    private val updateClient = okhttp3.OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .writeTimeout(15, TimeUnit.SECONDS)
        .callTimeout(30, TimeUnit.SECONDS)
        .build()

    val updateInfo: Map<String, ModuleUpdateInfo>
        get() = moduleUpdates.updateInfo
    val availableUpdateCount: Int
        get() = moduleUpdates.availableCount

    private lateinit var moduleSizeCache: ModuleSizeCache

    fun initializeCache(context: Context) {
        if (!::moduleSizeCache.isInitialized) {
            moduleSizeCache = ModuleSizeCache(context)
        }
    }

    fun getModuleSize(dirId: String): String {
        if (!::moduleSizeCache.isInitialized) {
            return "0 KB"
        }
        val size = moduleSizeCache.getModuleSize(dirId)
        return formatFileSize(size)
    }

    fun refreshModuleSizeCache() {
        if (!::moduleSizeCache.isInitialized) return

        viewModelScope.launch(Dispatchers.IO) {
            Log.d(TAG, "开始刷新模块大小缓存")
            val currentModules = modules.map { it.dirId }
            moduleSizeCache.refreshCache(currentModules)
            Log.d(TAG, "模块大小缓存刷新完成")
        }
    }

    class ModuleInfo(
        val id: String,
        val name: String,
        val author: String,
        val version: String,
        val versionCode: Int,
        val description: String,
        val enabled: Boolean,
        val update: Boolean,
        val remove: Boolean,
        val updateJson: String,
        val hasWebUi: Boolean,
        val hasActionScript: Boolean,
        val metamodule: Boolean,
        val dirId: String, // real module id (dir name)
        val actionIconPath: String? = null,
        val webUiIconPath: String? = null,
        val runtimeKinds: Set<ModuleRuntimeKind> = emptySet(),
        val zygiskConflict: Boolean = false,
        var config: ModuleConfig? = null,
        val preinit: String = "",

    )

    var isRefreshing by mutableStateOf(false)
        private set
    var search by mutableStateOf("")

    var sortEnabledFirst by mutableStateOf(false)
    var sortActionFirst by mutableStateOf(false)
    val moduleList by derivedStateOf {
        val comparator =
            compareBy<ModuleInfo>(
                {
                    val executable = it.hasWebUi || it.hasActionScript
                    when {
                        it.metamodule && it.enabled -> 0
                        sortEnabledFirst && sortActionFirst -> when {
                            it.enabled && executable -> 1
                            it.enabled -> 2
                            executable -> 3
                            else -> 4
                        }
                        sortEnabledFirst && !sortActionFirst -> if (it.enabled) 1 else 2
                        !sortEnabledFirst && sortActionFirst -> if (executable) 1 else 2
                        else -> 1
                    }
                },
                { if (sortEnabledFirst) !it.enabled else 0 },
                { if (sortActionFirst) !(it.hasWebUi || it.hasActionScript) else 0 },
            ).thenBy(Collator.getInstance(Locale.getDefault()), ModuleInfo::id)
        modules.filter {
            it.id.contains(search, true) || it.name.contains(search, true) || HanziToPinyin.getInstance()
                .toPinyinString(it.name)?.contains(search, true) == true
        }.sortedWith(comparator)
    }

    var isNeedRefresh by mutableStateOf(false)
        private set

    fun markNeedRefresh() {
        isNeedRefresh = true
        refreshModuleSizeCache()
    }

    fun ensureModuleListLoaded() {
        if ((!hasLoaded || isNeedRefresh) && fetchJob?.isActive != true) {
            fetchModuleList()
        }
    }

    fun fetchModuleList(forceUpdateCheck: Boolean = false) {
        fetchJob?.cancel()
        fetchJob = viewModelScope.launch {
            fetchMutex.withLock {
                isRefreshing = true
                val start = SystemClock.elapsedRealtime()

                try {
                    val moduleInfos = withContext(Dispatchers.IO) {
                        // ksud reads the YukiZygisk feature itself, does the Zygisk scan and
                        // the conflict force-disable in this one call, so the list and its
                        // tags land in a single state write.
                        val result = listModules()

                        Log.i(TAG, "result: $result")

                        val array = JSONArray(result)
                        (0 until array.length())
                            .asSequence()
                            .map { array.getJSONObject(it) }
                            .map { obj ->
                                ModuleInfo(
                                    obj.getString("id"),
                                    obj.optString("name"),
                                    obj.optString("author", "Unknown"),
                                    obj.optString("version", "Unknown"),
                                    obj.getIntCompat("versionCode", 0),
                                    obj.optString("description"),
                                    obj.getBooleanCompat("enabled"),
                                    obj.getBooleanCompat("update"),
                                    obj.getBooleanCompat("remove"),
                                    obj.optString("updateJson"),
                                    obj.getBooleanCompat("web"),
                                    obj.getBooleanCompat("action"),
                                    obj.getBooleanCompat("metamodule"),
                                    obj.optString("dir_id", obj.getString("id")),
                                    obj.optString("actionIcon").takeIf { it.isNotBlank() },
                                    obj.optString("webuiIcon").takeIf { it.isNotBlank() },
                                    parseModuleRuntimeKinds(
                                        obj.optJSONArray("runtimes")?.let { runtimes ->
                                            List(runtimes.length()) { runtimes.optString(it) }
                                        },
                                        obj.optString("runtime"),
                                    ),
                                    obj.getBooleanCompat("yzConflict"),
                                    preinit = obj.optString("preinit")
                                )
                            }.toList()
                    }

                    modules = moduleInfos
                    hasLoaded = true
                    isNeedRefresh = false
                    moduleUpdates.refresh(moduleInfos, force = forceUpdateCheck)

                    launch(Dispatchers.IO) {
                        moduleInfos.forEach { module ->
                            withContext(Dispatchers.IO) {
                                try {
                                    runCatching {
                                        module.config = module.id.asModuleConfig
                                    }.onFailure { e ->
                                        Log.e(TAG, "Failed to load config from id for module ${module.id}", e)
                                    }
                                    if (module.config == null) {
                                        runCatching {
                                            module.config = module.name.asModuleConfig
                                        }.onFailure { e ->
                                            Log.e(TAG, "Failed to load config from name for module ${module.id}", e)
                                        }
                                    }
                                    if (module.config == null) {
                                        runCatching {
                                            module.config = module.description.asModuleConfig
                                        }.onFailure { e ->
                                            Log.e(TAG, "Failed to load config from description for module ${module.id}", e)
                                        }
                                    }
                                    if (module.config == null) {
                                        module.config = ModuleConfig()
                                    }
                                } catch (e: Exception) {
                                    Log.e(TAG, "Failed to load any config for module ${module.id}", e)
                                    module.config = ModuleConfig()
                                }
                            }
                        }
                    }

                    withContext(Dispatchers.IO) {
                        if (::moduleSizeCache.isInitialized) {
                            val currentModules = moduleInfos.map { it.dirId }
                            moduleSizeCache.initializeCacheIfNeeded(currentModules)
                        }
                    }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    Log.e(TAG, "fetchModuleList: ", e)
                } finally {
                    isRefreshing = false
                }
                Log.i(TAG, "load cost: ${SystemClock.elapsedRealtime() - start}, modules: $modules")
            }
        }
    }

    private fun sanitizeVersionString(version: String): String {
        return version.replace(Regex("[^a-zA-Z0-9.\\-_]"), "_")
    }

    private suspend fun checkUpdate(m: ModuleInfo): ModuleUpdateInfo {
        val empty = ModuleUpdateInfo()
        if (m.updateJson.isEmpty() || m.remove || m.update || !m.enabled) {
            return empty
        }
        val result = kotlin.runCatching {
            val url = m.updateJson
            Log.i(TAG, "checkUpdate url: $url")

            val request = okhttp3.Request.Builder()
                .url(url)
                .header("User-Agent", CUSTOM_USER_AGENT)
                .build()

            updateClient.newCall(request).execute().use { response ->
                Log.d(TAG, "checkUpdate code: ${response.code}")
                if (response.isSuccessful) response.body.string() else ""
            }
        }.getOrElse { e ->
            Log.e(TAG, "checkUpdate exception", e)
            ""
        }

        Log.i(TAG, "checkUpdate result: $result")

        if (result.isEmpty()) {
            return empty
        }

        val updateJson = kotlin.runCatching {
            JSONObject(result)
        }.getOrNull() ?: return empty

        var version = updateJson.optString("version", "")
        version = sanitizeVersionString(version)
        val versionCode = updateJson.optInt("versionCode", 0)
        val zipUrl = updateJson.optString("zipUrl", "")
        val changelog = updateJson.optString("changelog", "")
        if (versionCode <= m.versionCode || zipUrl.isEmpty()) {
            return empty
        }

        val verification = ModuleRepositoryProvider.get().verifyUpdate(m.id, m.updateJson, zipUrl, versionCode)
        return ModuleUpdateInfo(zipUrl, version, changelog, verification)
    }
}

fun ModuleViewModel.ModuleInfo.copy(
    id: String = this.id,
    name: String = this.name,
    author: String = this.author,
    version: String = this.version,
    versionCode: Int = this.versionCode,
    description: String = this.description,
    enabled: Boolean = this.enabled,
    update: Boolean = this.update,
    remove: Boolean = this.remove,
    updateJson: String = this.updateJson,
    hasWebUi: Boolean = this.hasWebUi,
    hasActionScript: Boolean = this.hasActionScript,
    metamodule: Boolean = this.metamodule,
    dirId: String = this.dirId,
    actionIconPath: String? = this.actionIconPath,
    webUiIconPath: String? = this.webUiIconPath,
    runtimeKinds: Set<ModuleRuntimeKind> = this.runtimeKinds,
    zygiskConflict: Boolean = this.zygiskConflict,
    config: ModuleConfig? = this.config,
    preinit: String = this.preinit,
): ModuleViewModel.ModuleInfo {
    return ModuleViewModel.ModuleInfo(
        id, name, author, version, versionCode, description,
        enabled, update, remove, updateJson, hasWebUi, hasActionScript, metamodule,
        dirId, actionIconPath, webUiIconPath, runtimeKinds, zygiskConflict, config, preinit
    )
}

class ModuleSizeCache(context: Context) {
    companion object {
        private const val TAG = "ModuleSizeCache"
        private const val CACHE_PREFS_NAME = "module_size_cache"
        private const val CACHE_VERSION_KEY = "cache_version"
        private const val CACHE_INITIALIZED_KEY = "cache_initialized"
        private const val CURRENT_CACHE_VERSION = 1
    }

    private val cachePrefs = context.getSharedPreferences(CACHE_PREFS_NAME, Context.MODE_PRIVATE)
    private val sizeCache = mutableStateMapOf<String, Long>()

    init {
        loadCacheFromPrefs()
    }

    private fun loadCacheFromPrefs() {
        try {
            val cacheVersion = cachePrefs.getInt(CACHE_VERSION_KEY, 0)
            if (cacheVersion != CURRENT_CACHE_VERSION) {
                Log.d(TAG, "缓存版本不匹配，清空缓存")
                clearCache()
                return
            }

            val allEntries = cachePrefs.all
            for ((key, value) in allEntries) {
                if (key != CACHE_VERSION_KEY && key != CACHE_INITIALIZED_KEY && value is Long) {
                    sizeCache[key] = value
                }
            }
            Log.d(TAG, "从缓存加载了 ${sizeCache.size} 个模块大小数据")
        } catch (e: Exception) {
            Log.e(TAG, "加载缓存失败", e)
            clearCache()
        }
    }

    private fun saveCacheToPrefs() {
        try {
            cachePrefs.edit {
                putInt(CACHE_VERSION_KEY, CURRENT_CACHE_VERSION)
                putBoolean(CACHE_INITIALIZED_KEY, true)

                for ((dirId, size) in sizeCache) {
                    putLong(dirId, size)
                }

            }
            Log.d(TAG, "保存了 ${sizeCache.size} 个模块大小到缓存")
        } catch (e: Exception) {
            Log.e(TAG, "保存缓存失败", e)
        }
    }

    fun getModuleSize(dirId: String): Long {
        return sizeCache[dirId] ?: 0L
    }

    fun initializeCacheIfNeeded(currentModules: List<String>) {
        val isInitialized = cachePrefs.getBoolean(CACHE_INITIALIZED_KEY, false)
        if (!isInitialized || sizeCache.isEmpty()) {
            Log.d(TAG, "首次初始化缓存，计算所有模块大小")
            refreshCache(currentModules)
        } else {
            val newModules = currentModules.filter { !sizeCache.containsKey(it) }
            if (newModules.isNotEmpty()) {
                Log.d(TAG, "发现 ${newModules.size} 个新模块，计算大小: $newModules")
                for (dirId in newModules) {
                    val size = calculateModuleFolderSize(dirId)
                    sizeCache[dirId] = size
                    Log.d(TAG, "新模块 $dirId 大小: ${formatFileSize(size)}")
                }
                saveCacheToPrefs()
            }
        }
    }

    fun refreshCache(currentModules: List<String>) {
        try {
            val toRemove = sizeCache.keys.filter { it !in currentModules }
            toRemove.forEach { sizeCache.remove(it) }

            if (toRemove.isNotEmpty()) {
                Log.d(TAG, "清理了 ${toRemove.size} 个不存在的模块缓存: $toRemove")
            }

            for (dirId in currentModules) {
                val size = calculateModuleFolderSize(dirId)
                sizeCache[dirId] = size
                Log.d(TAG, "更新模块 $dirId 大小: ${formatFileSize(size)}")
            }

            saveCacheToPrefs()
        } catch (e: Exception) {
            Log.e(TAG, "刷新缓存失败", e)
        }
    }

    private fun clearCache() {
        sizeCache.clear()
        cachePrefs.edit { clear() }
        Log.d(TAG, "清空所有缓存")
    }

    private fun calculateModuleFolderSize(dirId: String): Long {
        return try {
            val shell = getRootShell()
            val command = "/data/adb/ksu/bin/busybox du -sb /data/adb/modules/$dirId"
            val result = shell.newJob().add(command).to(ArrayList(), null).exec()

            if (result.isSuccess && result.out.isNotEmpty()) {
                val sizeStr = result.out.firstOrNull()?.split("\t")?.firstOrNull()
                sizeStr?.toLongOrNull() ?: 0L
            } else {
                0L
            }
        } catch (e: Exception) {
            Log.e(TAG, "计算模块大小失败 $dirId: ${e.message}")
            0L
        }
    }
}

private fun JSONObject.getBooleanCompat(key: String, default: Boolean = false): Boolean {
    if (!has(key)) return default
    return when (val value = opt(key)) {
        null -> default
        is Boolean -> value
        is String -> value.equals("true", ignoreCase = true) || value == "1"
        is Number -> value.toInt() != 0
        else -> default
    }
}

private fun JSONObject.getIntCompat(key: String, default: Int = 0): Int {
    if (!has(key)) return default
    return when (val value = opt(key)) {
        null -> default
        is Int -> value
        is Number -> value.toInt()
        is String -> value.toIntOrNull() ?: default
        else -> default
    }
}

fun formatFileSize(bytes: Long): String {
    if (bytes <= 0) return "0 KB"

    val units = arrayOf("B", "KB", "MB", "GB", "TB")
    val digitGroups = (log10(bytes.toDouble()) / log10(1024.0)).toInt()

    return DecimalFormat("#,##0.#").format(
        bytes / 1024.0.pow(digitGroups.toDouble())
    ) + " " + units[digitGroups]
}
