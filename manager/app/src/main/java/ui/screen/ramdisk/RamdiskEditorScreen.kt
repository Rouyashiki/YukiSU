package ui.screen.ramdisk

import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.content.Context
import android.content.res.Resources
import android.net.Uri
import android.os.SystemClock
import android.provider.OpenableColumns
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.anatdx.yukifb.hex.state.HexEditorController
import com.anatdx.yukifb.hex.ui.HexEditor
import com.anatdx.yukifb.backend.FileContentSource
import com.anatdx.yukifb.model.EntryId
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileContentChangedException
import com.anatdx.yukifb.state.FileChecksumController
import com.anatdx.yukifb.ui.FileChecksumsDialog
import com.anatdx.yukifb.ui.FileActionProgressDialog
import com.anatdx.yukifb.model.TextFileEncoding
import com.anatdx.yukifb.state.FileBrowserAction
import com.anatdx.yukifb.state.FileBrowserController
import com.anatdx.yukifb.state.TextFileEditorState
import com.anatdx.yukifb.ui.FileBrowser
import com.anatdx.yukifb.ui.FileBrowserPreview
import com.anatdx.yukifb.ui.FileBrowserBackHandler
import com.anatdx.yukifb.ui.FileBrowserPresentationState
import com.anatdx.yukifb.ui.TextFileEditor
import com.anatdx.yukifb.ui.YukiFBBackHandler
import com.anatdx.yukifb.ui.YukiFBMotion
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukifb.ui.YukiFBProvider
import com.anatdx.yukifb.ui.YukiFBScene
import com.anatdx.yukifb.ui.YukiFBStyle
import com.anatdx.yukifb.ui.YukiFBThemeDefaults
import com.anatdx.yukifb.ui.rememberYukiFBBackState
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.activity.util.LocalPredictiveBackEnabled
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.BackgroundLayer
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.LocalNavigationLeaveGuard
import com.anatdx.yukisu.ui.util.SnackbarController
import com.anatdx.yukisu.ui.util.getKsud
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.RamdiskEditorScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.delay
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.partition.PartitionManagerHelper

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun RamdiskEditorScreen(
    navigator: DestinationsNavigator,
    partitionName: String,
    targetSlot: String?,
) {
    val scheme = MaterialTheme.colorScheme
    val alpha = CardConfig.cardAlpha
    val colors = YukiFBThemeDefaults.colors().copy(
        background = Color.Transparent,
        listContainer = scheme.surfaceContainer.copy(alpha = alpha),
        selectedContainer = scheme.secondaryContainer.copy(alpha = alpha),
        topBar = (if (CardConfig.isCustomBackgroundEnabled) scheme.surfaceContainerLow else scheme.background).copy(alpha = alpha),
        bottomBar = scheme.surfaceContainerHigh.copy(alpha = alpha),
        editor = scheme.surfaceContainerLowest.copy(alpha = alpha),
    )
    YukiFBProvider(
        sceneBackground = { BackgroundLayer() },
        style = if (isExpressiveUi) YukiFBStyle.Expressive else YukiFBStyle.Classic,
        colors = colors,
        motion = if (LocalYukiMotion.current.enabled) YukiFBMotion.Full else YukiFBMotion.None,
        icon = { vector, description, modifier, tint -> YukiIcon(vector, description, modifier, tint) },
    ) {
        RamdiskEditorContent(navigator, partitionName, targetSlot)
    }
}

@Composable
private fun RamdiskEditorContent(
    navigator: DestinationsNavigator,
    partitionName: String,
    targetSlot: String?,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val snackbarHost = rememberSnackbarController()
    var retryGeneration by remember { mutableIntStateOf(0) }
    var loadState by remember {
        mutableStateOf<RamdiskEditorLoadState>(RamdiskEditorLoadState.Loading)
    }
    var selectedFragmentIndex by remember { mutableStateOf<Int?>(null) }
    var pendingImportDirectory by remember { mutableStateOf<EntryId?>(null) }
    var pendingExportEntry by remember { mutableStateOf<FileEntry?>(null) }
    var importGeneration by remember { mutableIntStateOf(0) }
    var isImporting by remember { mutableStateOf(false) }
    var isExportingFile by remember { mutableStateOf(false) }
    var isExportingImage by remember { mutableStateOf(false) }
    var isDumping by remember { mutableStateOf(false) }
    var hasRebuiltImage by remember { mutableStateOf(false) }
    var hasUnexportedImage by remember { mutableStateOf(false) }
    var openedTextFile by remember { mutableStateOf<OpenedTextFile?>(null) }
    var openedElfFile by remember { mutableStateOf<OpenedElfFile?>(null) }
    var openedHexFile by remember { mutableStateOf<OpenedHexFile?>(null) }
    var pendingOpenEntry by remember { mutableStateOf<FileEntry?>(null) }
    var checksumEntry by remember { mutableStateOf<FileEntry?>(null) }
    var checksumController by remember { mutableStateOf<FileChecksumController?>(null) }
    var checksumSelection by remember { mutableStateOf(defaultRamdiskChecksumIds) }
    val checksumState = checksumController?.state?.collectAsState()?.value
    var externalOperation by remember { mutableStateOf<Job?>(null) }
    var externalTitle by remember { mutableStateOf("") }
    var externalProgress by remember { mutableStateOf<FileByteProgress?>(null) }
    var externalCancelling by remember { mutableStateOf(false) }
    DisposableEffect(checksumController) {
        val controller = checksumController
        onDispose { controller?.cancel() }
    }
    val hexState = openedHexFile?.controller?.state?.collectAsState()?.value
    DisposableEffect(openedHexFile) {
        val editor = openedHexFile
        onDispose {
            if (editor != null) CoroutineScope(Dispatchers.IO).launch { editor.controller.close() }
        }
    }
    var pendingTextFile by remember { mutableStateOf<PendingTextFile?>(null) }
    var isOpeningFile by remember { mutableStateOf(false) }
    var isSavingTextFile by remember { mutableStateOf(false) }
    var textOperationJob by remember { mutableStateOf<Job?>(null) }
    var discardPrompt by remember { mutableStateOf<DiscardPrompt?>(null) }
    var pendingExit by remember { mutableStateOf<(() -> Unit)?>(null) }
    val predictiveBack = LocalPredictiveBackEnabled.current
    val leaveGuard = LocalNavigationLeaveGuard.current
    val sessionOperationJobs = remember { mutableSetOf<Job>() }

    fun launchTextOperation(block: suspend () -> Unit) {
        sessionOperationJobs.removeAll { it.isCompleted }
        textOperationJob?.cancel()
        val job = scope.launch {
            try {
                block()
            } finally {
                if (textOperationJob === currentCoroutineContext()[Job]) {
                    textOperationJob = null
                }
            }
        }
        textOperationJob = job
        sessionOperationJobs += job
    }

    fun launchSessionOperation(block: suspend () -> Unit) {
        sessionOperationJobs.removeAll { it.isCompleted }
        sessionOperationJobs += scope.launch { block() }
    }

    LaunchedEffect(partitionName, targetSlot, retryGeneration) {
        val previousImage = (loadState as? RamdiskEditorLoadState.Ready)?.image
        val previousOperations = sessionOperationJobs.toList()
        previousOperations.forEach { it.cancel() }
        checksumController?.cancel()
        checksumEntry = null
        checksumController = null
        openedHexFile?.controller?.close()
        openedHexFile = null
        pendingOpenEntry = null
        previousImage?.closeNow()
        previousOperations.joinAll()
        sessionOperationJobs.clear()
        textOperationJob = null
        openedTextFile = null
        openedElfFile = null
        pendingTextFile = null
        pendingImportDirectory = null
        pendingExportEntry = null
        importGeneration = 0
        isImporting = false
        isExportingFile = false
        isExportingImage = false
        isOpeningFile = false
        isSavingTextFile = false
        isDumping = false
        hasRebuiltImage = false
        hasUnexportedImage = false
        discardPrompt = null
        pendingExit = null
        loadState = RamdiskEditorLoadState.Loading
        selectedFragmentIndex = null
        loadState = try {
            prepareRamdiskEditor(
                context = context,
                partitionName = partitionName,
                targetSlot = targetSlot,
            )
        } catch (error: CancellationException) {
            throw error
        } catch (error: Throwable) {
            RamdiskEditorLoadState.Error(
                error.message ?: resources.getString(R.string.partition_unknown)
            )
        }
    }

    val ready = loadState as? RamdiskEditorLoadState.Ready
    DisposableEffect(ready) {
        onDispose {
            ready?.image?.closeNow()
            ready?.sourceImage?.delete()
            ready?.outputImage?.delete()
        }
    }

    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri: Uri? ->
        val parentId = pendingImportDirectory
        pendingImportDirectory = null
        val backend = (loadState as? RamdiskEditorLoadState.Ready)?.image
            ?.fragments
            ?.getOrNull(selectedFragmentIndex ?: 0)
        if (uri != null && parentId != null && backend != null) {
            launchSessionOperation {
                isImporting = true
                runCatching {
                    val name = withContext(Dispatchers.IO) {
                        queryDisplayName(context, uri)
                            ?: resources.getString(R.string.ramdisk_editor_import_fallback_name)
                    }
                    backend.importFile(
                        parentId = parentId,
                        name = name,
                        source = FileContentSource {
                            context.contentResolver.openInputStream(uri)
                                ?: throw IOException(
                                    resources.getString(R.string.ramdisk_editor_document_open_failed)
                                )
                        },
                    )
                }.onSuccess {
                    importGeneration++
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_import_failed,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
                isImporting = false
            }
        }
    }

    val fileExportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri: Uri? ->
        val entry = pendingExportEntry
        pendingExportEntry = null
        val backend = (loadState as? RamdiskEditorLoadState.Ready)?.image
            ?.fragments
            ?.getOrNull(selectedFragmentIndex ?: 0)
        if (uri != null && entry != null && backend != null) {
            launchSessionOperation {
                isExportingFile = true
                runCatching {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                            backend.read(entry.id) { input ->
                                input.copyTo(output)
                            }
                        } ?: throw IOException(
                            resources.getString(R.string.ramdisk_editor_document_open_failed)
                        )
                    }
                }.onSuccess {
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_file_export_success,
                            entry.name,
                        )
                    )
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_file_export_failed,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
                isExportingFile = false
            }
        }
    }

    val imageExportLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri: Uri? ->
        val outputImage = (loadState as? RamdiskEditorLoadState.Ready)?.image?.outputImage
        if (uri != null && outputImage?.isFile == true && !isExportingImage) {
            isExportingImage = true
            launchSessionOperation {
                val failure = try {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                            outputImage.inputStream().buffered().use { input ->
                                input.copyTo(output)
                            }
                        } ?: throw IOException(
                            resources.getString(R.string.ramdisk_editor_document_open_failed)
                        )
                    }
                    hasUnexportedImage = false
                    null
                } catch (error: CancellationException) {
                    throw error
                } catch (error: Throwable) {
                    error
                } finally {
                    isExportingImage = false
                }
                if (failure == null) {
                    snackbarHost.showSnackbar(
                        resources.getString(R.string.ramdisk_editor_export_success)
                    )
                } else {
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.ramdisk_editor_export_failed,
                            failure.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
            }
        }
    }

    when (val state = loadState) {
        RamdiskEditorLoadState.Loading -> {
            RamdiskEditorStatusScreen(
                title = stringResource(R.string.ramdisk_editor_title, partitionName),
                message = stringResource(R.string.ramdisk_editor_preparing),
                onBack = { navigator.popBackStack() },
                loading = true,
            )
        }

        is RamdiskEditorLoadState.Error -> {
            RamdiskEditorStatusScreen(
                title = stringResource(R.string.ramdisk_editor_title, partitionName),
                message = state.message,
                onBack = { navigator.popBackStack() },
                loading = false,
                onRetry = { retryGeneration++ },
            )
        }

        is RamdiskEditorLoadState.Ready -> {
            val image = state.image
            val dirty by image.dirty.collectAsState()
            val multipleRamdisks = image.fragments.size > 1
            var lastRootBackAt by remember(image) { mutableLongStateOf(0L) }
            val hostBack = rememberYukiFBBackState()
            val browserSessions = remember(image) { mutableMapOf<Int, RamdiskBrowserSession>() }
            DisposableEffect(image) {
                onDispose { browserSessions.values.forEach { it.scope.cancel() } }
            }
            LaunchedEffect(lastRootBackAt) {
                if (lastRootBackAt != 0L) {
                    delay(ROOT_EXIT_CONFIRM_INTERVAL_MILLIS)
                    lastRootBackAt = 0L
                }
            }
            val hostBusy = hexState?.busy == true || checksumState?.running == true || externalOperation != null || isImporting || isExportingFile || isExportingImage || isOpeningFile || isSavingTextFile || isDumping

            fun requestScreenExit(navigate: () -> Unit) {
                val decision = RamdiskBackContext(
                    busy = hostBusy || browserSessions.values.any {
                        it.controller.state.value.let { page -> page.isBusy || page.isLoading }
                    },
                    hexOpen = openedHexFile != null,
                    hexDirty = openedHexFile?.controller?.state?.value?.dirty == true,
                    textOpen = openedTextFile != null,
                    textDirty = openedTextFile?.state?.hasUnsavedChanges == true,
                    archiveChanged = image.dirty.value || hasUnexportedImage,
                ).action(leavingScreen = true)
                when (decision) {
                    RamdiskBackAction.Blocked -> Unit
                    RamdiskBackAction.ConfirmText -> {
                        pendingExit = { requestScreenExit(navigate) }
                        discardPrompt = DiscardPrompt.TEXT_FILE
                    }
                    RamdiskBackAction.ConfirmHex -> {
                        pendingExit = { requestScreenExit(navigate) }
                        discardPrompt = DiscardPrompt.HEX_FILE
                    }
                    RamdiskBackAction.ConfirmArchive -> {
                        pendingExit = navigate
                        discardPrompt = DiscardPrompt.ARCHIVE
                    }
                    else -> navigate()
                }
            }
            val leaveInterceptor = rememberUpdatedState<(() -> Unit, () -> Unit) -> Unit> { navigate, intercepted ->
                if (hostBusy || dirty || hasUnexportedImage || hexState?.dirty == true || openedTextFile?.state?.hasUnsavedChanges == true ||
                    browserSessions.values.any { it.controller.state.value.let { page -> page.isBusy || page.isLoading } }) {
                    // The predictive transaction is cancelled before showing a dialog.
                    // A confirmed exit starts a fresh pop, not the cancelled continuation.
                    intercepted()
                    requestScreenExit { navigator.popBackStack() }
                } else navigate()
            }
            DisposableEffect(image, leaveGuard) {
                leaveGuard.register(image, RamdiskEditorScreenDestination.route) { navigate, intercepted ->
                    leaveInterceptor.value(navigate, intercepted)
                }
                onDispose { leaveGuard.unregister(image) }
            }

            fun requestEditorExit() {
                if (hostBusy || dirty || hasUnexportedImage) {
                    requestScreenExit { navigator.popBackStack() }
                    return
                }
                val now = SystemClock.elapsedRealtime()
                if (
                    lastRootBackAt != 0L &&
                    now - lastRootBackAt in 0L..ROOT_EXIT_CONFIRM_INTERVAL_MILLIS
                ) {
                    snackbarHost.dismiss()
                    navigator.popBackStack()
                } else {
                    lastRootBackAt = now
                    scope.launch {
                        snackbarHost.showSnackbar(
                            message = resources.getString(
                                R.string.ramdisk_editor_press_back_again
                            ),
                            duration = SnackbarDuration.Short,
                        )
                    }
                }
            }

            fun rebuildImage() {
                if (!dirty || hostBusy) return
                launchSessionOperation {
                    isDumping = true
                    runCatching { image.dump() }
                        .onSuccess {
                            hasRebuiltImage = true
                            hasUnexportedImage = true
                            snackbarHost.showSnackbar(
                                resources.getString(R.string.ramdisk_editor_rebuild_success)
                            )
                        }
                        .onFailure { error ->
                            snackbarHost.showSnackbar(
                                resources.getString(
                                    R.string.ramdisk_editor_rebuild_failed,
                                    error.message
                                        ?: resources.getString(R.string.partition_unknown),
                                )
                            )
                        }
                    isDumping = false
                }
            }

            val exportImage = {
                imageExportLauncher.launch(buildExportFileName(partitionName))
            }

            LaunchedEffect(dirty, hasUnexportedImage, selectedFragmentIndex) {
                lastRootBackAt = 0L
            }

            val selectorVisible = multipleRamdisks && selectedFragmentIndex == null
            val sceneKey = listOf(selectedFragmentIndex, openedTextFile?.entry?.id, openedElfFile?.entry?.id, openedHexFile?.entry?.id)
            val sceneLevel = if (selectorVisible) 0 else if (openedTextFile != null || openedElfFile != null || openedHexFile != null) 2 else 1
            val previousLevel = remember(image) { intArrayOf(sceneLevel) }
            val sceneDirection = remember(sceneKey) {
                (if (sceneLevel < previousLevel[0]) -1 else 1).also { previousLevel[0] = sceneLevel }
            }
            YukiFBScene(
                sceneKey = sceneKey, modifier = Modifier.fillMaxSize(), direction = sceneDirection,
                backState = hostBack,
                backPreview = if (openedTextFile != null || openedElfFile != null || openedHexFile != null) {
                    {
                        val backend = image.fragments.getOrElse(selectedFragmentIndex ?: 0) { image.fragments.first() }
                        browserSessions[backend.fragment.index]?.let { session ->
                            val preview by session.controller.state.collectAsState()
                            FileBrowserPreview(
                                preview, session.presentation, modifier = Modifier.fillMaxSize(),
                                showCloseButton = true, importEnabled = true,
                                topBarActions = {
                                    RamdiskImageActions(dirty, hasRebuiltImage, isDumping, {}, {})
                                },
                            )
                        }
                    }
                } else if (!selectorVisible && multipleRamdisks) {
                    {
                        RamdiskFragmentSelector(
                            partitionName, image.fragments, dirty, hasRebuiltImage, isDumping,
                            snackbarHost, {}, {}, {}, {},
                        )
                    }
                } else null,
            ) {
                if (selectorVisible) {
                    YukiFBBackHandler(
                        state = hostBack,
                        targetKey = listOf(image, lastRootBackAt, dirty, hasUnexportedImage, hostBusy),
                        enabled = discardPrompt == null && (lastRootBackAt == 0L || dirty || hasUnexportedImage || hostBusy),
                        predictiveBackEnabled = predictiveBack,
                        previewEnabled = false,
                        onBack = ::requestEditorExit,
                    )
                    RamdiskFragmentSelector(
                        partitionName = partitionName,
                        fragments = image.fragments,
                        dirty = dirty,
                        hasRebuiltImage = hasRebuiltImage,
                        isDumping = isDumping,
                        snackbarHost = snackbarHost,
                        onBack = ::requestEditorExit,
                        onSelect = { selectedFragmentIndex = it.fragment.index },
                        onDump = ::rebuildImage,
                        onExportImage = exportImage,
                    )
                } else {
                    val backend = image.fragments.getOrElse(selectedFragmentIndex ?: 0) {
                        image.fragments.first()
                    }
                    val browserSession = remember(image, backend) {
                        browserSessions.getOrPut(backend.fragment.index) {
                            val childScope = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
                            RamdiskBrowserSession(
                                FileBrowserController(backend, backend.rootEntry, childScope),
                                FileBrowserPresentationState(), childScope,
                            )
                        }
                    }
                    val browserController = browserSession.controller
                    val browserPresentation = browserSession.presentation
                    val browserState by browserController.state.collectAsState()

                    LaunchedEffect(importGeneration, backend) {
                        if (importGeneration > 0) {
                            browserPresentation.invalidatePreviews()
                            browserController.dispatch(FileBrowserAction.Refresh)
                        }
                    }

                    LaunchedEffect(
                        browserState.currentDirectory.id,
                        browserState.selectedIds,
                        browserState.search != null,
                    ) {
                        lastRootBackAt = 0L
                    }

                    fun openFile(entry: FileEntry, kind: RamdiskOpenKind) {
                        if (!isOpeningFile) {
                            launchTextOperation {
                                isOpeningFile = true
                                val failure = try {
                                    require(entry.type == FileEntryType.REGULAR_FILE) {
                                        resources.getString(
                                            R.string.ramdisk_editor_regular_files_only
                                        )
                                    }
                                    if (kind == RamdiskOpenKind.HEX) {
                                        val source = backend.openBinary(entry.id)
                                        val storage = RamdiskHexStorage.create(checkNotNull(state.sourceImage.parentFile))
                                        try {
                                            openedHexFile = OpenedHexFile(entry, HexEditorController(
                                                source = source,
                                                parentScope = browserSession.scope,
                                                storage = storage,
                                                committer = backend.binaryCommitter(entry.id) {
                                                    browserPresentation.invalidatePreviews()
                                                    browserController.dispatch(FileBrowserAction.Refresh)
                                                },
                                                codecs = ramdiskHexCodecs,
                                            ))
                                        } catch (error: Throwable) {
                                            storage.close()
                                            throw error
                                        }
                                    } else if (kind == RamdiskOpenKind.ELF) {
                                        pendingTextFile = null
                                        openedElfFile = OpenedElfFile(
                                            entry = entry,
                                            header = backend.readElfHeader(entry.id),
                                        )
                                    } else {
                                        require(
                                            (entry.size ?: 0L) <=
                                                MAX_RAMDISK_TEXT_FILE_SIZE
                                        ) {
                                            resources.getString(
                                                R.string.ramdisk_editor_text_too_large
                                            )
                                        }
                                        val bytes = backend.read(entry.id) { input ->
                                            readRamdiskTextBytes(input)
                                        }
                                        val decoded = try {
                                            withContext(Dispatchers.Default) {
                                                decodeInitialRamdiskText(bytes)
                                            }
                                        } catch (error: CancellationException) {
                                            throw error
                                        } catch (_: IOException) {
                                            null
                                        }
                                        if (
                                            decoded == null ||
                                            decoded.requiresEncodingConfirmation
                                        ) {
                                            pendingTextFile = PendingTextFile(
                                                entry = entry,
                                                sourceBytes = bytes,
                                            )
                                        } else {
                                            pendingTextFile = null
                                            openedTextFile = OpenedTextFile(
                                                entry = entry,
                                                sourceBytes = bytes,
                                                decoded = decoded,
                                            )
                                        }
                                    }
                                    null
                                } catch (error: CancellationException) {
                                    throw error
                                } catch (error: Throwable) {
                                    error
                                } finally {
                                    isOpeningFile = false
                                }
                                if (failure != null) {
                                    snackbarHost.showSnackbar(
                                        resources.getString(
                                            R.string.ramdisk_editor_open_failed,
                                            failure.message
                                                ?: resources.getString(
                                                    R.string.partition_unknown
                                                ),
                                        )
                                    )
                                }
                            }
                        }
                    }

                    val checksumCapability = remember(backend) { RamdiskFileChecksums(RamdiskContentReader(backend::readSnapshot)) }
                    val externalCapability = remember(backend, context) { RamdiskExternalActions(context, backend::stat, RamdiskContentReader(backend::readSnapshot)) }
                    fun startExternal(entry: FileEntry, share: Boolean) {
                        if (externalOperation != null) return
                        externalTitle = resources.getString(if (share) com.anatdx.yukifb.R.string.yukifb_action_share else com.anatdx.yukifb.R.string.yukifb_action_open_externally)
                        externalProgress = null
                        externalCancelling = false
                        val job = scope.launch(start = CoroutineStart.LAZY) {
                            try {
                                if (share) externalCapability.share(entry.id) { externalProgress = it }
                                else externalCapability.openExternally(entry.id) { externalProgress = it }
                            } catch (cancelled: CancellationException) {
                                throw cancelled
                            } catch (failure: Exception) {
                                currentCoroutineContext().ensureActive()
                                val message = resources.getString(when {
                                    failure is android.content.ActivityNotFoundException -> R.string.ramdisk_external_no_app
                                    failure is FileContentChangedException -> R.string.ramdisk_external_changed
                                    generateSequence<Throwable>(failure) { it.cause }.any { it is android.system.ErrnoException && it.errno == android.system.OsConstants.ENOSPC } -> R.string.ramdisk_external_no_space
                                    else -> R.string.ramdisk_external_failed
                                })
                                // Keep errors tied to this operation so cancellation suppresses late UI.
                                externalOperation = null
                                externalProgress = null
                                externalCancelling = false
                                snackbarHost.showSnackbar(message)
                            }
                        }
                        externalOperation = job
                        sessionOperationJobs.removeAll { it.isCompleted }
                        sessionOperationJobs += job
                        job.invokeOnCompletion {
                            if (externalOperation === job) {
                                externalOperation = null
                                externalProgress = null
                                externalCancelling = false
                            }
                        }
                        job.start()
                    }
                    pendingOpenEntry?.let { entry ->
                        RamdiskFileActionsDialog(
                            entry, ramdiskFileActions(entry, backend.supportsHex, checksumCapability, externalCapability, externalCapability),
                            onDismiss = { pendingOpenEntry = null },
                            onAction = { action ->
                                pendingOpenEntry = null
                                when (action) {
                                    RamdiskFileAction.TEXT -> openFile(entry, RamdiskOpenKind.TEXT)
                                    RamdiskFileAction.ELF -> openFile(entry, RamdiskOpenKind.ELF)
                                    RamdiskFileAction.HEX -> openFile(entry, RamdiskOpenKind.HEX)
                                    RamdiskFileAction.SHARE -> startExternal(entry, true)
                                    RamdiskFileAction.EXTERNAL -> startExternal(entry, false)
                                    RamdiskFileAction.CHECKSUMS -> {
                                        checksumEntry = entry
                                        checksumController = FileChecksumController(entry, checksumCapability, scope, checksumSelection)
                                    }
                                }
                            },
                        )
                    }
                    if (checksumEntry != null) checksumController?.let { controller ->
                        FileChecksumsDialog(controller) {
                            checksumSelection = controller.state.value.selectedIds
                            checksumEntry = null
                        }
                    }
                    if (externalOperation != null) FileActionProgressDialog(externalTitle, externalProgress, externalCancelling) {
                        externalCancelling = true
                        externalOperation?.cancel()
                    }

                    val handleBrowserBack: () -> Unit = {
                        val editor = openedTextFile
                        val decision = RamdiskBackContext(
                            busy = hostBusy || browserState.isBusy || browserState.isLoading,
                            textOpen = editor != null,
                            textDirty = editor?.state?.hasUnsavedChanges == true,
                            elfOpen = openedElfFile != null,
                            hexOpen = openedHexFile != null,
                            hexDirty = hexState?.dirty == true,
                            selecting = browserState.isSelectionMode,
                            searching = browserState.search != null,
                            hasParent = browserState.breadcrumbs.size > 1,
                            multipleFragments = multipleRamdisks,
                            archiveChanged = image.dirty.value || hasUnexportedImage,
                            exitPrimed = lastRootBackAt != 0L,
                        ).action()
                        when (decision) {
                            RamdiskBackAction.Blocked -> Unit
                            RamdiskBackAction.ConfirmText -> {
                                pendingExit = null
                                discardPrompt = DiscardPrompt.TEXT_FILE
                            }

                            RamdiskBackAction.ConfirmHex -> {
                                pendingExit = null
                                discardPrompt = DiscardPrompt.HEX_FILE
                            }
                            RamdiskBackAction.CloseHex -> openedHexFile = null
                            RamdiskBackAction.CloseText -> openedTextFile = null
                            RamdiskBackAction.CloseElf -> openedElfFile = null
                            RamdiskBackAction.ClearSelection -> {
                                browserController.dispatch(FileBrowserAction.ClearSelection)
                            }

                            RamdiskBackAction.StopSearch -> {
                                browserController.dispatch(FileBrowserAction.StopSearch)
                            }

                            RamdiskBackAction.ParentDirectory -> {
                                browserController.dispatch(FileBrowserAction.NavigateUp)
                            }

                            RamdiskBackAction.FragmentSelector -> {
                                selectedFragmentIndex = null
                            }

                            else -> requestEditorExit()
                        }
                    }

                    val viewerVisible = openedTextFile != null || openedElfFile != null || openedHexFile != null
                    val browserOwnsBack = !viewerVisible && !hostBusy && !browserState.isBusy && !browserState.isLoading &&
                        (browserState.isSelectionMode || browserState.search != null || browserState.breadcrumbs.size > 1)
                    val rootCanLeave = !viewerVisible && !multipleRamdisks && !dirty && !hasUnexportedImage &&
                        !hostBusy && !browserState.isBusy && !browserState.isLoading && lastRootBackAt != 0L
                    YukiFBBackHandler(
                        state = hostBack,
                        targetKey = listOf(sceneKey, browserState.currentDirectory.id, hostBusy, browserState.isBusy,
                            browserState.isLoading, openedTextFile?.state?.hasUnsavedChanges, hexState?.dirty, dirty, hasUnexportedImage, lastRootBackAt),
                        enabled = discardPrompt == null && pendingTextFile == null && pendingOpenEntry == null && checksumEntry == null && !browserOwnsBack && !rootCanLeave,
                        predictiveBackEnabled = predictiveBack,
                        previewEnabled = !hostBusy && !browserState.isBusy && !browserState.isLoading &&
                            openedTextFile?.state?.hasUnsavedChanges != true && hexState?.dirty != true && (viewerVisible || multipleRamdisks),
                        onBack = handleBrowserBack,
                    )
                    FileBrowserBackHandler(
                        state = browserState, presentationState = browserPresentation,
                        onAction = browserController::dispatch,
                        enabled = !viewerVisible && !hostBusy && discardPrompt == null && pendingTextFile == null && pendingOpenEntry == null && checksumEntry == null,
                        predictiveBackEnabled = predictiveBack,
                    )

                    Box(Modifier.fillMaxSize()) {
                        val editor = openedTextFile
                        val elfFile = openedElfFile
                        if (editor != null) {
                            TextFileEditor(
                                fileName = editor.entry.name,
                                state = editor.state,
                                onSave = { request ->
                                    if (!isSavingTextFile) {
                                        launchTextOperation {
                                            isSavingTextFile = true
                                            val failure = try {
                                                val encoded =
                                                    withContext(Dispatchers.Default) {
                                                        encodeRamdiskText(
                                                            text = request.text,
                                                            encoding = request.encoding,
                                                            preservedByteOrderMark =
                                                                editor.sourceByteOrderMark.takeIf {
                                                                    editor.sourceEncoding ==
                                                                        request.encoding
                                                                },
                                                        )
                                                    }
                                                backend.replace(
                                                    editor.entry.id,
                                                    FileContentSource {
                                                        ByteArrayInputStream(encoded.bytes)
                                                    },
                                                )
                                                editor.updatePersistedSource(
                                                    encoded = encoded,
                                                    encoding = request.encoding,
                                                )
                                                if (
                                                    editor.state.createSaveRequest() == request
                                                ) {
                                                    editor.state.markSaved()
                                                }
                                                browserPresentation.invalidatePreviews()
                                                browserController.dispatch(FileBrowserAction.Refresh)
                                                null
                                            } catch (error: CancellationException) {
                                                throw error
                                            } catch (error: Throwable) {
                                                error
                                            } finally {
                                                isSavingTextFile = false
                                            }
                                            if (failure != null) {
                                                snackbarHost.showSnackbar(
                                                    resources.getString(
                                                        R.string.ramdisk_editor_text_save_failed,
                                                        failure.message
                                                            ?: resources.getString(
                                                                R.string.partition_unknown
                                                            ),
                                                    )
                                                )
                                            }
                                        }
                                    }
                                },
                                onReopenWithEncoding = { encoding ->
                                    if (!isOpeningFile && !isSavingTextFile) {
                                        launchTextOperation {
                                            isOpeningFile = true
                                            val failure = try {
                                                val decoded = withContext(Dispatchers.Default) {
                                                    decodeRamdiskText(
                                                        bytes = editor.sourceBytes,
                                                        encoding = encoding,
                                                    )
                                                }
                                                editor.updateDecodedSource(decoded)
                                                null
                                            } catch (error: CancellationException) {
                                                throw error
                                            } catch (error: Throwable) {
                                                error
                                            } finally {
                                                isOpeningFile = false
                                            }
                                            if (failure != null) {
                                                snackbarHost.showSnackbar(
                                                    resources.getString(
                                                        R.string.ramdisk_editor_open_failed,
                                                        failure.message
                                                            ?: resources.getString(
                                                                R.string.partition_unknown
                                                            ),
                                                    )
                                                )
                                            }
                                        }
                                    }
                                },
                                onClose = handleBrowserBack,
                                confirmClose = false,
                                modifier = Modifier.fillMaxSize(),
                                isSaving = isSavingTextFile || isOpeningFile,
                                availableEncodings = supportedRamdiskTextEncodings,
                            )
                        } else if (openedHexFile != null) {
                            val hex = checkNotNull(openedHexFile)
                            HexEditor(
                                fileName = hex.entry.name,
                                controller = hex.controller,
                                onClose = handleBrowserBack,
                                confirmClose = false,
                                modifier = Modifier.fillMaxSize(),
                            )
                        } else if (elfFile != null) {
                            ElfSummaryViewer(
                                fileName = elfFile.entry.name,
                                header = elfFile.header,
                                onClose = handleBrowserBack,
                                modifier = Modifier.fillMaxSize(),
                            )
                        } else {
                            FileBrowser(
                                controller = browserController,
                                presentationState = browserPresentation,
                                onOpenFile = { entry ->
                                    if (!hostBusy && entry.type == FileEntryType.REGULAR_FILE) pendingOpenEntry = entry
                                },
                                onImport = { directory ->
                                    if (!isImporting) {
                                        pendingImportDirectory = directory.id
                                        importLauncher.launch(arrayOf("*/*"))
                                    }
                                },
                                onExport = { entry ->
                                    if (!isExportingFile) {
                                        pendingExportEntry = entry
                                        fileExportLauncher.launch(entry.name)
                                    }
                                },
                                onClose = handleBrowserBack,
                                modifier = Modifier.fillMaxSize(),
                                topBarActions = {
                                    RamdiskImageActions(
                                        dirty = dirty,
                                        hasRebuiltImage = hasRebuiltImage,
                                        isDumping = isDumping,
                                        onDump = ::rebuildImage,
                                        onExportImage = exportImage,
                                    )
                                },
                            )
                        }

                        SnackbarHost(
                            hostState = snackbarHost.hostState,
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .padding(16.dp),
                        )
                    }
                }
            }
            if (hostBusy && openedHexFile == null && checksumEntry == null && externalOperation == null) {
                RamdiskOperationOverlay(
                    label = stringResource(when {
                        isImporting -> R.string.ramdisk_editor_importing
                        isExportingFile -> R.string.ramdisk_editor_exporting
                        isExportingImage -> R.string.ramdisk_editor_exporting_image
                        isDumping -> R.string.ramdisk_editor_rebuilding
                        isSavingTextFile -> R.string.ramdisk_editor_saving
                        checksumState?.running == true -> com.anatdx.yukifb.R.string.yukifb_action_cancelling
                        else -> R.string.ramdisk_editor_opening
                    }),
                )
            }
        }
    }

    when (discardPrompt) {
        DiscardPrompt.ARCHIVE -> {
            DiscardDialog(
                title = stringResource(R.string.ramdisk_editor_discard_archive_title),
                message = stringResource(R.string.ramdisk_editor_discard_archive_message),
                dismissLabel = stringResource(R.string.ramdisk_editor_cancel),
                confirmLabel = stringResource(R.string.ramdisk_editor_confirm),
                onDismiss = { discardPrompt = null; pendingExit = null },
                onDiscard = {
                    discardPrompt = null
                    val navigate = pendingExit
                    pendingExit = null
                    if (navigate != null) navigate() else navigator.popBackStack()
                },
            )
        }

        DiscardPrompt.HEX_FILE -> {
            YukiAlertDialog(
                onDismissRequest = { discardPrompt = null; pendingExit = null },
                title = { Text(stringResource(com.anatdx.yukifb.hex.R.string.yukifb_hex_unsaved)) },
                text = { Text(stringResource(com.anatdx.yukifb.hex.R.string.yukifb_hex_unsaved_message)) },
                confirmButton = {
                    TextButton(enabled = hexState?.busy != true, onClick = {
                        val editor = openedHexFile
                        if (editor != null) launchSessionOperation {
                            try {
                                editor.controller.save()
                                discardPrompt = null
                                openedHexFile = null
                                val navigate = pendingExit
                                pendingExit = null
                                navigate?.invoke()
                            } catch (error: CancellationException) { throw error }
                            catch (error: Exception) {
                                snackbarHost.showSnackbar(resources.getString(
                                    com.anatdx.yukifb.hex.R.string.yukifb_hex_error, error.message.orEmpty()))
                            }
                        }
                    }) { Text(stringResource(com.anatdx.yukifb.hex.R.string.yukifb_hex_save)) }
                },
                dismissButton = {
                    Row {
                        TextButton(enabled = hexState?.busy != true, onClick = {
                            discardPrompt = null
                            openedHexFile = null
                            val navigate = pendingExit
                            pendingExit = null
                            navigate?.invoke()
                        }) { Text(stringResource(R.string.ramdisk_editor_discard)) }
                        TextButton(enabled = hexState?.busy != true, onClick = {
                            discardPrompt = null; pendingExit = null
                        }) { Text(stringResource(R.string.ramdisk_editor_cancel)) }
                    }
                },
            )
        }

        DiscardPrompt.TEXT_FILE -> {
            DiscardDialog(
                title = stringResource(R.string.ramdisk_editor_discard_text_title),
                message = stringResource(R.string.ramdisk_editor_discard_text_message),
                dismissLabel = stringResource(R.string.ramdisk_editor_continue_editing),
                confirmLabel = stringResource(R.string.ramdisk_editor_discard),
                onDismiss = { discardPrompt = null; pendingExit = null },
                onDiscard = {
                    discardPrompt = null
                    openedTextFile = null
                    val navigate = pendingExit
                    pendingExit = null
                    navigate?.invoke()
                },
            )
        }

        null -> Unit
    }

    pendingTextFile?.let { pending ->
        TextEncodingDialog(
            fileName = pending.entry.name,
            encodings = supportedRamdiskTextEncodings,
            busy = isOpeningFile,
            onDismiss = {
                if (!isOpeningFile) {
                    pendingTextFile = null
                }
            },
            onEncodingSelected = { encoding ->
                if (!isOpeningFile) {
                    launchTextOperation {
                        isOpeningFile = true
                        val failure = try {
                            val decoded = withContext(Dispatchers.Default) {
                                decodeRamdiskText(
                                    bytes = pending.sourceBytes,
                                    encoding = encoding,
                                )
                            }
                            openedTextFile = OpenedTextFile(
                                entry = pending.entry,
                                sourceBytes = pending.sourceBytes,
                                decoded = decoded,
                            )
                            pendingTextFile = null
                            null
                        } catch (error: CancellationException) {
                            throw error
                        } catch (error: Throwable) {
                            error
                        } finally {
                            isOpeningFile = false
                        }
                        if (failure != null) {
                            snackbarHost.showSnackbar(
                                resources.getString(
                                    R.string.ramdisk_editor_open_failed,
                                    failure.message
                                        ?: resources.getString(
                                            R.string.partition_unknown
                                        ),
                                )
                            )
                        }
                    }
                }
            },
        )
    }
}

@Composable
private fun RamdiskImageActions(
    dirty: Boolean,
    hasRebuiltImage: Boolean,
    isDumping: Boolean,
    onDump: () -> Unit,
    onExportImage: () -> Unit,
) {
    IconButton(
        enabled = hasRebuiltImage && !dirty && !isDumping,
        onClick = onExportImage,
    ) {
        YukiIcon(
            Icons.Filled.Download,
            contentDescription = stringResource(R.string.ramdisk_editor_export_image),
        )
    }
    IconButton(
        enabled = dirty && !isDumping,
        onClick = onDump,
    ) {
        if (isDumping) {
            CircularProgressIndicator(
                modifier = Modifier.size(22.dp),
                strokeWidth = 2.dp,
            )
        } else {
            YukiIcon(
                Icons.Filled.Save,
                contentDescription = stringResource(R.string.ramdisk_editor_rebuild_image),
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RamdiskFragmentSelector(
    partitionName: String,
    fragments: List<YrcpRamdiskBackend>,
    dirty: Boolean,
    hasRebuiltImage: Boolean,
    isDumping: Boolean,
    snackbarHost: SnackbarController,
    onBack: () -> Unit,
    onSelect: (YrcpRamdiskBackend) -> Unit,
    onDump: () -> Unit,
    onExportImage: () -> Unit,
) {
    val title = @Composable {
        Text(stringResource(R.string.ramdisk_editor_fragments_title, partitionName))
    }
    val navigationIcon = @Composable {
        IconButton(onClick = onBack) {
            YukiIcon(
                Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.ramdisk_editor_back),
            )
        }
    }
    val actions: @Composable RowScope.() -> Unit = {
        RamdiskImageActions(
            dirty = dirty,
            hasRebuiltImage = hasRebuiltImage,
            isDumping = isDumping,
            onDump = onDump,
            onExportImage = onExportImage,
        )
    }
    Scaffold(
        containerColor = Color.Transparent,
        topBar = {
            if (isExpressiveUi) {
                YukiLargeTopAppBar(
                    colors = ramdiskTopBarColors(),
                    title = title,
                    navigationIcon = navigationIcon,
                    actions = actions,
                )
            } else {
                TopAppBar(
                    colors = ramdiskTopBarColors(),
                    title = title,
                    navigationIcon = navigationIcon,
                    actions = actions,
                )
            }
        },
        snackbarHost = { SnackbarHost(snackbarHost.hostState) },
    ) { paddingValues ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues),
            contentPadding = PaddingValues(
                horizontal = if (isExpressiveUi) 16.dp else 20.dp,
                vertical = 16.dp,
            ),
            verticalArrangement = Arrangement.spacedBy(if (isExpressiveUi) 8.dp else 12.dp),
        ) {
            item {
                Text(
                    text = stringResource(R.string.ramdisk_editor_fragments_description),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 4.dp),
                )
            }
            items(
                items = fragments,
                key = { it.fragment.index },
            ) { backend ->
                val fragment = backend.fragment
                Card(
                    onClick = { onSelect(backend) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = if (isExpressiveUi) {
                        MaterialTheme.shapes.extraLarge
                    } else {
                        MaterialTheme.shapes.medium
                    },
                    colors = CardDefaults.cardColors(
                        containerColor = if (isExpressiveUi) {
                            MaterialTheme.colorScheme.secondaryContainer.copy(alpha = CardConfig.cardAlpha)
                        } else {
                            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
                        },
                    ),
                ) {
                    ListItem(
                        content = {
                            Text(
                                text = fragment.name.ifBlank {
                                    stringResource(
                                        R.string.ramdisk_editor_fragment_fallback,
                                        fragment.index + 1,
                                    )
                                },
                                style = if (isExpressiveUi) {
                                    MaterialTheme.typography.titleLarge
                                } else {
                                    MaterialTheme.typography.titleMedium
                                },
                            )
                        },
                        supportingContent = {
                            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                Text(
                                    stringResource(
                                        R.string.ramdisk_editor_fragment_summary,
                                        fragment.vendorType.toVendorRamdiskType(),
                                        fragment.compression,
                                        Formatter.formatFileSize(
                                            LocalContext.current,
                                            fragment.packedSize,
                                        ),
                                    )
                                )
                                fragment.boardId
                                    .filter { it != 0L }
                                    .takeIf { it.isNotEmpty() }
                                    ?.let { boardId ->
                                        Text(
                                            text = stringResource(
                                                R.string.ramdisk_editor_fragment_board_id,
                                                boardId.joinToString(":") {
                                                    "%08x".format(Locale.US, it)
                                                },
                                            ),
                                            style = MaterialTheme.typography.bodySmall,
                                        )
                                    }
                            }
                        },
                        leadingContent = {
                            YukiIcon(
                                Icons.Filled.Memory,
                                contentDescription = null,
                                tint = if (isExpressiveUi) {
                                    MaterialTheme.colorScheme.onSecondaryContainer
                                } else {
                                    MaterialTheme.colorScheme.primary
                                },
                            )
                        },
                        colors = ListItemDefaults.colors(
                            containerColor = androidx.compose.ui.graphics.Color.Transparent
                        ),
                    )
                }
            }
        }
    }
}

@Composable
private fun Long.toVendorRamdiskType(): String =
    when (this) {
        1L -> stringResource(R.string.ramdisk_editor_fragment_type_platform)
        2L -> stringResource(R.string.ramdisk_editor_fragment_type_recovery)
        3L -> stringResource(R.string.ramdisk_editor_fragment_type_dlkm)
        else -> stringResource(R.string.ramdisk_editor_fragment_type_none)
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RamdiskEditorStatusScreen(
    title: String,
    message: String,
    onBack: () -> Unit,
    loading: Boolean,
    onRetry: (() -> Unit)? = null,
) {
    Scaffold(
        containerColor = Color.Transparent,
        topBar = {
            TopAppBar(
                colors = ramdiskTopBarColors(),
                title = { Text(title) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        YukiIcon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.ramdisk_editor_back),
                        )
                    }
                },
            )
        },
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
                .padding(PaddingValues(24.dp)),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            if (loading) {
                CircularProgressIndicator()
            }
            Text(
                text = message,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.padding(top = 16.dp),
            )
            if (onRetry != null) {
                Button(
                    onClick = onRetry,
                    modifier = Modifier.padding(top = 20.dp),
                ) {
                    YukiIcon(Icons.Filled.Refresh, contentDescription = null)
                    Text(
                        text = stringResource(R.string.ramdisk_editor_retry),
                        modifier = Modifier.padding(start = 8.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun DiscardDialog(
    title: String,
    message: String,
    dismissLabel: String,
    confirmLabel: String,
    onDismiss: () -> Unit,
    onDiscard: () -> Unit,
) {
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(message) },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(dismissLabel)
            }
        },
        confirmButton = {
            TextButton(onClick = onDiscard) {
                Text(
                    text = confirmLabel,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        },
    )
}

@Composable
private fun TextEncodingDialog(
    fileName: String,
    encodings: List<TextFileEncoding>,
    busy: Boolean,
    onDismiss: () -> Unit,
    onEncodingSelected: (TextFileEncoding) -> Unit,
) {
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(stringResource(R.string.ramdisk_editor_choose_encoding_title, fileName))
        },
        text = {
            Column {
                Text(stringResource(R.string.ramdisk_editor_choose_encoding_message))
                encodings.forEach { encoding ->
                    TextButton(
                        onClick = { onEncodingSelected(encoding) },
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(
                            text = encoding.displayName,
                            modifier = Modifier.fillMaxWidth(),
                            textAlign = TextAlign.Start,
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = onDismiss,
                enabled = !busy,
            ) {
                Text(stringResource(R.string.ramdisk_editor_cancel))
            }
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ElfSummaryViewer(
    fileName: String,
    header: ElfHeaderInfo,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val fields = header.toDisplayFields(LocalResources.current)
    val groups = listOf(
        R.string.ramdisk_editor_elf_identity to fields.filter { it.first in setOf(
            R.string.ramdisk_editor_elf_field_file_size, R.string.ramdisk_editor_elf_field_magic,
            R.string.ramdisk_editor_elf_field_class, R.string.ramdisk_editor_elf_field_data,
            R.string.ramdisk_editor_elf_field_ident_version, R.string.ramdisk_editor_elf_field_os_abi,
            R.string.ramdisk_editor_elf_field_abi_version, R.string.ramdisk_editor_elf_field_type,
            R.string.ramdisk_editor_elf_field_machine, R.string.ramdisk_editor_elf_field_object_version,
        ) },
        R.string.ramdisk_editor_elf_entry_flags to fields.filter { it.first in setOf(
            R.string.ramdisk_editor_elf_field_entry, R.string.ramdisk_editor_elf_field_flags,
        ) },
        R.string.ramdisk_editor_elf_layout to fields.filter { it.first in setOf(
            R.string.ramdisk_editor_elf_field_header_size, R.string.ramdisk_editor_elf_field_program_offset,
            R.string.ramdisk_editor_elf_field_program_entry_size, R.string.ramdisk_editor_elf_field_program_count,
            R.string.ramdisk_editor_elf_field_section_offset, R.string.ramdisk_editor_elf_field_section_entry_size,
            R.string.ramdisk_editor_elf_field_section_count, R.string.ramdisk_editor_elf_field_section_name_index,
        ) },
    )
    Scaffold(
        containerColor = Color.Transparent,
        modifier = modifier,
        topBar = {
            TopAppBar(
                colors = ramdiskTopBarColors(),
                title = { Text(fileName) },
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        YukiIcon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.ramdisk_editor_back),
                        )
                    }
                },
            )
        },
    ) { paddingValues ->
        SelectionContainer {
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                item {
                    Column(modifier = Modifier.padding(bottom = 8.dp)) {
                        Text(
                            text = stringResource(R.string.ramdisk_editor_elf_summary_title),
                            style = MaterialTheme.typography.titleMedium,
                        )
                        Text(
                            text = stringResource(
                                R.string.ramdisk_editor_elf_summary_description
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp),
                        )
                    }
                }
                groups.forEach { (groupTitle, groupFields) ->
                    item(key = "group-$groupTitle") {
                        Text(stringResource(groupTitle), style = MaterialTheme.typography.titleSmall,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(start = 4.dp, top = 16.dp, bottom = 6.dp))
                    }
                    itemsIndexed(
                        items = groupFields,
                        key = { _, field -> field.first },
                    ) { index, (label, value) ->
                        Card(
                            modifier = Modifier.fillMaxWidth(),
                            shape = if (isExpressiveUi) ListItemDefaults.segmentedShapes(index, groupFields.size).shape
                                else MaterialTheme.shapes.small,
                            colors = CardDefaults.cardColors(
                                containerColor = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
                            ),
                        ) {
                            ListItem(
                                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 6.dp),
                                content = {
                                    Text(
                                        text = stringResource(label),
                                        style = MaterialTheme.typography.labelLarge,
                                    )
                                },
                                supportingContent = {
                                    Text(
                                        text = value,
                                        style = MaterialTheme.typography.bodyMedium,
                                        fontFamily = FontFamily.Monospace,
                                    )
                                },
                                colors = ListItemDefaults.colors(
                                    containerColor = androidx.compose.ui.graphics.Color.Transparent
                                ),
                            )
                        }
                    }
                }
            }
        }
    }
}

private fun ElfHeaderInfo.toDisplayFields(resources: Resources): List<Pair<Int, String>> {
    fun bytes(value: ULong): String = resources.getString(
        R.string.ramdisk_editor_elf_value_bytes,
        value.toString(),
    )

    fun offset(value: ULong): String = resources.getString(
        R.string.ramdisk_editor_elf_value_offset,
        "0x${value.toString(16)}",
        value.toString(),
    )

    fun enumValue(name: String?, raw: Int): String =
        if (name == null) {
            resources.getString(
                R.string.ramdisk_editor_elf_value_unknown,
                raw.toString(16),
            )
        } else {
            resources.getString(R.string.ramdisk_editor_elf_value_enum, name, raw)
        }

    fun count(value: Int, extended: Boolean): String =
        if (extended) {
            resources.getString(
                R.string.ramdisk_editor_elf_value_extended,
                value,
            )
        } else {
            value.toString()
        }

    val entryWidth = if (elfClass == ELF_CLASS_32) 8 else 16
    return listOf(
        R.string.ramdisk_editor_elf_field_file_size to bytes(fileSize),
        R.string.ramdisk_editor_elf_field_magic to ident.joinToString(" ") {
            "%02x".format(Locale.US, it.toInt() and 0xff)
        },
        R.string.ramdisk_editor_elf_field_class to enumValue(elfClassName(elfClass), elfClass),
        R.string.ramdisk_editor_elf_field_data to
            enumValue(elfDataEncodingName(dataEncoding), dataEncoding),
        R.string.ramdisk_editor_elf_field_ident_version to
            if (identVersion == ELF_VERSION_CURRENT) {
                resources.getString(
                    R.string.ramdisk_editor_elf_value_current,
                    identVersion.toString(),
                )
            } else {
                identVersion.toString()
            },
        R.string.ramdisk_editor_elf_field_os_abi to enumValue(elfOsAbiName(osAbi), osAbi),
        R.string.ramdisk_editor_elf_field_abi_version to abiVersion.toString(),
        R.string.ramdisk_editor_elf_field_type to enumValue(elfTypeName(type), type),
        R.string.ramdisk_editor_elf_field_machine to enumValue(elfMachineName(machine), machine),
        R.string.ramdisk_editor_elf_field_object_version to
            if (elfVersion == ELF_VERSION_CURRENT.toUInt()) {
                resources.getString(
                    R.string.ramdisk_editor_elf_value_current,
                    elfVersion.toString(),
                )
            } else {
                "0x${elfVersion.toString(16)}"
            },
        R.string.ramdisk_editor_elf_field_entry to
            "0x${entry.toString(16).padStart(entryWidth, '0')}",
        R.string.ramdisk_editor_elf_field_program_offset to offset(programHeaderOffset),
        R.string.ramdisk_editor_elf_field_section_offset to offset(sectionHeaderOffset),
        R.string.ramdisk_editor_elf_field_flags to
            "0x${flags.toString(16).padStart(8, '0')}",
        R.string.ramdisk_editor_elf_field_header_size to bytes(headerSize.toULong()),
        R.string.ramdisk_editor_elf_field_program_entry_size to
            bytes(programHeaderEntrySize.toULong()),
        R.string.ramdisk_editor_elf_field_program_count to count(
            programHeaderCount,
            (headerFlags and ELF_HEADER_EXTENDED_PHNUM) != 0u,
        ),
        R.string.ramdisk_editor_elf_field_section_entry_size to
            bytes(sectionHeaderEntrySize.toULong()),
        R.string.ramdisk_editor_elf_field_section_count to count(
            sectionHeaderCount,
            (headerFlags and ELF_HEADER_EXTENDED_SHNUM) != 0u,
        ),
        R.string.ramdisk_editor_elf_field_section_name_index to count(
            sectionNameIndex,
            (headerFlags and ELF_HEADER_EXTENDED_SHSTRNDX) != 0u,
        ),
    )
}

private fun elfClassName(value: Int): String? = when (value) {
    ELF_CLASS_32 -> "ELF32"
    ELF_CLASS_64 -> "ELF64"
    else -> null
}

private fun elfDataEncodingName(value: Int): String? = when (value) {
    1 -> "2's complement, little endian"
    2 -> "2's complement, big endian"
    else -> null
}

private fun elfTypeName(value: Int): String? = when (value) {
    0 -> "NONE"
    1 -> "REL"
    2 -> "EXEC"
    3 -> "DYN"
    4 -> "CORE"
    else -> null
}

private fun elfOsAbiName(value: Int): String? = when (value) {
    0 -> "UNIX - System V"
    1 -> "HP-UX"
    2 -> "NetBSD"
    3 -> "GNU/Linux"
    6 -> "Solaris"
    7 -> "AIX"
    8 -> "IRIX"
    9 -> "FreeBSD"
    10 -> "Tru64"
    12 -> "OpenBSD"
    64 -> "ARM EABI"
    97 -> "ARM"
    255 -> "Standalone"
    else -> null
}

private fun elfMachineName(value: Int): String? = ELF_MACHINE_NAMES[value]

private suspend fun prepareRamdiskEditor(
    context: Context,
    partitionName: String,
    targetSlot: String?,
): RamdiskEditorLoadState.Ready {
    val directory = File(context.cacheDir, "ramdisk_editor").apply {
        check(isDirectory || mkdirs()) { "Cannot create the ramdisk editor cache directory" }
    }
    val sourceImage = File.createTempFile("source_", ".img", directory)
    val outputImage = File.createTempFile("rebuilt_", ".img", directory)
    val logs = mutableListOf<String>()
    try {
        val backedUp = PartitionManagerHelper.backupPartition(
            context = context,
            partition = partitionName,
            outputPath = sourceImage.absolutePath,
            slot = targetSlot,
            onStdout = logs::add,
            onStderr = { logs.add(it) },
        )
        if (!backedUp || sourceImage.length() <= 0L) {
            throw IOException(
                logs.lastOrNull()
                    ?: context.getString(R.string.ramdisk_editor_partition_backup_failed)
            )
        }
        val image = YrcpRamdiskImage.open(
            ksudPath = getKsud(),
            sourceImage = sourceImage,
            outputImage = outputImage,
            stagingDirectory = directory,
            rootDisplayName = context.getString(
                R.string.ramdisk_editor_root_name,
                partitionName,
            ),
        )
        return RamdiskEditorLoadState.Ready(
            image = image,
            sourceImage = sourceImage,
            outputImage = outputImage,
        )
    } catch (error: Throwable) {
        sourceImage.delete()
        outputImage.delete()
        throw error
    }
}

private fun queryDisplayName(context: Context, uri: Uri): String? =
    context.contentResolver.query(
        uri,
        arrayOf(OpenableColumns.DISPLAY_NAME),
        null,
        null,
        null,
    )?.use { cursor ->
        if (!cursor.moveToFirst()) return@use null
        val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (index < 0) null else cursor.getString(index)?.takeIf(String::isNotBlank)
    }

private fun buildExportFileName(partitionName: String): String {
    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    return "${partitionName}_ramdisk_$timestamp.img"
}

private sealed interface RamdiskEditorLoadState {
    data object Loading : RamdiskEditorLoadState

    data class Ready(
        val image: YrcpRamdiskImage,
        val sourceImage: File,
        val outputImage: File,
    ) : RamdiskEditorLoadState

    data class Error(val message: String) : RamdiskEditorLoadState
}

private class OpenedTextFile(
    val entry: FileEntry,
    sourceBytes: ByteArray,
    decoded: DecodedRamdiskText,
) {
    val state = TextFileEditorState(
        initialText = decoded.text,
        initialEncoding = decoded.encoding,
    )
    var sourceBytes: ByteArray = sourceBytes
        private set
    var sourceEncoding: TextFileEncoding = decoded.encoding
        private set
    var sourceByteOrderMark: UnicodeByteOrderMark? = decoded.byteOrderMark
        private set

    fun updateDecodedSource(decoded: DecodedRamdiskText) {
        state.replaceDocument(
            text = decoded.text,
            encoding = decoded.encoding,
        )
        sourceEncoding = decoded.encoding
        sourceByteOrderMark = decoded.byteOrderMark
    }

    fun updatePersistedSource(
        encoded: EncodedRamdiskText,
        encoding: TextFileEncoding,
    ) {
        sourceBytes = encoded.bytes
        sourceEncoding = encoding
        sourceByteOrderMark = encoded.byteOrderMark
    }
}

private data class PendingTextFile(
    val entry: FileEntry,
    val sourceBytes: ByteArray,
)

private data class OpenedElfFile(
    val entry: FileEntry,
    val header: ElfHeaderInfo,
)

private enum class DiscardPrompt {
    HEX_FILE,
    ARCHIVE,
    TEXT_FILE,
}

private const val ROOT_EXIT_CONFIRM_INTERVAL_MILLIS = 2_500L
private const val ELF_CLASS_32 = 1
private const val ELF_CLASS_64 = 2
private const val ELF_VERSION_CURRENT = 1
private const val ELF_HEADER_EXTENDED_PHNUM = 1u
private const val ELF_HEADER_EXTENDED_SHNUM = 2u
private const val ELF_HEADER_EXTENDED_SHSTRNDX = 4u
private val ELF_MACHINE_NAMES = mapOf(
    2 to "sparc",
    3 to "386",
    4 to "m68k",
    6 to "486",
    8 to "mips",
    15 to "parisc",
    18 to "sparc8+",
    20 to "ppc",
    21 to "ppc64",
    22 to "s390",
    40 to "arm",
    42 to "sh",
    43 to "sparc9",
    50 to "ia64",
    62 to "x86-64",
    88 to "m32r",
    92 to "openrisc",
    93 to "arc",
    94 to "xtensa",
    113 to "nios2",
    135 to "score",
    140 to "c6x",
    164 to "hexagon",
    183 to "arm64",
    188 to "tile",
    189 to "microblaze",
    191 to "tilegx",
    195 to "arcv2",
    243 to "riscv",
    247 to "bpf",
    252 to "csky",
    258 to "loongarch",
)


private class RamdiskBrowserSession(
    val controller: FileBrowserController,
    val presentation: FileBrowserPresentationState,
    val scope: CoroutineScope,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ramdiskTopBarColors() = getTopAppBarColors(
    containerColor = if (CardConfig.isCustomBackgroundEnabled) MaterialTheme.colorScheme.surfaceContainerLow
        else MaterialTheme.colorScheme.background,
)

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RamdiskOperationOverlay(label: String) {
    Dialog(onDismissRequest = {}, properties = DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false)) {
        Surface(shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Row(Modifier.padding(horizontal = 20.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                if (isExpressiveUi) LoadingIndicator(Modifier.size(32.dp))
                else CircularProgressIndicator(Modifier.size(24.dp))
                Text(label, style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

private data class OpenedHexFile(val entry: FileEntry, val controller: HexEditorController)

private enum class RamdiskOpenKind { TEXT, HEX, ELF }
