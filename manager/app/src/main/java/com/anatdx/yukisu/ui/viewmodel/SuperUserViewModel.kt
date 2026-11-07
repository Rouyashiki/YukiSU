package com.anatdx.yukisu.ui.viewmodel

import android.content.*
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.graphics.drawable.Drawable
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import androidx.compose.runtime.*
import androidx.core.content.edit
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.BackupPackage
import com.anatdx.yukisu.ksuApp
import com.anatdx.yukisu.ui.KsuService
import com.anatdx.yukisu.ui.util.*
import com.anatdx.yukisu.ui.util.module.AllowlistBackup
import com.anatdx.yukisu.ui.util.module.AllowlistRestore
import com.anatdx.yukisu.ui.util.module.AllowlistPlan
import com.anatdx.yukisu.ui.util.module.AllowlistPackageReader
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.text.Collator
import java.util.*
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import com.anatdx.yukisu.IKsuInterface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.parcelize.IgnoredOnParcel
import kotlinx.parcelize.Parcelize

enum class AppCategory(val displayNameRes: Int, val persistKey: String) {
    ALL(com.anatdx.yukisu.R.string.category_all_apps, "ALL"),
    ROOT(com.anatdx.yukisu.R.string.category_root_apps, "ROOT"),
    CUSTOM(com.anatdx.yukisu.R.string.category_custom_apps, "CUSTOM"),
    DEFAULT(com.anatdx.yukisu.R.string.category_default_apps, "DEFAULT");

    companion object {
        fun fromPersistKey(key: String): AppCategory = entries.find { it.persistKey == key } ?: ALL
    }
}

enum class SortType(val displayNameRes: Int, val persistKey: String) {
    NAME_ASC(com.anatdx.yukisu.R.string.sort_name_asc, "NAME_ASC"),
    NAME_DESC(com.anatdx.yukisu.R.string.sort_name_desc, "NAME_DESC"),
    INSTALL_TIME_NEW(com.anatdx.yukisu.R.string.sort_install_time_new, "INSTALL_TIME_NEW"),
    INSTALL_TIME_OLD(com.anatdx.yukisu.R.string.sort_install_time_old, "INSTALL_TIME_OLD");

    companion object {
        fun fromPersistKey(key: String): SortType = entries.find { it.persistKey == key } ?: NAME_ASC
    }
}

class SuperUserViewModel : ViewModel() {
    companion object {
        private const val TAG = "SuperUserViewModel"
        const val SHELL_UID = 2000
        const val WEBVIEW_ZYGOTE_UID = 1053
        const val WEBVIEW_ZYGOTE_PROFILE_KEY = "webview_zygote"
        private val appsLock = Any()
        var apps by mutableStateOf<List<AppInfo>>(emptyList())
        private val _isAppListLoaded = MutableStateFlow(false)
        val isAppListLoaded = _isAppListLoaded.asStateFlow()

        @JvmStatic
        fun getAppIconDrawable(context: Context, packageName: String): Drawable? {
            val appList = cachedPackageApps ?: synchronized(appsLock) { apps }
            return appList.find { it.packageName == packageName }
                ?.packageInfo?.applicationInfo?.loadIcon(context.packageManager)
        }

        var appGroups by mutableStateOf<List<AppGroup>>(emptyList())

        private const val PREFS_NAME = "settings"
        private const val KEY_SHOW_SYSTEM_APPS = "show_system_apps"
        private const val KEY_SELECTED_CATEGORY = "selected_category"
        private const val KEY_CURRENT_SORT_TYPE = "current_sort_type"
        private const val CORE_POOL_SIZE = 8
        private const val MAX_POOL_SIZE = 16
        private const val KEEP_ALIVE_TIME = 60L
        private const val BATCH_SIZE = 20
        private const val PER_USER_RANGE = 100000
        private val fetchAppListMutex = Mutex()
        private val allowlistBackupMutex = Mutex()
        private val ksuServiceLock = Any()
        // Keep one process-wide binding so ordinary reads reuse the service package snapshot.
        private var ksuServiceBinder: IBinder? = null
        private var ksuServiceConnection: ServiceConnection? = null

        private fun clearKsuServiceConnection(connection: ServiceConnection) {
            synchronized(ksuServiceLock) {
                if (ksuServiceConnection === connection) {
                    ksuServiceBinder = null
                    ksuServiceConnection = null
                }
            }
        }

        private suspend fun connectKsuService(): IBinder? = withContext(Dispatchers.Main.immediate) {
            var staleConnection: ServiceConnection? = null
            val cachedBinder = synchronized(ksuServiceLock) {
                ksuServiceBinder?.takeIf { it.isBinderAlive } ?: run {
                    ksuServiceBinder = null
                    staleConnection = ksuServiceConnection
                    ksuServiceConnection = null
                    null
                }
            }
            if (cachedBinder != null) return@withContext cachedBinder

            staleConnection?.let { connection ->
                try {
                    com.topjohnwu.superuser.ipc.RootService.unbind(connection)
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to unbind stale KsuService connection", e)
                }
            }

            suspendCancellableCoroutine<IBinder?> { continuation ->
                val connection = KsuServiceConnection(continuation)
                synchronized(ksuServiceLock) {
                    ksuServiceConnection = connection
                }
                val intent = Intent(ksuApp, KsuService::class.java)
                try {
                    val task = com.topjohnwu.superuser.ipc.RootService.bindOrTask(
                        intent, Shell.EXECUTOR, connection
                    )
                    task?.let { Shell.getShell().execTask(it) }
                } catch (e: Exception) {
                    connection.fail()
                    Log.e(TAG, "Failed to bind KsuService", e)
                }
            }
        }

        private suspend fun queryPackageApps(
            forceRefresh: Boolean,
            onProgress: (Float) -> Unit = {},
        ): List<AppInfo>? {
            if (!withContext(Dispatchers.IO) { KsuCli.SHELL.isRoot }) {
                Log.w(TAG, "Root access is required to load the app list")
                return null
            }

            val binder = connectKsuService() ?: return null

            return withContext(Dispatchers.IO) {
                val pm = ksuApp.packageManager
                val allPackages = IKsuInterface.Stub.asInterface(binder)
                val total = if (forceRefresh) {
                    allPackages.refreshPackages()
                } else {
                    allPackages.packageCount
                }
                val pageSize = 100
                val result = mutableListOf<AppInfo>()

                var start = 0
                while (start < total) {
                    val page = allPackages.getPackages(start, pageSize)
                    if (page.isEmpty()) break

                    result += page.mapNotNull { packageInfo ->
                        packageInfo.applicationInfo?.takeIf { it.uid != WEBVIEW_ZYGOTE_UID }?.let { appInfo ->
                            AppInfo(
                                label = appInfo.loadLabel(pm).toString(),
                                packageInfo = packageInfo,
                                profile = null
                            )
                        }
                    }
                    start += page.size
                    onProgress(start.toFloat() / total)
                }

                result.toList()
            }
        }

        @Volatile
        private var cachedPackageApps: List<AppInfo>? = null

        internal fun getAppsForWebUI(): List<AppInfo> = cachedPackageApps.orEmpty()

        suspend fun loadAppsForWebUI(): List<AppInfo> = cachedPackageApps
            ?: fetchAppListMutex.withLock {
                cachedPackageApps ?: queryPackageApps(forceRefresh = false)
                    ?.also { cachedPackageApps = it }.orEmpty()
            }

        private class KsuServiceConnection(
            private var continuation: CancellableContinuation<IBinder?>?
        ) : ServiceConnection {
            override fun onServiceDisconnected(name: ComponentName?) {
                clearKsuServiceConnection(this)
                resume(null)
            }

            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                if (binder == null) {
                    clearKsuServiceConnection(this)
                } else {
                    synchronized(ksuServiceLock) {
                        if (ksuServiceConnection === this) {
                            ksuServiceBinder = binder
                        }
                    }
                }
                resume(binder)
            }

            override fun onNullBinding(name: ComponentName?) {
                clearKsuServiceConnection(this)
                resume(null)
            }

            override fun onBindingDied(name: ComponentName?) {
                clearKsuServiceConnection(this)
                resume(null)
            }

            fun fail() {
                clearKsuServiceConnection(this)
                resume(null)
            }

            private fun resume(binder: IBinder?) {
                val pending = synchronized(this) {
                    continuation.also { continuation = null }
                } ?: return
                pending.resume(binder)
            }
        }
    }

    @Immutable
    @Parcelize
    data class AppInfo(
        val label: String,
        val packageInfo: PackageInfo,
        val profile: Natives.Profile?,
        val special: Boolean = false,
    ) : Parcelable {
        @IgnoredOnParcel
        val packageName: String = packageInfo.packageName
        @IgnoredOnParcel
        val uid: Int = packageInfo.applicationInfo!!.uid
        @IgnoredOnParcel
        val isWebViewZygote: Boolean = special && uid == WEBVIEW_ZYGOTE_UID
        @IgnoredOnParcel
        val profileKey: String = if (isWebViewZygote) WEBVIEW_ZYGOTE_PROFILE_KEY
            else profile?.name?.takeIf { it.isNotBlank() } ?: packageName
        @IgnoredOnParcel
        val displayIdentifier: String = if (special) profileKey else packageName
    }

    @Immutable
    @Parcelize
    data class AppGroup(
        val uid: Int,
        val apps: List<AppInfo>,
        val profile: Natives.Profile?,
        val dynamicManagerFlags: Int = 0
    ) : Parcelable {
        @IgnoredOnParcel
        val mainApp: AppInfo = apps.first()
        @IgnoredOnParcel
        val displayIdentifiers: List<String> = apps.map { it.displayIdentifier }
        @IgnoredOnParcel
        val profileKey: String = mainApp.profileKey
        @IgnoredOnParcel
        val allowSu: Boolean = !mainApp.special && profile?.allowSu == true
        @IgnoredOnParcel
        val isPresetManager: Boolean =
            dynamicManagerFlags and Natives.DYNAMIC_MANAGER_FLAG_PRESET != 0
        @IgnoredOnParcel
        val isDynamicManager: Boolean =
            dynamicManagerFlags and Natives.DYNAMIC_MANAGER_FLAG_TRUSTED != 0
        @IgnoredOnParcel
        val userName: String? = Natives.getUserName(uid)
        @IgnoredOnParcel
        val hasCustomProfile : Boolean = profile?.let { if (allowSu) !it.rootUseDefault else !it.nonRootUseDefault } ?: false
        @IgnoredOnParcel
        internal val sortKey = AppGroupSortKey(
            label = mainApp.label,
            priority = when {
                isDynamicManager -> 0
                uid == SHELL_UID -> 1
                allowSu -> 2
                hasCustomProfile -> 3
                else -> 4
            },
            latestInstallTime = apps.maxOf { it.packageInfo.firstInstallTime },
            latestUpdateTime = apps.maxOf { it.packageInfo.lastUpdateTime },
        )
    }

    private val appProcessingThreadPool = ThreadPoolExecutor(
        CORE_POOL_SIZE, MAX_POOL_SIZE, KEEP_ALIVE_TIME, TimeUnit.SECONDS,
        LinkedBlockingQueue()
    ) { runnable ->
        Thread(runnable, "AppProcessing-${System.currentTimeMillis()}").apply {
            isDaemon = true
            priority = Thread.NORM_PRIORITY
        }
    }.asCoroutineDispatcher()

    private val appListMutex = Mutex()
    private val configChangeListeners = mutableSetOf<(String) -> Unit>()
    private val prefs = ksuApp.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    var search by mutableStateOf("")
    var showSystemApps by mutableStateOf(prefs.getBoolean(KEY_SHOW_SYSTEM_APPS, false))
        private set
    var selectedCategory by mutableStateOf(loadSelectedCategory())
        private set
    var currentSortType by mutableStateOf(loadCurrentSortType())
        private set
    var isRefreshing by mutableStateOf(false)
        private set
    var showBatchActions by mutableStateOf(false)
        internal set
    var selectedApps by mutableStateOf<Set<String>>(emptySet())
        internal set
    var loadingProgress by mutableFloatStateOf(0f)
        private set

    private fun loadSelectedCategory(): AppCategory {
        val categoryKey = prefs.getString(KEY_SELECTED_CATEGORY, AppCategory.ALL.persistKey)
            ?: AppCategory.ALL.persistKey
        return AppCategory.fromPersistKey(categoryKey)
    }

    private fun loadCurrentSortType(): SortType {
        val sortKey = prefs.getString(KEY_CURRENT_SORT_TYPE, SortType.NAME_ASC.persistKey)
            ?: SortType.NAME_ASC.persistKey
        return SortType.fromPersistKey(sortKey)
    }

    fun updateShowSystemApps(newValue: Boolean) {
        showSystemApps = newValue
        prefs.edit { putBoolean(KEY_SHOW_SYSTEM_APPS, newValue) }
        notifyAppListChanged()
    }

    private fun notifyAppListChanged() {
        val currentApps = apps
        apps = emptyList()
        apps = currentApps
    }

    fun updateSelectedCategory(newCategory: AppCategory) {
        selectedCategory = newCategory
        prefs.edit { putString(KEY_SELECTED_CATEGORY, newCategory.persistKey) }
    }

    fun updateCurrentSortType(newSortType: SortType) {
        currentSortType = newSortType
        prefs.edit { putString(KEY_CURRENT_SORT_TYPE, newSortType.persistKey) }
    }

    fun toggleBatchMode() {
        showBatchActions = !showBatchActions
        if (!showBatchActions) clearSelection()
    }

    fun toggleAppSelection(packageName: String) {
        selectedApps = if (selectedApps.contains(packageName)) {
            selectedApps - packageName
        } else {
            selectedApps + packageName
        }
    }

    fun clearSelection() {
        selectedApps = emptySet()
    }

    suspend fun updateBatchPermissions(allowSu: Boolean, umountModules: Boolean? = null) {
        val selectedUids = apps.asSequence()
            .filter { it.displayIdentifier in selectedApps && (!allowSu || !it.special) }
            .map { it.uid }
            .toSet()

        selectedUids.forEach { uid ->
            appGroups.find { it.uid == uid }?.let { group ->
                val profile = Natives.getAppProfile(group.profileKey, uid)
                val updatedProfile = profile.copy(
                    allowSu = allowSu,
                    umountModules = umountModules ?: profile.umountModules,
                    nonRootUseDefault = false
                )
                if (Natives.setAppProfile(updatedProfile)) {
                    updateUidProfileLocally(uid, updatedProfile)
                    notifyConfigChange(group.profileKey)
                }
            }
        }
        clearSelection()
        showBatchActions = false
        refreshAppConfigurations()
    }

    suspend fun updateGroupPermission(
        appGroup: AppGroup,
        allowSu: Boolean,
        umountModules: Boolean? = null
    ): Boolean {
        if (appGroup.mainApp.special && allowSu) return false
        val profile = Natives.getAppProfile(appGroup.profileKey, appGroup.uid)
        val updatedProfile = profile.copy(
            allowSu = allowSu,
            umountModules = umountModules ?: profile.umountModules,
            nonRootUseDefault = if (!allowSu && umountModules != null) {
                if (appGroup.mainApp.special) false else !umountModules
            } else {
                profile.nonRootUseDefault
            }
        )
        val updated = Natives.setAppProfile(updatedProfile)

        if (updated) {
            updateUidProfileLocally(appGroup.uid, updatedProfile)
            notifyConfigChange(appGroup.profileKey)
            refreshAppConfigurations()
        }

        return updated
    }

    fun updateUidProfileLocally(uid: Int, updatedProfile: Natives.Profile) {
        appListMutex.tryLock().let { locked ->
            if (locked) {
                try {
                    apps = apps.map { app ->
                        if (app.uid == uid) {
                            app.copy(profile = updatedProfile)
                        } else app
                    }
                    appGroups = groupAppsByUid(apps)
                } finally {
                    appListMutex.unlock()
                }
            }
        }
    }

    private fun notifyConfigChange(packageName: String) {
        configChangeListeners.forEach { listener ->
            try {
                listener(packageName)
            } catch (e: Exception) {
                Log.e(TAG, "Error notifying config change for $packageName", e)
            }
        }
    }

    suspend fun refreshAppConfigurations() {
        withContext(appProcessingThreadPool) {
            supervisorScope {
                val currentApps = apps.toList()
                val uidGroups = currentApps.groupBy { it.uid }.values.toList()
                val batches = uidGroups.chunked(BATCH_SIZE)
                loadingProgress = 0f

                val updatedApps = batches.mapIndexed { batchIndex, batch ->
                    async {
                        val batchResult = batch.flatMap { uidApps ->
                            try {
                                val profile = loadUidProfile(uidApps)
                                uidApps.map { it.copy(profile = profile) }
                            } catch (e: Exception) {
                                Log.e(TAG, "Error refreshing profile for uid ${uidApps.first().uid}", e)
                                uidApps
                            }
                        }
                        loadingProgress = (batchIndex + 1).toFloat() / batches.size.coerceAtLeast(1)
                        batchResult
                    }
                }.awaitAll().flatten()

                appListMutex.withLock {
                    apps = updatedApps
                    appGroups = groupAppsByUid(updatedApps)
                }
                loadingProgress = 1f
            }
        }
    }

    suspend fun fetchAppList(forceRefresh: Boolean = false) {
        isRefreshing = true
        loadingProgress = 0f

        try {
            fetchAppListMutex.withLock {
                fetchAppListFromService(forceRefresh)
            }
        } finally {
            isRefreshing = false
        }
    }

    internal suspend fun exportAllowlist(): String = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            val service = backupService()
            val profiles = readAllowlistSnapshot()
            val protected = protectedProfileAppIds()
            val ordinary = profiles.values.filter {
                it.currentUid != AllowlistRestore.DEFAULT_UID &&
                    it.currentUid % PER_USER_RANGE !in protected
            }
            val inventory = AllowlistPackageReader.read(service) { ordinary.map { it.currentUid }.toSet() }
            val rules = AllowlistPackageReader.rules(service, ordinary.map { it.name }.toSet())
            val entries = ordinary.map { profile ->
                check(inventory.identifies(profile)) { "Cannot read package identity: ${profile.name} (${profile.currentUid})" }
                val packages = inventory.byUid.getValue(profile.currentUid)
                AllowlistBackup.Entry(
                    packages.map { it.packageName }, profile.copy(rules = rules.getValue(profile.name)),
                    packages.associate { it.packageName to it.currentSigners }
                )
            }
            val document = AllowlistBackup.Document(
                profiles[AllowlistRestore.DEFAULT_UID]?.umountModules ?: false, entries
            )
            AllowlistBackup.validateAgainst(
                document,
                entries.associate { it.profile.currentUid to it.packages },
                protected
            )
            check(readAllowlistSnapshot() == profiles) { "Profiles changed during backup; retry" }
            check(protectedProfileAppIds() == protected) { "Manager identities changed during backup; retry" }
            ordinary.forEach { profile ->
                check(service.getBackupUidIdentity(profile.currentUid) == inventory.byUid[profile.currentUid]) {
                    "Package changed during backup: ${profile.name}"
                }
            }
            AllowlistBackup.encode(document)
        }
    }

    internal suspend fun prepareAllowlist(
        document: AllowlistBackup.Document,
        candidateUsers: Set<Int> = emptySet()
    ): AllowlistPlan.Context = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            prepareAllowlist(document, candidateUsers, backupService())
        }
    }

    private suspend fun prepareAllowlist(
        document: AllowlistBackup.Document,
        candidateUsers: Set<Int>,
        service: IKsuInterface
    ): AllowlistPlan.Context {
        val started = android.os.SystemClock.elapsedRealtime()
        val before = readAllowlistSnapshot()
        val protected = protectedProfileAppIds()
        val inventory = AllowlistPackageReader.read(service) { installed ->
            val exact = document.apps.flatMap { entry ->
                entry.packages.mapNotNull { installed.byName[entry.profile.currentUid / PER_USER_RANGE to it]?.uid }
            }
            val required = before.keys + exact + installed.packages.filter {
                it.userId in candidateUsers && installed.byUid[it.uid]?.size == 1 && it.uid % PER_USER_RANGE !in protected
            }.map { it.uid }
            val ruleNames = required.flatMap { installed.byUid[it].orEmpty().map { app -> app.packageName } }.toSet()
            required + installed.packages.filter { it.packageName in ruleNames }.map { it.uid }
        }
        val keys = before.values.filter { it.currentUid != AllowlistRestore.DEFAULT_UID }.map { it.name }.toSet() +
            inventory.packages.filter { it.status == BackupPackage.OK }.map { it.packageName }
        val rules = AllowlistPackageReader.rules(service, keys)
        check(readAllowlistSnapshot() == before && protectedProfileAppIds() == protected) { "Profiles changed while preparing restore; retry" }
        Log.i(TAG, "Restore preparation: ${document.apps.size} entries, ${android.os.SystemClock.elapsedRealtime() - started} ms")
        return AllowlistPlan.Context(document, inventory, before, rules, protected, candidateUsers)
    }

    internal suspend fun restoreAllowlist(plan: AllowlistPlan.Plan): AllowlistPlan.Context? = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            val service = backupService()
            val fresh = prepareAllowlist(plan.context.document, plan.context.candidateUsers, service)
            val freshPlan = AllowlistPlan.create(fresh, plan.options)
            if (!plan.sameExecution(freshPlan)) return@withLock fresh
            check(plan.canApply) { "No changes selected" }
            currentCoroutineContext().ensureActive()
            withContext(NonCancellable) {
                val started = android.os.SystemClock.elapsedRealtime()
                try {
                    AllowlistRestore.apply(plan, object : AllowlistRestore.Store {
                        override fun read(uid: Int) = Natives.readAppProfile(uid)
                        override fun write(profile: Natives.Profile) = Natives.setAppProfile(profile)
                        override fun readRules(key: String) = service.readProfileRulesForBackup(key)
                        override fun writeRules(key: String, rules: String) = service.writeProfileRulesForBackup(key, rules)
                        override fun identitiesMatch(uids: Set<Int>): Boolean {
                            if (protectedProfileAppIds() != plan.context.protected) return false
                            return uids.all { uid ->
                                val expected = plan.context.installed.byUid[uid] ?: return@all false
                                expected.all { it.status == BackupPackage.OK } && service.getBackupUidIdentity(uid) == expected
                            }
                        }
                    })
                } finally {
                    Log.i(TAG, "Restore execution: ${plan.profiles.size} profiles, ${plan.rules.size} rules, ${android.os.SystemClock.elapsedRealtime() - started} ms")
                    viewModelScope.launch {
                        runCatching {
                            withTimeout(30_000) { refreshAppConfigurations() }
                        }.onFailure { error ->
                            Log.e(TAG, "Failed to refresh app configurations after restore", error)
                        }
                    }
                }
            }
            null
        }
    }

    private suspend fun backupService(): IKsuInterface {
        check(Natives.isManager && !Natives.checkUapiMismatch()) { "Compatible manager/kernel access is required" }
        check(KsuCli.SHELL.isRoot) { "Root access is required" }
        return IKsuInterface.Stub.asInterface(connectKsuService() ?: error("Package service is unavailable"))
    }

    private fun protectedProfileAppIds(): Set<Int> =
        Natives.getProtectedProfileAppIds().toSet() + (ksuApp.applicationInfo.uid % PER_USER_RANGE) +
            WEBVIEW_ZYGOTE_UID

    private fun readAllowlistSnapshot(): Map<Int, Natives.Profile> {
        val uids = (Natives.getProfileUids(true).toList() + Natives.getProfileUids(false).toList()).sorted()
        check(uids.distinct().size == uids.size) { "Profile list changed while reading" }
        val result = uids.associateWith { uid -> Natives.readAppProfile(uid) ?: error("Profile disappeared: $uid") }.toMutableMap()
        val defaults = Natives.readAppProfile(AllowlistRestore.DEFAULT_UID)
        if (defaults != null) {
            check(defaults.name == AllowlistRestore.DEFAULT_KEY && !defaults.allowSu) {
                "Invalid default umount profile"
            }
            result[AllowlistRestore.DEFAULT_UID] = defaults
        } else {
            check(AllowlistRestore.DEFAULT_UID !in result) { "Default profile changed while reading" }
        }
        return result
    }

    private suspend fun fetchAppListFromService(forceRefresh: Boolean) {
        val packages = queryPackageApps(forceRefresh) { loadingProgress = it } ?: return
        cachedPackageApps = packages
        withContext(Dispatchers.IO) {
            val pm = ksuApp.packageManager
            val result = packages.toMutableList()

            val systemInfo = ApplicationInfo(pm.getApplicationInfo("android", 0)).apply {
                uid = WEBVIEW_ZYGOTE_UID
            }
            result += AppInfo(
                label = "WebView Zygote",
                packageInfo = PackageInfo().apply {
                    packageName = ""
                    applicationInfo = systemInfo
                },
                profile = null,
                special = true,
            )

            synchronized(appsLock) {
                _isAppListLoaded.value = true
            }

            appListMutex.withLock {
                val filteredApps = result.filter { it.packageName != ksuApp.packageName }
                val profiledApps = filteredApps.groupBy { it.uid }.values.flatMap { uidApps ->
                    val profile = loadUidProfile(uidApps)
                    uidApps.map { it.copy(profile = profile) }
                }
                apps = profiledApps
                appGroups = groupAppsByUid(profiledApps)
            }
            loadingProgress = 1f
        }
    }

    val appGroupList by derivedStateOf {
        appGroups.filter { group ->
            group.apps.any { app ->
                app.label.contains(search, true) ||
                        app.displayIdentifier.contains(search, true) ||
                        HanziToPinyin.getInstance().toPinyinString(app.label)?.contains(search, true) == true
            }
        }.filter { group ->
            group.mainApp.isWebViewZygote || group.uid == SHELL_UID || showSystemApps ||
                    group.apps.any { it.packageInfo.applicationInfo!!.flags.and(ApplicationInfo.FLAG_SYSTEM) == 0 }
        }
    }

    private fun loadDynamicManagerFlags(): Map<Int, Int> {
        val items = runCatching { Natives.getDynamicManagers() }.getOrDefault(IntArray(0))
        if (items.size < 2) return emptyMap()

        val result = mutableMapOf<Int, Int>()
        var index = 0
        while (index + 1 < items.size) {
            val appId = items[index]
            val flags = items[index + 1]
            result[appId] = flags
            index += 2
        }
        return result
    }

    private fun groupAppsByUid(appList: List<AppInfo>): List<AppGroup> {
        val dynamicManagers = loadDynamicManagerFlags()

        return appList.groupBy { it.uid }
            .map { (uid, apps) ->
                val sortedApps = apps.sortedBy { it.label }
                val profile = apps.firstOrNull()?.profile
                val dynamicManagerFlags = dynamicManagers[uid % PER_USER_RANGE] ?: 0
                AppGroup(
                    uid = uid,
                    apps = sortedApps,
                    profile = profile,
                    dynamicManagerFlags = dynamicManagerFlags
                )
            }
            .sortedWith(
                compareBy<AppGroup> {
                    when {
                        it.isDynamicManager -> 0
                        it.uid == SHELL_UID -> 1
                        it.allowSu -> 2
                        it.hasCustomProfile -> 3
                        else -> 4
                    }
                }.thenBy(Collator.getInstance(Locale.getDefault())) {
                    it.userName?.takeIf { name -> name.isNotBlank() } ?: it.uid.toString()
                }.thenBy(Collator.getInstance(Locale.getDefault())) { it.mainApp.label }
            )
    }

    private fun loadUidProfile(uidApps: Collection<AppInfo>): Natives.Profile {
        val first = uidApps.first()
        if (first.isWebViewZygote) {
            return Natives.getAppProfile(WEBVIEW_ZYGOTE_PROFILE_KEY, WEBVIEW_ZYGOTE_UID)
                .copy(name = WEBVIEW_ZYGOTE_PROFILE_KEY, allowSu = false)
        }
        val packageNames = uidApps.mapTo(mutableSetOf()) { it.packageName }
        val fallbackKey = packageNames.min()
        val profile = Natives.getAppProfile(fallbackKey, first.uid)
        return if (profile.name in packageNames) profile else profile.copy(name = fallbackKey)
    }
    override fun onCleared() {
        try {
            appProcessingThreadPool.close()
            configChangeListeners.clear()
        } catch (e: Exception) {
            Log.e(TAG, "Error cleaning up resources", e)
        }
    }
}
