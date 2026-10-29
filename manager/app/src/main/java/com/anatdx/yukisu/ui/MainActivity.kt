package com.anatdx.yukisu.ui

import com.anatdx.yukisu.ui.screen.scriptrun.ScriptRunArg
import com.anatdx.yukisu.data.script.ScriptRepository
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.core.net.toUri
import androidx.lifecycle.lifecycleScope
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.ramcosta.composedestinations.DestinationsNavHost
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.generated.destinations.ExecuteModuleActionScreenDestination
import com.ramcosta.composedestinations.generated.destinations.HomeScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ScriptRunScreenDestination
import com.ramcosta.composedestinations.spec.NavHostGraphSpec
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.integrity.KsudIntegrity
import com.anatdx.yukisu.ui.activity.component.ManagerScaffold
import com.anatdx.yukisu.ui.activity.util.DataRefreshUtils
import com.anatdx.yukisu.ui.activity.util.IntentDispatcher
import com.anatdx.yukisu.ui.activity.util.DisplayUtils
import com.anatdx.yukisu.ui.activity.util.LocalPredictiveBackEnabled
import com.anatdx.yukisu.ui.activity.util.predictiveBackSurfaces
import com.anatdx.yukisu.ui.activity.util.rememberPredictiveBackEnabled
import com.anatdx.yukisu.ui.activity.util.rememberPredictiveBackNavHostEngine
import com.anatdx.yukisu.ui.activity.util.ThemeChangeContentObserver
import com.anatdx.yukisu.ui.activity.util.ThemeUtils
import com.anatdx.yukisu.ui.activity.util.UltraActivityUtils
import com.anatdx.yukisu.ui.component.InstallConfirmationDialog
import com.anatdx.yukisu.ui.component.KasumiConflictDialog
import com.anatdx.yukisu.ui.component.ZipFileDetector
import com.anatdx.yukisu.ui.screen.BottomBarDestination
import com.anatdx.yukisu.ui.theme.KernelSUTheme
import com.anatdx.yukisu.ui.theme.ThemeManager
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.ui.util.KsuCli
import com.anatdx.yukisu.ui.util.LocalNavigationLeaveGuard
import com.anatdx.yukisu.ui.util.LocalSnackbarHost
import com.anatdx.yukisu.ui.util.NavigationLeaveGuard
import com.anatdx.yukisu.ui.util.install
import com.anatdx.yukisu.ui.util.migrateFeatureDefaults
import com.anatdx.yukisu.ui.util.resetTaskDescriptionToAppName
import com.anatdx.yukisu.ui.viewmodel.HomeViewModel
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import com.anatdx.yukisu.ui.webui.WebUIActivity
import com.anatdx.yukisu.ui.webui.initPlatform
import kotlinx.coroutines.flow.MutableStateFlow
import com.anatdx.yukisu.ui.util.module.ModuleLink
import com.anatdx.yukisu.ui.util.module.Shortcut
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.moreSettings.util.LocaleHelper
import ui.screen.ramdisk.RamdiskExternalFiles
import java.io.File

class MainActivity : ComponentActivity() {
    private lateinit var superUserViewModel: SuperUserViewModel
    private lateinit var homeViewModel: HomeViewModel
    internal val settingsStateFlow = MutableStateFlow(SettingsState())
    private val intentDispatcher by viewModels<IntentDispatcher>()
    private var pendingZipDecision: CompletableDeferred<Unit>? = null

    data class SettingsState(
        val isHideOtherInfo: Boolean = false
    )

    private lateinit var themeChangeObserver: ThemeChangeContentObserver
    private var isInitialized = false

    override fun attachBaseContext(newBase: Context?) {
        super.attachBaseContext(newBase?.let { LocaleHelper.applyLanguage(it) })
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        try {
            DisplayUtils.applyCustomDpi(this)

            // Enable edge to edge
            enableEdgeToEdge()

            window.isNavigationBarContrastEnforced = false

            super.onCreate(savedInstanceState)
            intentDispatcher.initialize(intent)
            lifecycleScope.launch(Dispatchers.IO) {
                RamdiskExternalFiles(File(cacheDir, "ramdisk_external")).cleanExpired()
            }
            resetTaskDescriptionToAppName()
            ThemeManager.loadUiStyle(this)
            ThemeManager.loadMotionPreset(this)
            ThemeManager.loadPageTransitionStyle(this)

            // Note: ksud installation moved to KsuCli.refreshShells()
            // which is called after SuperKey authentication succeeds.
            // In SuperKey mode, isManager is false until authentication,
            // so we can't install ksud here.

            if (!isInitialized) {
                initializeViewModels()
                initializeData()
                isInitialized = true
            }

            setContent {
                KernelSUTheme {
                    val navController = rememberNavController()
                    val snackBarHostState = rememberSnackbarController()
                    val navigationLeaveGuard = remember { NavigationLeaveGuard() }
                    val predictiveBackEnabled by rememberPredictiveBackEnabled()
                    val currentDestination = navController.currentBackStackEntryAsState().value?.destination

                    val bottomBarRoutes = remember {
                        BottomBarDestination.entries.map { it.direction.route }.toSet()
                    }

                    val navigator = navController.rememberDestinationsNavigator()
                    BackHandler(currentDestination != null && currentDestination.route != HomeScreenDestination.route) {
                        navigationLeaveGuard.navigateOrIntercept(currentDestination?.route) {
                            navigator.navigate(HomeScreenDestination) {
                                navigator.clearBackStack(NavGraphs.root as NavHostGraphSpec)
                                launchSingleTop = true
                            }
                        }
                    }

                    InstallConfirmationDialog(
                        show = intentDispatcher.showConfirmation,
                        zipFiles = intentDispatcher.zipFiles,
                        onConfirm = { confirmedFiles, ak3Options ->
                            intentDispatcher.showConfirmation = false
                            intentDispatcher.zipFiles = emptyList()
                            intentDispatcher.queue.complete()
                            pendingZipDecision?.complete(Unit)
                            UltraActivityUtils.navigateToFlashScreen(
                                this,
                                confirmedFiles,
                                ak3Options,
                                navigator,
                            )
                        },
                        onDismiss = {
                            intentDispatcher.showConfirmation = false
                            ZipFileDetector.cleanupStagedFiles(intentDispatcher.zipFiles)
                            intentDispatcher.zipFiles = emptyList()
                            intentDispatcher.queue.complete()
                            pendingZipDecision?.complete(Unit)
                            finish()
                        }
                    )

                    LaunchedEffect(intentDispatcher) {
                        while (true) {
                            val snapshot = intentDispatcher.queue.next()
                            try {
                                if (snapshot.data?.scheme == "ksu" || snapshot.hasExtra("shortcut_type")) {
                                    val link = resolveModuleIntent(snapshot)
                                    if (link != null && !Natives.isManager) {
                                        navigator.navigate(HomeScreenDestination) { launchSingleTop = true }
                                        intentDispatcher.managerAccess.awaitReady()
                                    }
                                    intentDispatcher.queue.complete()
                                    when (link?.type) {
                                        ModuleLink.Type.Action -> navigator.navigate(
                                            ExecuteModuleActionScreenDestination(link.moduleId, fromShortcut = true)
                                        )
                                        ModuleLink.Type.WebUI -> {
                                            val webIntent = Intent(this@MainActivity, WebUIActivity::class.java)
                                                .setData("kernelsu://webui/${link.moduleId}".toUri())
                                                .putExtra("id", link.moduleId)
                                                .putExtra("name", snapshot.getStringExtra("module_name") ?: link.moduleId)
                                                .putExtra("from_webui_shortcut", true)
                                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                                            startActivity(webIntent)
                                        }
                                        null -> Unit
                                    }
                                    continue
                                }

                                val zipUris = extractZipUris(snapshot)
                                if (zipUris.isNullOrEmpty()) {
                                    intentDispatcher.queue.complete()
                                    continue
                                }
                                ZipFileDetector.cleanupStagedFiles(intentDispatcher.zipFiles)
                                intentDispatcher.zipFiles = emptyList()
                                intentDispatcher.showConfirmation = false
                                val singleUri = zipUris.singleOrNull()
                                val script = singleUri?.let { ScriptRepository.peekOneshot(it) }
                                if (script != null) {
                                    intentDispatcher.queue.complete()
                                    navigator.navigate(
                                        ScriptRunScreenDestination(
                                            ScriptRunArg(checkNotNull(singleUri), offerInstall = true, displayName = script.name)
                                        )
                                    )
                                    continue
                                }
                                val decision = CompletableDeferred<Unit>()
                                pendingZipDecision = decision
                                UltraActivityUtils.detectZipTypeAndShowConfirmation(this@MainActivity, zipUris) { infos ->
                                    if (infos.isNotEmpty()) {
                                        intentDispatcher.zipFiles = infos
                                        intentDispatcher.showConfirmation = true
                                    }
                                }
                                if (intentDispatcher.showConfirmation) {
                                    decision.await()
                                } else {
                                    snackBarHostState.showSnackbar(
                                        getString(R.string.unsupported_file_format),
                                        withDismissAction = true
                                    )
                                }
                                pendingZipDecision = null
                                intentDispatcher.queue.complete()
                            } catch (e: CancellationException) {
                                throw e
                            } catch (e: Exception) {
                                intentDispatcher.queue.complete()
                                android.util.Log.e("MainActivity", "Failed to handle incoming intent", e)
                            }
                        }
                    }

                    LaunchedEffect(Unit) {
                        initPlatform()
                        if (getSharedPreferences("settings", MODE_PRIVATE)
                                .getBoolean("auto_update_ksud", false)
                        ) {
                            KsuCli.autoSyncKsudIfNeeded()
                        }
                    }

                    CompositionLocalProvider(
                        LocalSnackbarHost provides snackBarHostState,
                        LocalNavigationLeaveGuard provides navigationLeaveGuard,
                    ) {
                        ManagerScaffold(
                            navController = navController,
                            showNavigation = currentDestination?.route in bottomBarRoutes,
                            active = !predictiveBackEnabled,
                            containerColor = if (predictiveBackEnabled) Color.Transparent else MaterialTheme.colorScheme.surface,
                            snackbarHost = {
                                if (!predictiveBackEnabled) SnackbarHost(hostState = snackBarHostState.hostState)
                            },
                        ) { outerPadding ->
                            DestinationsNavHost(
                                modifier = Modifier.fillMaxSize().padding(outerPadding),
                                navGraph = NavGraphs.root as NavHostGraphSpec,
                                navController = navController,
                                engine = rememberPredictiveBackNavHostEngine(bottomBarRoutes, predictiveBackEnabled),
                            ) {
                                predictiveBackSurfaces(NavGraphs.root) { route, contentModifier, content ->
                                    val predictive = LocalPredictiveBackEnabled.current
                                    ManagerScaffold(
                                        navController = navController,
                                        showNavigation = route in bottomBarRoutes,
                                        active = predictive,
                                        containerColor = if (predictive) MaterialTheme.colorScheme.surface else Color.Transparent,
                                        snackbarHost = {
                                            if (predictive) SnackbarHost(hostState = snackBarHostState.hostState)
                                        },
                                    ) { innerPadding ->
                                        Box(Modifier.fillMaxSize().padding(innerPadding).then(contentModifier)) { content() }
                                    }
                                }
                            }
                        }
                    }
                    KasumiConflictDialog()
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun initializeViewModels() {
        superUserViewModel = SuperUserViewModel()
        homeViewModel = HomeViewModel()

        themeChangeObserver = ThemeUtils.registerThemeChangeObserver(this)
    }

    private fun initializeData() {
        lifecycleScope.launch {
            try {
                superUserViewModel.fetchAppList()
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }

        DataRefreshUtils.startDataRefreshCoroutine(lifecycleScope)
        DataRefreshUtils.startSettingsMonitorCoroutine(lifecycleScope, this, settingsStateFlow)

        ThemeUtils.initializeThemeSettings(this, settingsStateFlow)
    }

    override fun onResume() {
        try {
            super.onResume()
            resetTaskDescriptionToAppName()
            ThemeUtils.onActivityResume()

            lifecycleScope.launch {
                val manager = withContext(Dispatchers.IO) {
                    val manager = runCatching { Natives.isManager }.getOrDefault(false)
                    if (manager && !KsuCli.SHELL.isRoot) {
                        KsuCli.refreshShells(checkKsud = false)
                    }
                    if (manager) {
                        migrateFeatureDefaults(KsuCli.SHELL)
                    }
                    KsudIntegrity.refresh(this@MainActivity, notifyOnMismatch = false)
                    manager
                }
                if (manager) onManagerAccessChanged()
                if (isInitialized) {
                    refreshData()
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun refreshData() {
        lifecycleScope.launch {
            try {
                superUserViewModel.fetchAppList()
                DataRefreshUtils.refreshData(lifecycleScope)
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }

    override fun onPause() {
        try {
            super.onPause()
            ThemeUtils.onActivityPause(this)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onDestroy() {
        try {
            if (isFinishing) {
                ZipFileDetector.cleanupStagedFiles(intentDispatcher.zipFiles)
            }
            ThemeUtils.unregisterThemeChangeObserver(this, themeChangeObserver)
            super.onDestroy()
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        intentDispatcher.submit(intent)
    }

    internal fun onManagerAccessChanged() {
        intentDispatcher.managerAccess.notifyChanged()
    }

    private fun resolveModuleIntent(intent: Intent): ModuleLink? {
        if (intent.data?.scheme == "ksu") {
            if (intent.action != Intent.ACTION_VIEW) return null
            return ModuleLink.parse(intent.data?.toString(), Shortcut.intentToken(this))
        }
        val type = when (intent.getStringExtra("shortcut_type")) {
            "module_action" -> ModuleLink.Type.Action
            "module_webui" -> ModuleLink.Type.WebUI
            else -> return null
        }
        val moduleId = intent.getStringExtra("module_id") ?: return null
        return ModuleLink(moduleId, type).takeIf { ModuleLink.isValidModuleId(moduleId) }
    }

    private fun extractZipUris(intent: Intent?): ArrayList<Uri>? {
        return when (intent?.action) {
            Intent.ACTION_SEND -> {
                val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(Intent.EXTRA_STREAM)
                }
                uri?.let(::arrayListOf)
            }

            Intent.ACTION_SEND_MULTIPLE -> {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM)
                }
            }

            else -> when {
                intent?.data != null -> arrayListOf(intent.data!!)
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU -> {
                    intent?.getParcelableArrayListExtra("uris", Uri::class.java)
                }

                else -> {
                    @Suppress("DEPRECATION")
                    intent?.getParcelableArrayListExtra("uris")
                }
            }
        }
    }
}
