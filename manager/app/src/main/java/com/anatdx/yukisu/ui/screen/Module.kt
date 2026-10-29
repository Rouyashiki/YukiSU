package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.ramcosta.composedestinations.generated.destinations.ScriptRunScreenDestination
import com.anatdx.yukisu.ui.screen.scriptrun.ScriptRunArg
import com.anatdx.yukisu.data.script.ScriptRepository
import android.annotation.SuppressLint
import android.app.Activity.*
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PersistableBundle
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Wysiwyg
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerEventTimeoutCancellationException
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick as semanticsOnClick
import androidx.compose.ui.semantics.onLongClick as semanticsOnLongClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.core.text.HtmlCompat
import com.anatdx.yukisu.data.registry.UpdateVerification
import com.anatdx.yukisu.ui.viewmodel.ModuleUpdateInfo
import com.anatdx.yukisu.ui.webui.ModuleConfig
import com.anatdx.yukisu.ui.webui.ModuleConfig.Companion.asModuleConfig
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.ExecuteModuleActionScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ModuleRepositoryScreenDestination
import com.anatdx.yukisu.ui.kasumi.KasumiMountConfigDialog
import com.anatdx.yukisu.ui.kasumi.KASUMI_MODE_COLORS
import com.anatdx.yukisu.ui.kasumi.ConfigChoice
import com.anatdx.yukisu.ui.kasumi.util.KasumiManager
import kotlinx.coroutines.CancellationException
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.navigation.EmptyDestinationsNavigator
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.component.*
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getElevatedCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.*
import com.anatdx.yukisu.ui.util.module.ModuleUtils
import com.anatdx.yukisu.ui.util.module.Shortcut
import com.anatdx.yukisu.ui.util.module.ModuleLink
import com.anatdx.yukisu.ui.util.module.ModuleRuntimeKind
import com.anatdx.yukisu.ui.viewmodel.ModuleViewModel
import com.anatdx.yukisu.ui.webui.WebUIActivity
import com.anatdx.yukisu.ui.webui.WebUIXActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import java.util.concurrent.TimeUnit

private data class ModuleDownloadUiState(
    val moduleName: String,
    val fileName: String,
    val progress: DownloadProgress = DownloadProgress(),
)

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
@SuppressLint("ResourceType", "AutoboxingStateCreation")
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun ModuleScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<ModuleViewModel>()
    val context = LocalContext.current
    val resources = LocalResources.current
    val prefs = context.getSharedPreferences("settings", MODE_PRIVATE)
    val snackBarHost = rememberSnackbarController()
    val scope = rememberCoroutineScope()
    val confirmDialog = rememberConfirmDialog()
    var lastClickTime by remember { mutableStateOf(0L) }

    LaunchedEffect(Unit) {
        viewModel.initializeCache(context)
    }

    val bottomSheetState = rememberBottomSheetState(
        initialValue = SheetValue.Hidden,
        enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
    )
    var showBottomSheet by remember { mutableStateOf(false) }
    var showModuleCompat by rememberSaveable { mutableStateOf(false) }
    var showPreinit by rememberSaveable { mutableStateOf(false) }

    if (showPreinit) {
        ModulePreinitDialog(
            onDismiss = { showPreinit = false },
            onReset = {
                viewModel.markNeedRefresh()
                scope.launch { viewModel.fetchModuleList() }
            },
        )
    }

    if (showModuleCompat) {
        ModuleCompatDialog(onDismiss = { showModuleCompat = false })
    }
    val listState = rememberLazyListState()
    val fabVisible by rememberFabVisibilityState(listState)

    var shortcutModuleId by rememberSaveable { mutableStateOf<String?>(null) }
    var shortcutName by rememberSaveable { mutableStateOf("") }
    var shortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var defaultShortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var selectedShortcutType by rememberSaveable { mutableStateOf<ModuleLink.Type?>(null) }
    var showShortcutDialog by remember { mutableStateOf(false) }
    var mountState by remember { mutableStateOf<KasumiManager.MountState?>(null) }
    var mountDialog by remember { mutableStateOf<Pair<String, String>?>(null) }
    var mountRefresh by remember { mutableIntStateOf(0) }
    var mountFilter by rememberSaveable { mutableStateOf("all") }
    var mountConflicts by remember { mutableStateOf<List<KasumiManager.MountConflict>?>(null) }
    var checkingConflicts by remember { mutableStateOf(false) }
    val mountRevision by KasumiManager.revision.collectAsState()
    LaunchedEffect(mountRefresh, mountRevision, viewModel.moduleList) {
        try { mountState = KasumiManager.getMountState() }
        catch (e: Exception) {
            if (e is CancellationException) throw e
            mountState = null
            Log.w("ModuleScreen", "Kagami mount controls unavailable", e)
        }
    }
    mountDialog?.let { (id, name) ->
        val state = mountState
        if (state != null) KasumiMountConfigDialog(
            moduleId = id,
            moduleName = name,
            initialInfo = state.modules[id],
            kasumiAvailable = state.available,
            globalMode = state.globalMode,
            bypassSupported = state.bypassSupported,
            onDismiss = { mountDialog = null },
            onSaved = { applied ->
                mountDialog = null
                mountRefresh++
                scope.launch {
                    snackBarHost.showSnackbar(resources.getString(
                        if (applied) R.string.kasumi_module_saved_applied else R.string.kasumi_saved_reboot,
                    ))
                }
            },
        )
    }
    mountConflicts?.let { conflicts ->
        YukiAlertDialog(
            onDismissRequest = { mountConflicts = null },
            title = { Text(stringResource(R.string.kasumi_check_conflicts)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    if (conflicts.isEmpty()) Text(stringResource(R.string.kasumi_no_conflicts))
                    conflicts.forEach { conflict ->
                        Text(conflict.path, style = MaterialTheme.typography.bodyMedium)
                        Text(conflict.modules.joinToString(", "), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(12.dp))
                    }
                }
            },
            confirmButton = { TextButton(onClick = { mountConflicts = null }) { Text(stringResource(android.R.string.ok)) } },
        )
    }
    suspend fun openScript(uri: Uri): Boolean {
        val manifest = ScriptRepository.peekOneshot(uri) ?: return false
        navigator.navigate(
            ScriptRunScreenDestination(
                ScriptRunArg(uri, offerInstall = true, displayName = manifest.name)
            )
        )
        return true
    }

    val selectZipLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode != RESULT_OK) {
            return@rememberLauncherForActivityResult
        }
        val data = it.data ?: return@rememberLauncherForActivityResult

        scope.launch {
            val clipData = data.clipData
            if (clipData != null) {
                val selectedModules = mutableListOf<Uri>()
                val selectedModuleNames = mutableMapOf<Uri, String>()

                fun processUri(uri: Uri) {
                    try {
                        if (!ModuleUtils.isUriAccessible(context, uri)) {
                            return
                        }
                        ModuleUtils.takePersistableUriPermission(context, uri)
                        val moduleName = ModuleUtils.extractModuleName(context, uri)
                        selectedModules.add(uri)
                        selectedModuleNames[uri] = moduleName
                    } catch (e: Exception) {
                        Log.e("ModuleScreen", "Error while processing URI: $uri, Error: ${e.message}")
                    }
                }

                for (i in 0 until clipData.itemCount) {
                    val uri = clipData.getItemAt(i).uri
                    processUri(uri)
                }

                if (selectedModules.isEmpty()) {
                    snackBarHost.showSnackbar("Unable to access selected module files")
                    return@launch
                }

                if (selectedModules.size == 1 && openScript(selectedModules.single())) return@launch

                val modulesList = selectedModuleNames.values.joinToString("\n• ", "• ")
                val confirmResult = confirmDialog.awaitConfirm(
                    title = resources.getString(R.string.module_install),
                    content = resources.getString(R.string.module_install_multiple_confirm_with_names, selectedModules.size, modulesList),
                    confirm = resources.getString(R.string.install),
                    dismiss = resources.getString(R.string.cancel)
                )

                if (confirmResult == ConfirmResult.Confirmed) {
                    // 直接安装所有模块
                    try {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModules(selectedModules)))
                        viewModel.markNeedRefresh()
                    } catch (e: Exception) {
                        Log.e("ModuleScreen", "Error navigating to FlashScreen: ${e.message}")
                        snackBarHost.showSnackbar("Error while installing module: ${e.message}")
                    }
                }
            } else {
                val uri = data.data ?: return@launch
                // 单个安装模块
                try {
                    if (!ModuleUtils.isUriAccessible(context, uri)) {
                        snackBarHost.showSnackbar("Unable to access selected module files")
                        return@launch
                    }

                    ModuleUtils.takePersistableUriPermission(context, uri)

                    if (openScript(uri)) return@launch
                    val moduleName = ModuleUtils.extractModuleName(context, uri)

                    val confirmResult = confirmDialog.awaitConfirm(
                        title = resources.getString(R.string.module_install),
                        content = resources.getString(R.string.module_install_confirm, moduleName),
                        confirm = resources.getString(R.string.install),
                        dismiss = resources.getString(R.string.cancel)
                    )

                    if (confirmResult == ConfirmResult.Confirmed) {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModule(uri)))
                        viewModel.markNeedRefresh()
                    }
                } catch (e: Exception) {
                    Log.e("ModuleScreen", "Error processing a single URI: $uri, Error: ${e.message}")
                    snackBarHost.showSnackbar("Error processing module file: ${e.message}")
                }
            }
        }
    }

    // 快捷方式图片选择器
    val pickShortcutIconLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri ->
        shortcutIconUri = uri?.toString()
    }

    val shortcutPreviewIcon = remember { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(shortcutIconUri) {
        val uriStr = shortcutIconUri
        if (uriStr.isNullOrBlank()) {
            shortcutPreviewIcon.value = null
            return@LaunchedEffect
        }
        val bitmap = withContext(Dispatchers.IO) {
            Shortcut.loadShortcutBitmap(context, uriStr)
        }
        shortcutPreviewIcon.value = bitmap?.asImageBitmap()
    }

    var hasExistingShortcut by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(shortcutModuleId, selectedShortcutType, showShortcutDialog) {
        val moduleId = shortcutModuleId
        val type = selectedShortcutType
        if (!showShortcutDialog || moduleId.isNullOrBlank() || type == null) {
            hasExistingShortcut = false
            return@LaunchedEffect
        }
        val exists = withContext(Dispatchers.IO) {
            when (type) {
                ModuleLink.Type.Action -> Shortcut.hasModuleActionShortcut(context, moduleId)
                ModuleLink.Type.WebUI -> Shortcut.hasModuleWebUiShortcut(context, moduleId)
            }
        }
        hasExistingShortcut = exists
    }

    fun onModuleAddShortcut(module: ModuleViewModel.ModuleInfo, type: ModuleLink.Type) {
        shortcutModuleId = module.dirId
        shortcutName = module.name
        selectedShortcutType = type
        val actionIcon = module.actionIconPath
            ?.takeIf { it.isNotBlank() }
            ?.let { Shortcut.moduleFileUri(module.dirId, it) }
        val webUiIcon = module.webUiIconPath
            ?.takeIf { it.isNotBlank() }
            ?.let { Shortcut.moduleFileUri(module.dirId, it) }
        val defaultIcon = when (type) {
            ModuleLink.Type.Action -> actionIcon ?: webUiIcon
            ModuleLink.Type.WebUI -> webUiIcon ?: actionIcon
        }
        defaultShortcutIconUri = defaultIcon
        shortcutIconUri = defaultIcon
        showShortcutDialog = true
    }

    LaunchedEffect(Unit) {
        if (viewModel.moduleList.isEmpty() || viewModel.isNeedRefresh) {
            viewModel.sortEnabledFirst = prefs.getBoolean("module_sort_enabled_first", false)
            viewModel.sortActionFirst = prefs.getBoolean("module_sort_action_first", false)
            viewModel.fetchModuleList()
        }
    }

    // Both checks were re-running on every recomposition; cache them.
    // hasMagisk() shells out, so push it to IO via produceState.
    val isSafeMode = remember { Natives.isSafeMode }
    val hasMagisk by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) { hasMagisk() }
    }
    val hideInstallButton = isSafeMode || hasMagisk

    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    val webUILauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) { viewModel.fetchModuleList() }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = {
                    Text(
                        text = stringResource(R.string.module),
                        fontWeight = if (isExpressiveUi) FontWeight.Normal else null
                    )
                },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                dropdownContent = {
                    IconButton(
                        onClick = { navigator.navigate(ModuleRepositoryScreenDestination) },
                    ) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Inventory2,
                            contentDescription = stringResource(R.string.module_repositories),
                        )
                    }
                    IconButton(
                        onClick = { showBottomSheet = true },
                    ) {
                        YukiIcon(
                            imageVector = Icons.Filled.MoreVert,
                            contentDescription = stringResource(id = R.string.settings),
                        )
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        floatingActionButton = {
            AnimatedFab(visible = !hideInstallButton && fabVisible) {
                FloatingActionButton(
                    shape = if (isExpressiveUi) {
                        CircleShape
                    } else {
                        FloatingActionButtonDefaults.shape
                    },
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                    containerColor = MaterialTheme.colorScheme.primary,
                    onClick = {
                        selectZipLauncher.launch(
                            Intent(Intent.ACTION_GET_CONTENT).apply {
                                type = "application/zip"
                                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                            }
                        )
                    },
                    content = {
                        Icon(
                            painter = painterResource(id = R.drawable.package_import),
                            contentDescription = null
                        )
                    }
                )
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
        snackbarHost = { SnackbarHost(hostState = snackBarHost.hostState) }
    ) { innerPadding ->
        when {
            hasMagisk -> {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(24.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center
                    ) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Warning,
                            contentDescription = null,
                            modifier = Modifier
                                .size(64.dp)
                                .padding(bottom = 16.dp)
                        )
                        Text(
                            stringResource(R.string.module_magisk_conflict),
                            textAlign = TextAlign.Center,
                            style = MaterialTheme.typography.bodyLarge,
                        )
                    }
                }
            }
            else -> {
                ModuleList(
                    navigator = navigator,
                    viewModel = viewModel,
                    listState = listState,
                    mountState = mountState,
                    mountFilter = mountFilter,
                    onMountConfig = { id, name -> mountDialog = id to name },
                    onRefreshMounts = { mountRefresh++ },
                    modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
                    boxModifier = Modifier.padding(innerPadding),
                    onInstallModule = {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModule(it)))
                    },
                    onUpdateModule = {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModuleUpdate(it)))
                    },
                    onClickModule = { id, name, hasWebUi ->
                        val currentTime = System.currentTimeMillis()
                        if (currentTime - lastClickTime < 600) {
                            Log.d("ModuleScreen", "Click too fast, ignoring")
                            return@ModuleList
                        }
                        lastClickTime = currentTime

                        if (hasWebUi) {
                            scope.launch {
                                try {
                                    val wxEngine = Intent(context, WebUIXActivity::class.java)
                                        .setData("kernelsu://webuix/$id".toUri())
                                        .putExtra("id", id)
                                        .putExtra("name", name)

                                    val ksuEngine = Intent(context, WebUIActivity::class.java)
                                        .setData("kernelsu://webui/$id".toUri())
                                        .putExtra("id", id)
                                        .putExtra("name", name)

                                    val config = try {
                                        withContext(Dispatchers.IO) { id.asModuleConfig }
                                    } catch (e: CancellationException) {
                                        throw e
                                    } catch (e: Exception) {
                                        Log.e("ModuleScreen", "Failed to get config from id: $id", e)
                                        null
                                    }

                                    val globalEngine = prefs.getString("webui_engine", "default") ?: "default"
                                    val moduleEngine = config?.getWebuiEngine(context)
                                    val selectedEngine = when (globalEngine) {
                                        "wx" -> wxEngine
                                        "ksu" -> ksuEngine
                                        // Automatic mode requires an explicit WebUI X preference.
                                        "default" -> if (moduleEngine == "wx") wxEngine else ksuEngine
                                        else -> ksuEngine
                                    }
                                    webUILauncher.launch(selectedEngine)
                                } catch (e: CancellationException) {
                                    throw e
                                } catch (e: Exception) {
                                    Log.e("ModuleScreen", "Error launching WebUI: ${e.message}", e)
                                    scope.launch {
                                        snackBarHost.showSnackbar("Error launching WebUI: ${e.message}")
                                    }
                                }
                            }
                            return@ModuleList
                        }
                    },
                    onAddShortcut = ::onModuleAddShortcut,
                    context = context,
                    snackBarHost = snackBarHost
                )
            }
        }

        if (showBottomSheet) {
            YukiModalBottomSheet(
                onDismissRequest = {
                    showBottomSheet = false
                },
                modifier = Modifier.clickHapticFeedback(),
                sheetState = bottomSheetState,
                dragHandle = {
                    Surface(
                        modifier = Modifier.padding(vertical = 11.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                        shape = RoundedCornerShape(16.dp)
                    ) {
                        Box(
                            Modifier.size(
                                width = 32.dp,
                                height = 4.dp
                            )
                        )
                    }
                }
            ) {
                ModuleBottomSheetContent(
                    viewModel = viewModel,
                    prefs = prefs,
                    scope = scope,
                    bottomSheetState = bottomSheetState,
                    mountState = mountState,
                    mountFilter = mountFilter,
                    onMountFilter = { mountFilter = it },
                    checkingConflicts = checkingConflicts,
                    onModuleCompat = {
                        scope.launch {
                            bottomSheetState.hide()
                            showBottomSheet = false
                            showModuleCompat = true
                        }
                    },
                    onPreinit = {
                        scope.launch {
                            bottomSheetState.hide()
                            showBottomSheet = false
                            showPreinit = true
                        }
                    },
                    onCheckConflicts = {
                        if (!checkingConflicts) {
                            checkingConflicts = true
                            scope.launch {
                                try {
                                    bottomSheetState.hide()
                                    showBottomSheet = false
                                    mountConflicts = KasumiManager.checkConflicts()
                                } catch (error: Exception) {
                                    if (error is CancellationException) throw error
                                    snackBarHost.showSnackbar(error.message ?: resources.getString(R.string.operation_failed))
                                } finally { checkingConflicts = false }
                            }
                        }
                    },
                    onDismiss = { showBottomSheet = false }
                )
            }
        }

    if (showShortcutDialog) {
        YukiAlertDialog(
            onDismissRequest = { showShortcutDialog = false },
            title = { Text(stringResource(R.string.module_shortcut_title)) },
            text = {
                Column(
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    // 图标预览
                    Box(
                        contentAlignment = Alignment.Center,
                        modifier = Modifier
                            .padding(vertical = 16.dp)
                            .size(100.dp)
                            .clip(RoundedCornerShape(25.dp))
                    ) {
                        val preview = shortcutPreviewIcon.value
                        if (preview != null) {
                            Image(
                                bitmap = preview,
                                modifier = Modifier.size(100.dp),
                                contentDescription = null,
                            )
                        } else {
                            Box(
                                modifier = Modifier
                                    .size(100.dp)
                                    .background(Color.White)
                            )
                            Icon(
                                painter = painterResource(id = R.drawable.ic_launcher_foreground),
                                contentDescription = null,
                                modifier = Modifier.size(150.dp)
                            )
                        }
                    }

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        FilledTonalButton(
                            modifier = Modifier.weight(1f),
                            onClick = { pickShortcutIconLauncher.launch("image/*") },
                        ) {
                            Text(stringResource(id = R.string.module_shortcut_icon_pick))
                        }
                        val motion = LocalYukiMotion.current
                        AnimatedVisibility(
                            visible = shortcutIconUri != defaultShortcutIconUri,
                            enter = expandHorizontally(motion.spatial()) + fadeIn(motion.effects()),
                            exit = shrinkHorizontally(motion.effects()) + fadeOut(motion.effects()),
                        ) {
                            IconButton(
                                onClick = { shortcutIconUri = defaultShortcutIconUri }
                            ) {
                                YukiIcon(
                                    imageVector = Icons.AutoMirrored.Filled.Undo,
                                    contentDescription = null,
                                    modifier = Modifier.size(28.dp),
                                )
                            }
                        }
                    }

                    // 名称输入框
                    OutlinedTextField(
                        value = shortcutName,
                        onValueChange = { shortcutName = it },
                        label = { Text(stringResource(id = R.string.module_shortcut_name_label)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )

                    FilledTonalButton(
                        onClick = {
                            val moduleId = shortcutModuleId ?: return@FilledTonalButton
                            val type = selectedShortcutType ?: return@FilledTonalButton
                            val uri = Shortcut.buildShortcutUri(context, moduleId, type)
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                            val clip = ClipData.newPlainText("", uri.toString()).apply {
                                description.extras = PersistableBundle().apply {
                                    putBoolean("android.content.extra.IS_SENSITIVE", true)
                                }
                            }
                            clipboard.setPrimaryClip(clip)
                            Toast.makeText(context, R.string.module_shortcut_scheme_copied, Toast.LENGTH_SHORT).show()
                        },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(stringResource(R.string.module_shortcut_copy_scheme))
                    }

                    if (hasExistingShortcut) {
                        FilledTonalButton(
                            onClick = {
                                val moduleId = shortcutModuleId
                                val type = selectedShortcutType
                                if (!moduleId.isNullOrBlank() && type != null) {
                                    when (type) {
                                        ModuleLink.Type.Action -> Shortcut.deleteModuleActionShortcut(context, moduleId)
                                        ModuleLink.Type.WebUI -> Shortcut.deleteModuleWebUiShortcut(context, moduleId)
                                    }
                                }
                                showShortcutDialog = false
                            },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(stringResource(id = R.string.module_shortcut_delete))
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val moduleId = shortcutModuleId
                        val type = selectedShortcutType
                        if (!moduleId.isNullOrBlank() && shortcutName.isNotBlank() && type != null) {
                            when (type) {
                                ModuleLink.Type.Action -> {
                                    Shortcut.createModuleActionShortcut(
                                        context = context,
                                        moduleId = moduleId,
                                        name = shortcutName,
                                        iconUri = shortcutIconUri
                                    )
                                }
                                ModuleLink.Type.WebUI -> {
                                    Shortcut.createModuleWebUiShortcut(
                                        context = context,
                                        moduleId = moduleId,
                                        name = shortcutName,
                                        iconUri = shortcutIconUri
                                    )
                                }
                            }
                        }
                        showShortcutDialog = false
                    }
                ) {
                    Text(
                        if (hasExistingShortcut) {
                            stringResource(id = R.string.module_update)
                        } else {
                            stringResource(id = android.R.string.ok)
                        }
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { showShortcutDialog = false }) {
                    Text(stringResource(id = android.R.string.cancel))
                }
            }
        )
    }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun ModuleBottomSheetContent(
    viewModel: ModuleViewModel,
    prefs: android.content.SharedPreferences,
    scope: kotlinx.coroutines.CoroutineScope,
    bottomSheetState: SheetState,
    mountState: KasumiManager.MountState?,
    mountFilter: String,
    onMountFilter: (String) -> Unit,
    checkingConflicts: Boolean,
    onModuleCompat: () -> Unit,
    onPreinit: () -> Unit,
    onCheckConflicts: () -> Unit,
    onDismiss: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(bottom = 24.dp)
    ) {
        Text(
            text = stringResource(R.string.sort_options),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp)
        )

        Column(
            modifier = Modifier.padding(horizontal = 24.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.module_sort_action_first),
                    style = MaterialTheme.typography.bodyMedium
                )
                YukiSwitch(
                    checked = viewModel.sortActionFirst,
                    onCheckedChange = { checked ->
                        viewModel.sortActionFirst = checked
                        prefs.edit {
                            putBoolean("module_sort_action_first", checked)
                        }
                        scope.launch {
                            viewModel.fetchModuleList()
                            bottomSheetState.hide()
                            onDismiss()
                        }
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.module_sort_enabled_first),
                    style = MaterialTheme.typography.bodyMedium
                )
                YukiSwitch(
                    checked = viewModel.sortEnabledFirst,
                    onCheckedChange = { checked ->
                        viewModel.sortEnabledFirst = checked
                        prefs.edit {
                            putBoolean("module_sort_enabled_first", checked)
                        }
                        scope.launch {
                            viewModel.fetchModuleList()
                            bottomSheetState.hide()
                            onDismiss()
                        }
                    }
                )
            }
            HorizontalDivider(Modifier.padding(vertical = 8.dp))
            FilledTonalButton(
                onClick = onModuleCompat,
                modifier = Modifier.fillMaxWidth(),
                shapes = if (isExpressiveUi) ButtonDefaults.shapes() else
                    ButtonDefaults.shapes(shape = ButtonDefaults.shape, pressedShape = ButtonDefaults.shape),
            ) { Text(stringResource(R.string.module_compat_title)) }
            OutlinedButton(onClick = onPreinit, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.module_preinit_title))
            }
            ConfigChoice(stringResource(R.string.kasumi_mount_filter), mountFilter,
                listOf("all", "auto", "kasumi", "overlay", "magic", "none", "kasumi-active"),
                mountState != null) { value ->
                onMountFilter(value)
                scope.launch { bottomSheetState.hide(); onDismiss() }
            }
            OutlinedButton(onClick = onCheckConflicts, enabled = mountState != null && !checkingConflicts,
                modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.kasumi_check_conflicts)) }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModuleList(
    navigator: DestinationsNavigator,
    viewModel: ModuleViewModel,
    listState: LazyListState,
    mountState: KasumiManager.MountState?,
    mountFilter: String,
    onMountConfig: (String, String) -> Unit,
    onRefreshMounts: () -> Unit,
    modifier: Modifier = Modifier,
    boxModifier: Modifier = Modifier,
    onInstallModule: (Uri) -> Unit,
    onUpdateModule: (Uri) -> Unit,
    onClickModule: (id: String, name: String, hasWebUi: Boolean) -> Unit,
    onAddShortcut: (ModuleViewModel.ModuleInfo, ModuleLink.Type) -> Unit,
    context: Context,
    snackBarHost: SnackbarController
) {
    val failedEnable = stringResource(R.string.module_failed_to_enable)
    val failedDisable = stringResource(R.string.module_failed_to_disable)
    val failedUninstall = stringResource(R.string.module_uninstall_failed)
    val successUninstall = stringResource(R.string.module_uninstall_success)
    val softReboot = isSoftRebootPreferred(context)
    val reboot = stringResource(if (softReboot) R.string.reboot_soft else R.string.reboot)
    val rebootToApply = stringResource(R.string.reboot_to_apply)
    val moduleStr = stringResource(R.string.module)
    val uninstall = stringResource(R.string.uninstall)
    val cancel = stringResource(android.R.string.cancel)
    val moduleUninstallConfirm = stringResource(R.string.module_uninstall_confirm)
    val metaModuleUninstallConfirm = stringResource(R.string.metamodule_uninstall_confirm)
    val updateText = stringResource(R.string.module_update)
    val changelogText = stringResource(R.string.module_changelog)
    val downloadingText = stringResource(R.string.module_downloading)
    val startDownloadingText = stringResource(R.string.module_start_downloading)
    val fetchChangeLogFailed = stringResource(R.string.module_changelog_failed)
    val downloadErrorText = stringResource(R.string.module_download_error)

    val loadingDialog = rememberLoadingDialog()
    val confirmDialog = rememberConfirmDialog()
    var activeDownload by remember { mutableStateOf<ModuleDownloadUiState?>(null) }
    var downloadHandle by remember { mutableStateOf<DownloadHandle?>(null) }

    suspend fun onModuleUpdate(
        module: ModuleViewModel.ModuleInfo,
        update: ModuleUpdateInfo,
        fileName: String
    ) {
        val verification = update.verification
        if (verification is UpdateVerification.Rejected) {
            Toast.makeText(context, "$downloadErrorText: ${verification.reason}", Toast.LENGTH_LONG).show()
            return
        }
        val client = OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            .build()

        val changelogResult = if (verification is UpdateVerification.Registry) {
            Result.success(
                HtmlCompat.fromHtml(verification.release.changelogHtml.orEmpty(), HtmlCompat.FROM_HTML_MODE_LEGACY)
                    .toString().ifBlank { "${module.name} ${update.version}" }
            )
        } else loadingDialog.withLoading {
            withContext(Dispatchers.IO) {
                runCatching {
                    val request = okhttp3.Request.Builder()
                        .url(update.changelog)
                        .header("User-Agent", "YukiSU/${BuildConfig.VERSION_NAME}")
                        .build()
                    client.newCall(request).execute().use { it.body.string() }
                }
            }
        }

        val showToast: suspend (String) -> Unit = { msg ->
            withContext(Dispatchers.Main) {
                Toast.makeText(
                    context,
                    msg,
                    Toast.LENGTH_SHORT
                ).show()
            }
        }

        val changelog = changelogResult.getOrElse {
            showToast(fetchChangeLogFailed.format(it.message))
            return
        }.ifBlank {
            showToast(fetchChangeLogFailed.format(module.name))
            return
        }

        val confirmResult = confirmDialog.awaitConfirm(
            changelogText,
            content = changelog,
            markdown = verification !is UpdateVerification.Registry,
            confirm = updateText,
        )

        if (confirmResult != ConfirmResult.Confirmed) {
            return
        }

        val downloading = downloadingText.format(module.name)
        activeDownload = ModuleDownloadUiState(module.name, fileName)
        downloadHandle = download(
            context,
            update.zipUrl,
            fileName,
            downloading,
            expected = (verification as? UpdateVerification.Registry)?.expected,
            onDownloaded = { uri ->
                activeDownload = null
                downloadHandle = null
                onUpdateModule(uri)
            },
            onProgress = { progress ->
                activeDownload = activeDownload?.copy(progress = progress)
            },
            onError = { errorMsg ->
                activeDownload = null
                downloadHandle = null
                Toast.makeText(context, "$downloadErrorText: $errorMsg", Toast.LENGTH_LONG).show()
            },
        )
    }

    suspend fun onModuleUninstallClicked(module: ModuleViewModel.ModuleInfo) {
        val isUninstall = !module.remove
        if (isUninstall) {
            val formatter = if (module.metamodule) metaModuleUninstallConfirm else moduleUninstallConfirm
            val confirmResult = confirmDialog.awaitConfirm(
                moduleStr,
                content = formatter.format(module.name),
                confirm = uninstall,
                dismiss = cancel
            )
            if (confirmResult != ConfirmResult.Confirmed) {
                return
            }
        }

        val success = loadingDialog.withLoading {
            withContext(Dispatchers.IO) {
                if (isUninstall) {
                    uninstallModule(module.dirId)
                } else {
                    undoUninstallModule(module.dirId)
                }
            }
        }

        if (success) {
            viewModel.fetchModuleList()
            viewModel.markNeedRefresh()
        }
        if (!isUninstall) return
        val message = if (success) {
            successUninstall.format(module.name)
        } else {
            failedUninstall.format(module.name)
        }
        val actionLabel = if (success) {
            reboot
        } else {
            null
        }
        snackBarHost.showSnackbar(
            message = message,
            actionLabel = actionLabel,
            duration = SnackbarDuration.Long,
            onAction = { reboot(if (softReboot) "soft_reboot" else "") },
        )
    }

    YukiPullToRefreshBox(
        modifier = boxModifier,
        onRefresh = {
            viewModel.fetchModuleList()
            onRefreshMounts()
        },
        isRefreshing = viewModel.isRefreshing
    ) {
        val visibleModules = viewModel.moduleList.filter { module ->
            when (mountFilter) {
                "all" -> true
                "kasumi-active" -> mountState?.activeKasumiIds?.contains(module.dirId) == true
                else -> mountState?.modules?.get(module.dirId)?.mode == mountFilter
            }
        }
        LazyColumn(
            state = listState,
            modifier = modifier,
            verticalArrangement = Arrangement.spacedBy(16.dp),
            contentPadding = remember {
                PaddingValues(
                    start = 16.dp,
                    top = 16.dp,
                    end = 16.dp,
                    bottom = 16.dp + 56.dp + 16.dp + 48.dp + 6.dp /* Scaffold Fab Spacing + Fab container height + SnackBar height */
                )
            },
        ) {
            when {
                visibleModules.isEmpty() -> {
                    item {
                        Box(
                            modifier = Modifier.fillParentMaxSize(),
                            contentAlignment = Alignment.Center
                        ) {
                            Column(
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.Center
                            ) {
                                YukiIcon(
                                    imageVector = Icons.Outlined.Extension,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f),
                                    modifier = Modifier
                                        .size(96.dp)
                                        .padding(bottom = 16.dp)
                                )
                                Text(
                                    text = stringResource(R.string.module_empty),
                                    textAlign = TextAlign.Center,
                                    style = MaterialTheme.typography.bodyLarge,
                                )
                            }
                        }
                    }
                }

                else -> {
                    items(
                        items = visibleModules,
                        key = { it.dirId }
                    ) { module ->
                        val scope = rememberCoroutineScope()
                        val updatedModule by produceState(initialValue = ModuleUpdateInfo(), key1 = module) {
                            value = withContext(Dispatchers.IO) { viewModel.checkUpdate(module) }
                        }

                        ModuleItem(
                            navigator = navigator,
                            module = module,
                            updateUrl = updatedModule.zipUrl,
                            mountInfo = mountState?.modules?.get(module.dirId),
                            mountConfigEnabled = mountState?.externalOwner?.isEmpty() == true && mountState.builtinEnabled,
                            onMountConfig = { onMountConfig(module.dirId, module.name) },
                            onUninstallClicked = {
                                scope.launch { onModuleUninstallClicked(module) }
                            },
                            onCheckChanged = { newChecked ->
                                val success = withContext(Dispatchers.IO) {
                                    toggleModule(module.dirId, newChecked)
                                }
                                if (success) {
                                    viewModel.fetchModuleList()
                                    snackBarHost.showSnackbar(
                                        message = rebootToApply,
                                        actionLabel = reboot,
                                        duration = SnackbarDuration.Long,
                                        onAction = { reboot(if (softReboot) "soft_reboot" else "") },
                                    )
                                } else {
                                    val message = if (newChecked) failedEnable else failedDisable
                                    snackBarHost.showSnackbar(message.format(module.name))
                                }
                                success
                            },
                            onUpdate = {
                                scope.launch {
                                    onModuleUpdate(
                                        module,
                                        updatedModule,
                                        "${module.name}-${updatedModule.version}.zip"
                                    )
                                }
                            },
                            onClick = { clickedModule: ModuleViewModel.ModuleInfo ->
                                onClickModule(clickedModule.dirId, clickedModule.name, clickedModule.hasWebUi)
                            },
                            onAddShortcut = { type ->
                                onAddShortcut(module, type)
                            }
                        )

                        Spacer(Modifier.height(1.dp))
                    }
                }
            }
        }
        }

        activeDownload?.let { state ->
            DownloadProgressDialog(
                title = downloadingText.format(state.moduleName),
                message = startDownloadingText.format(state.fileName),
                progress = state.progress,
                onCancel = {
                    downloadHandle?.cancel()
                    downloadHandle = null
                    activeDownload = null
                },
            )
        }
    }

@Composable
fun ModuleItem(
    navigator: DestinationsNavigator,
    module: ModuleViewModel.ModuleInfo,
    updateUrl: String,
    mountInfo: KasumiManager.ModuleInfo? = null,
    mountConfigEnabled: Boolean = false,
    onMountConfig: () -> Unit = {},
    onUninstallClicked: (ModuleViewModel.ModuleInfo) -> Unit,
    onCheckChanged: suspend (Boolean) -> Boolean,
    onUpdate: (ModuleViewModel.ModuleInfo) -> Unit,
    onClick: (ModuleViewModel.ModuleInfo) -> Unit,
    onAddShortcut: (ModuleLink.Type) -> Unit = {}
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val moduleDescriptionMaxLines = context.getSharedPreferences("settings", MODE_PRIVATE)
        .getInt("module_description_max_lines", 4)
        .coerceIn(1, 5)
    val (isHideTagRow, showMoreModuleInfo) = remember {
        val p = context.getSharedPreferences("settings", MODE_PRIVATE)
        Pair(p.getBoolean("is_hide_tag_row", false), p.getBoolean("show_more_module_info", false))
    }

    // 剪贴板管理器和触觉反馈
    val clipboardManager = context.getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
    val hapticFeedback = LocalHapticFeedback.current

    ElevatedCard(
        colors = getCardColors(
            if (isExpressiveUi) {
                MaterialTheme.colorScheme.surfaceContainer
            } else {
                MaterialTheme.colorScheme.surfaceContainerHigh
            }
        ),
        elevation = getElevatedCardElevation(),
        shape = if (isExpressiveUi) MaterialTheme.shapes.large else CardDefaults.elevatedShape,
    ) {
        val textDecoration = if (!module.remove) null else TextDecoration.LineThrough
        val viewModel = viewModel<ModuleViewModel>()
        
        var localEnabled by remember(module.enabled) { mutableStateOf(module.enabled) }
        val scope = rememberCoroutineScope()

        val conflictDisabled = module.zygiskConflict

        val sizeStr = remember(module.dirId) {
            viewModel.getModuleSize(module.dirId)
        }

        Column(
            modifier = Modifier.padding(22.dp, 18.dp, 22.dp, 12.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                val moduleVersion = stringResource(id = R.string.module_version)
                val moduleAuthor = stringResource(id = R.string.module_author)

                Column(
                    modifier = Modifier.fillMaxWidth(0.8f)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Text(
                            text = module.name,
                            fontSize = MaterialTheme.typography.titleMedium.fontSize,
                            fontWeight = FontWeight.SemiBold,
                            lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                            fontFamily = MaterialTheme.typography.titleMedium.fontFamily,
                            textDecoration = textDecoration,
                            modifier = Modifier.weight(1f, false)
                        )
                    }

                    Text(
                        text = "$moduleVersion: ${module.version}",
                        fontSize = MaterialTheme.typography.bodySmall.fontSize,
                        lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                        fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                        textDecoration = textDecoration,
                    )

                    Text(
                        text = "$moduleAuthor: ${module.author}",
                        fontSize = MaterialTheme.typography.bodySmall.fontSize,
                        lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                        fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                        textDecoration = textDecoration,
                    )

                    // 显示更多模块信息时添加updateJson
                    if (showMoreModuleInfo && module.updateJson.isNotEmpty()) {
                        val updateJsonLabel = stringResource(R.string.module_update_json)
                        Text(
                            text = "$updateJsonLabel: ${module.updateJson}",
                            fontSize = MaterialTheme.typography.bodySmall.fontSize,
                            lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                            textDecoration = textDecoration,
                            color = MaterialTheme.colorScheme.primary,
                            maxLines = 5,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier
                                .fillMaxWidth()
                                .combinedClickable(
                                    onClick = { },
                                    onLongClick = {
                                        val clipData = ClipData.newPlainText(
                                            "Update JSON URL",
                                            module.updateJson
                                        )
                                        clipboardManager.setPrimaryClip(clipData)
                                        hapticFeedback.performHapticFeedback(HapticFeedbackType.LongPress)

                                        Toast.makeText(
                                            context,
                                            resources.getString(R.string.module_update_json_copied),
                                            Toast.LENGTH_SHORT
                                        ).show()
                                    }
                                ),
                        )
                    }
                }

                Spacer(modifier = Modifier.weight(1f))

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                ) {
                    YukiSwitch(
                        enabled = !module.update && !conflictDisabled,
                        checked = localEnabled,
                        onCheckedChange = { newChecked ->
                            localEnabled = newChecked
                            scope.launch {
                                val success = onCheckChanged(newChecked)
                                if (!success) {
                                    localEnabled = !newChecked
                                }
                            }
                        },
                    )
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            Text(
                text = module.description,
                fontSize = MaterialTheme.typography.bodySmall.fontSize,
                fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                fontWeight = MaterialTheme.typography.bodySmall.fontWeight,
                overflow = TextOverflow.Ellipsis,
                maxLines = moduleDescriptionMaxLines,
                textDecoration = textDecoration,
            )

            if (conflictDisabled) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(
                        if (module.dirId == "yukizygisk") {
                            R.string.module_disabled_standalone_yukizygisk
                        } else {
                            R.string.module_disabled_by_yukizygisk
                        }
                    ),
                    color = MaterialTheme.colorScheme.error,
                    fontSize = MaterialTheme.typography.bodySmall.fontSize,
                    lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
                    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                )
            }

            val preinitWarning = when (module.preinit) {
                "no_space" -> R.string.module_preinit_no_space
                "error" -> R.string.module_preinit_error
                "bootloop" -> R.string.module_preinit_bootloop
                "unsupported" -> R.string.module_preinit_unsupported
                else -> null
            }
            preinitWarning?.let { message ->
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(message),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            if (!isHideTagRow) {
                Spacer(modifier = Modifier.height(12.dp))
                FlowRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                    itemVerticalAlignment = Alignment.CenterVertically,
                ) {
                    if (module.metamodule) {
                        Surface(
                            shape = RoundedCornerShape(4.dp),
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier
                        ) {
                            Text(
                                text = "META",
                                style = MaterialTheme.typography.labelSmall,
                                modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                                color = MaterialTheme.colorScheme.onPrimary,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = MaterialTheme.colorScheme.primary,
                    ) {
                        Text(
                            text = module.dirId,
                            style = MaterialTheme.typography.labelSmall,
                            modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                            color = MaterialTheme.colorScheme.onPrimary,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = MaterialTheme.colorScheme.secondaryContainer,
                        modifier = Modifier
                    ) {
                        Text(
                            text = sizeStr,
                            style = MaterialTheme.typography.labelSmall,
                            modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                            color = MaterialTheme.colorScheme.onSecondaryContainer,
                            maxLines = 1
                        )
                    }
                    if (module.runtimeKinds.isNotEmpty()) {
                        val runtimeKinds = ModuleRuntimeKind.entries.filter { it in module.runtimeKinds }
                        val runtimeLabel = runtimeKinds.map { kind ->
                            stringResource(
                                when (kind) {
                                    ModuleRuntimeKind.Native -> R.string.module_zn_module
                                    ModuleRuntimeKind.Zygisk -> R.string.module_zygisk_module
                                }
                            )
                        }.joinToString(" + ")
                        Surface(
                            modifier = Modifier.clearAndSetSemantics {
                                contentDescription = runtimeLabel
                            },
                            shape = RoundedCornerShape(4.dp),
                            color = MaterialTheme.colorScheme.tertiaryContainer,
                        ) {
                            Text(
                                text = runtimeKinds.joinToString(" + ") { it.badgeLabel },
                                style = MaterialTheme.typography.labelSmall,
                                modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                                color = MaterialTheme.colorScheme.onTertiaryContainer,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                    if (mountInfo != null && mountConfigEnabled && !module.metamodule) {
                        val strategy = if (mountInfo.mode == "none") "none" else mountInfo.strategy
                        val strategyLabel = stringResource(when (strategy) {
                            "kasumi" -> R.string.kasumi_mount_mode_kasumi
                            "overlay" -> R.string.kasumi_mount_mode_overlay
                            "magic" -> R.string.kasumi_strategy_magic_mount
                            else -> R.string.kasumi_strategy_not_mounted
                        })
                        Surface(
                            shape = RoundedCornerShape(4.dp),
                            color = KASUMI_MODE_COLORS[strategy] ?: MaterialTheme.colorScheme.primary,
                        ) {
                            Text(
                                text = strategyLabel,
                                style = MaterialTheme.typography.labelSmall,
                                modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                                color = Color.White,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                }
            }

            Spacer(modifier = Modifier.height(16.dp))

            HorizontalDivider(thickness = Dp.Hairline)

            Spacer(modifier = Modifier.height(8.dp))

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (mountInfo != null && mountConfigEnabled && !module.metamodule) {
                    ModuleActionButton(
                        enabled = !module.remove && localEnabled,
                        onClick = onMountConfig,
                        imageVector = Icons.Outlined.Folder,
                        contentDescription = stringResource(R.string.kasumi_mount_config),
                    )
                }
                if (module.hasActionScript) {
                    ModuleActionButton(
                        enabled = !module.remove && !module.update && localEnabled,
                        onClick = {
                            navigator.navigate(ExecuteModuleActionScreenDestination(module.dirId))
                            viewModel.markNeedRefresh()
                        },
                        onLongClick = if (module.remove) null else ({ onAddShortcut(ModuleLink.Type.Action) }),
                        imageVector = Icons.Outlined.PlayArrow,
                        contentDescription = "Action"
                    )
                }

                if (module.hasWebUi) {
                    ModuleActionButton(
                        enabled = !module.remove && !module.update && localEnabled,
                        onClick = { onClick(module) },
                        onLongClick = if (module.remove) null else ({ onAddShortcut(ModuleLink.Type.WebUI) }),
                        imageVector = Icons.AutoMirrored.Outlined.Wysiwyg,
                        contentDescription = "WebUI"
                    )
                }

                Spacer(modifier = Modifier.weight(1f, true))

                if (updateUrl.isNotEmpty()) {
                    ModuleActionButton(
                        enabled = !module.remove,
                        onClick = { onUpdate(module) },
                        imageVector = Icons.Outlined.Download,
                        contentDescription = null,
                        prominent = true
                    )
                }

                ModuleActionButton(
                    onClick = { onUninstallClicked(module) },
                    imageVector = if (!module.remove) Icons.Outlined.Delete else Icons.Outlined.Refresh,
                    modifier = if (!module.remove) Modifier else Modifier.rotate(180f),
                    contentDescription = null
                )
            }
        }
    }
}

@Composable
private fun ModuleActionButton(
    imageVector: ImageVector,
    contentDescription: String?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    onLongClick: (() -> Unit)? = null,
    enabled: Boolean = true,
    prominent: Boolean = false,
    interactionSource: MutableInteractionSource? = null
) {
    val currentLongClick by rememberUpdatedState(onLongClick)
    val hapticFeedback = LocalHapticFeedback.current
    val longClickLabel = stringResource(R.string.module_shortcut_add)
    val longClickModifier = if (onLongClick != null) {
        Modifier
            .clearAndSetSemantics {
                role = Role.Button
                contentDescription?.let { this.contentDescription = it }
                if (enabled) {
                    semanticsOnClick {
                        onClick()
                        true
                    }
                }
                semanticsOnLongClick(label = longClickLabel) {
                    currentLongClick?.invoke()
                    true
                }
            }
            .pointerInput(Unit) {
                awaitEachGesture {
                    awaitFirstDown(pass = PointerEventPass.Initial)
                    try {
                        withTimeout(viewConfiguration.longPressTimeoutMillis) {
                            waitForUpOrCancellation(pass = PointerEventPass.Initial)
                        }
                    } catch (_: PointerEventTimeoutCancellationException) {
                        hapticFeedback.performHapticFeedback(HapticFeedbackType.LongPress)
                        currentLongClick?.invoke()
                        // Consume the release before the button can treat it as a click.
                        do {
                            val event = awaitPointerEvent(pass = PointerEventPass.Initial)
                            event.changes.forEach { it.consume() }
                        } while (event.changes.any { it.pressed })
                    }
                }
            }
    } else {
        Modifier
    }
    val icon: @Composable () -> Unit = {
        YukiIcon(
            modifier = modifier.size(20.dp),
            imageVector = imageVector,
            contentDescription = contentDescription
        )
    }

    if (isExpressiveUi) {
        if (prominent) {
            FilledIconButton(
                onClick = onClick,
                shapes = IconButtonDefaults.shapes(),
                modifier = Modifier.size(40.dp).then(longClickModifier),
                enabled = enabled,
                interactionSource = interactionSource,
                content = icon
            )
        } else {
            FilledTonalIconButton(
                onClick = onClick,
                shapes = IconButtonDefaults.shapes(),
                modifier = Modifier.size(40.dp).then(longClickModifier),
                enabled = enabled,
                interactionSource = interactionSource,
                content = icon
            )
        }
    } else if (prominent) {
        Button(
            modifier = Modifier.defaultMinSize(minWidth = 52.dp, minHeight = 32.dp).then(longClickModifier),
            enabled = enabled,
            onClick = onClick,
            shape = ButtonDefaults.textShape,
            interactionSource = interactionSource,
            contentPadding = ButtonDefaults.TextButtonContentPadding,
            content = { icon() }
        )
    } else {
        FilledTonalButton(
            modifier = Modifier.defaultMinSize(minWidth = 52.dp, minHeight = 32.dp).then(longClickModifier),
            enabled = enabled,
            onClick = onClick,
            interactionSource = interactionSource,
            contentPadding = ButtonDefaults.TextButtonContentPadding,
            content = { icon() }
        )
    }
}

@Preview
@Composable
fun ModuleItemPreview() {
    val module = ModuleViewModel.ModuleInfo(
        id = "id",
        name = "name",
        version = "version",
        versionCode = 1,
        author = "author",
        description = "I am a test module and i do nothing but show a very long description",
        enabled = true,
        update = true,
        remove = false,
        updateJson = "",
        hasWebUi = true,
        hasActionScript = true,
        metamodule = true,
        dirId = "dirId",
        config = ModuleConfig()
    )
    ModuleItem(
        navigator = EmptyDestinationsNavigator,
        module = module,
        updateUrl = "",
        onUninstallClicked = {},
        onCheckChanged = { true },
        onUpdate = {},
        onClick = {}
    )
}
