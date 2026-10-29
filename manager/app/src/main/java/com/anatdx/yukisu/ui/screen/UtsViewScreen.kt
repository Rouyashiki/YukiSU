package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.WarningAmber
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.ui.util.UTS_FIELD_DOMAINNAME
import com.anatdx.yukisu.ui.util.UTS_FIELD_MACHINE
import com.anatdx.yukisu.ui.util.UTS_FIELD_NODENAME
import com.anatdx.yukisu.ui.util.UTS_FIELD_RELEASE
import com.anatdx.yukisu.ui.util.UTS_FIELD_SYSNAME
import com.anatdx.yukisu.ui.util.UTS_FIELD_VERSION
import com.anatdx.yukisu.ui.util.UtsTemplate
import com.anatdx.yukisu.ui.util.UtsViewStatus
import com.anatdx.yukisu.ui.util.LocalNavigationLeaveGuard
import com.anatdx.yukisu.ui.util.getLastPatchedUtsBootConfigurationToken
import com.anatdx.yukisu.ui.util.getOrInitializePatchedUtsBootConfigurationToken
import com.anatdx.yukisu.ui.util.getPendingUtsBootTemplate
import com.anatdx.yukisu.ui.util.getSavedUtsBootDraft
import com.anatdx.yukisu.ui.util.hasPendingUtsBootConfigFile
import com.anatdx.yukisu.ui.util.normalizedForCommit
import com.anatdx.yukisu.ui.util.getUtsViewConfig
import com.anatdx.yukisu.ui.util.getUtsViewEffective
import com.anatdx.yukisu.ui.util.getUtsViewOriginal
import com.anatdx.yukisu.ui.util.getUtsViewStatus
import com.anatdx.yukisu.ui.util.savePendingUtsBootTemplate
import com.anatdx.yukisu.ui.util.saveUtsBootDraft
import com.anatdx.yukisu.ui.util.setUtsViewMode
import com.anatdx.yukisu.ui.util.setUtsViewTemplate
import com.anatdx.yukisu.ui.util.utsBootConfigurationToken
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.InstallScreenDestination
import com.ramcosta.composedestinations.generated.destinations.UtsViewScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.yield

private val utsHorizontalPadding = 16.dp

private data class UtsScreenState(
    val status: UtsViewStatus = UtsViewStatus(),
    val original: UtsTemplate = UtsTemplate(),
    val effective: UtsTemplate = UtsTemplate(),
)

private enum class UtsTemplateTarget {
    BootGlobal,
    RuntimeGlobal,
    DenylistScoped,
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun UtsViewScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val snackbar = rememberSnackbarController()
    val scope = rememberCoroutineScope()
    val bootBaselineReady = remember { CompletableDeferred<Unit>() }
    val focusManager = LocalFocusManager.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val navigationLeaveGuard = LocalNavigationLeaveGuard.current
    val leaveGuardOwner = remember { Any() }
    val globalWriteMutex = remember { Mutex() }
    val denyWriteMutex = remember { Mutex() }
    var globalCommitJob by remember { mutableStateOf<Job?>(null) }
    var denyCommitJob by remember { mutableStateOf<Job?>(null) }
    var globalCommitRevision by remember { mutableLongStateOf(0L) }
    var denyCommitRevision by remember { mutableLongStateOf(0L) }
    var supported by remember { mutableStateOf<Boolean?>(null) }
    var screenState by remember { mutableStateOf(UtsScreenState()) }
    var global by remember { mutableStateOf(UtsTemplate()) }
    var deny by remember { mutableStateOf(UtsTemplate()) }
    val initialPendingBoot = remember { getPendingUtsBootTemplate() }
    val initialSavedBootDraft = remember { getSavedUtsBootDraft() }
    var pendingBoot by remember { mutableStateOf(initialPendingBoot) }
    var pendingBootFileExists by remember { mutableStateOf(hasPendingUtsBootConfigFile()) }
    var hasSavedBootDraft by remember { mutableStateOf(initialSavedBootDraft != null) }
    var bootDraft by remember {
        mutableStateOf(initialSavedBootDraft ?: pendingBoot ?: UtsTemplate())
    }
    var selectedTemplateTarget by remember {
        mutableStateOf(UtsTemplateTarget.BootGlobal)
    }
    var patchedBootConfigurationToken by remember { mutableStateOf<String?>(null) }
    var leavePromptActive by remember { mutableStateOf(false) }
    var leaving by remember { mutableStateOf(false) }
    var hasSnapshot by remember { mutableStateOf(false) }
    var refreshing by remember { mutableStateOf(false) }
    val runtimeEnabledMessage = stringResource(R.string.uts_view_runtime_enabled)
    val runtimeDisabledMessage = stringResource(R.string.uts_view_runtime_disabled)
    val scopedChangedMessage = stringResource(R.string.uts_view_scoped_changed)
    val failedMessage = stringResource(R.string.setting_change_failed)
    val unpatchedBootTitle = stringResource(R.string.uts_view_unpatched_boot_title)
    val unpatchedBootMessage = stringResource(R.string.uts_view_unpatched_boot_message)
    val yesLabel = stringResource(R.string.yes)
    val noLabel = stringResource(R.string.no)

    fun currentBootConfigurationToken(): String = utsBootConfigurationToken(
        enabled = pendingBootFileExists,
        template = bootDraft,
    )

    fun bootNeedsRepatch(): Boolean {
        val patchedToken = patchedBootConfigurationToken ?: return true
        return patchedToken != currentBootConfigurationToken()
    }

    suspend fun refresh() {
        refreshing = true
        try {
            val status = getUtsViewStatus()
            if (status == null) {
                if (!hasSnapshot) {
                    supported = false
                }
                return
            }
            val config = getUtsViewConfig()
            val original = getUtsViewOriginal()
            val effective = getUtsViewEffective()
            if (config == null || original == null || effective == null) {
                if (!hasSnapshot) {
                    supported = false
                }
                return
            }

            val nextScreenState = UtsScreenState(
                status = status,
                original = original,
                effective = effective,
            )
            global = config.first
            deny = config.second
            screenState = nextScreenState
            if (!pendingBootFileExists && !hasSavedBootDraft && bootDraft.mask == 0) {
                bootDraft = config.first
            }
            hasSnapshot = true
            supported = true
        } finally {
            refreshing = false
        }
    }

    suspend fun flushTemplateCommits() {
        do {
            val globalJob = globalCommitJob
            val denyJob = denyCommitJob
            globalJob?.join()
            denyJob?.join()
        } while (globalJob !== globalCommitJob || denyJob !== denyCommitJob)
    }

    fun launchBootRepatch() {
        if (leaving) return
        leaving = true
        focusManager.clearFocus(force = true)
        scope.launch {
            yield()
            flushTemplateCommits()
            val enabled = pendingBootFileExists
            val templateSnapshot = bootDraft.normalizedForCommit()
            val draftSaved = saveUtsBootDraft(templateSnapshot)
            val staged = draftSaved && if (enabled) {
                templateSnapshot.mask != 0 &&
                    savePendingUtsBootTemplate(templateSnapshot)
            } else {
                savePendingUtsBootTemplate(null)
            }
            if (!staged) {
                leaving = false
                snackbar.showSnackbar(failedMessage)
                return@launch
            }

            bootDraft = templateSnapshot
            hasSavedBootDraft = true
            pendingBoot = templateSnapshot.takeIf { enabled }
            pendingBootFileExists = enabled
            navigator.navigate(
                InstallScreenDestination(utsBootRepatch = true)
            )
        }
    }

    val leaveConfirmationDialog = rememberConfirmDialog(
        onConfirm = {
            leavePromptActive = false
            launchBootRepatch()
        },
        onDismiss = {
            leavePromptActive = false
        },
    )

    fun requestLeave(onIntercepted: () -> Unit = {}, navigate: () -> Unit) {
        if (leaving || leavePromptActive) {
            onIntercepted()
            return
        }
        leavePromptActive = true
        focusManager.clearFocus(force = true)
        scope.launch {
            var resolved = false
            try {
                yield()
                flushTemplateCommits()
                bootBaselineReady.await()
                if (bootNeedsRepatch()) {
                    onIntercepted()
                    resolved = true
                    leaveConfirmationDialog.showConfirm(
                        title = unpatchedBootTitle,
                        content = unpatchedBootMessage,
                        confirm = yesLabel,
                        dismiss = noLabel,
                    )
                } else {
                    leavePromptActive = false
                    leaving = true
                    navigate()
                    resolved = true
                }
            } finally {
                if (!resolved) {
                    leavePromptActive = false
                    onIntercepted()
                }
            }
        }
    }

    fun refreshAfterCommit() {
        if (refreshing || leaving) return
        focusManager.clearFocus(force = true)
        scope.launch {
            flushTemplateCommits()
            refresh()
        }
    }

    LaunchedEffect(Unit) {
        try {
            refresh()
            if (saveUtsBootDraft(bootDraft.normalizedForCommit())) {
                hasSavedBootDraft = true
            }
            patchedBootConfigurationToken =
                getOrInitializePatchedUtsBootConfigurationToken(
                    currentToken = currentBootConfigurationToken(),
                )
        } finally {
            bootBaselineReady.complete(Unit)
        }
    }

    LaunchedEffect(supported) {
        if (supported == false) {
            selectedTemplateTarget = UtsTemplateTarget.BootGlobal
        }
    }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (
                event == Lifecycle.Event.ON_START ||
                event == Lifecycle.Event.ON_RESUME
            ) {
                leaving = false
                getLastPatchedUtsBootConfigurationToken()?.let {
                    patchedBootConfigurationToken = it
                    bootBaselineReady.complete(Unit)
                }
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
        }
    }

    val currentLeaveInterceptor =
        rememberUpdatedState<(() -> Unit, () -> Unit) -> Unit> { navigate, onIntercepted ->
            requestLeave(onIntercepted = onIntercepted, navigate = navigate)
        }
    DisposableEffect(navigationLeaveGuard, leaveGuardOwner) {
        navigationLeaveGuard.register(
            owner = leaveGuardOwner,
            route = UtsViewScreenDestination.route,
        ) { navigate, onIntercepted ->
            currentLeaveInterceptor.value(navigate, onIntercepted)
        }
        onDispose {
            navigationLeaveGuard.unregister(leaveGuardOwner)
        }
    }

    Scaffold(
        topBar = {
            UtsViewTopBar(
                onBack = {
                    requestLeave {
                        navigator.navigateUp()
                    }
                },
                onRefresh = ::refreshAfterCommit,
                refreshEnabled = !refreshing,
                scrollBehavior = scrollBehavior,
            )
        },
        snackbarHost = { SnackbarHost(snackbar.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    ) { padding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .nestedScroll(scrollBehavior.nestedScrollConnection),
            contentPadding = PaddingValues(bottom = 24.dp),
        ) {
            if (supported == null || refreshing) {
                item {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(32.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        CircularProgressIndicator()
                    }
                }
            } else {
                if (supported == false) {
                    item {
                        UtsListGroup(title = stringResource(R.string.uts_view_unavailable)) {
                            UtsGroupItem(index = 0, count = 1) {
                                ListItem(
                                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                                    content = {
                                        Text(stringResource(R.string.feature_status_unsupported_summary))
                                    },
                                )
                            }
                        }
                    }
                } else {
                    item {
                        StatusGroup(screenState.status)
                    }
                    item {
                        IdentityGroup(screenState)
                    }
                }
                item {
                    val actionTarget = selectedTemplateTarget
                    val template = when (actionTarget) {
                        UtsTemplateTarget.RuntimeGlobal -> global
                        UtsTemplateTarget.DenylistScoped -> deny
                        UtsTemplateTarget.BootGlobal -> bootDraft
                    }
                    val description = when (actionTarget) {
                        UtsTemplateTarget.RuntimeGlobal -> {
                            if (screenState.status.bootLocked) {
                                stringResource(R.string.uts_view_boot_locked_warning)
                            } else {
                                stringResource(R.string.uts_view_runtime_warning)
                            }
                        }
                        UtsTemplateTarget.DenylistScoped ->
                            stringResource(R.string.uts_view_deny_warning)
                        UtsTemplateTarget.BootGlobal -> stringResource(
                            if (pendingBootFileExists && pendingBoot == null) {
                                R.string.uts_view_boot_invalid_warning
                            } else {
                                R.string.uts_view_boot_warning
                            }
                        )
                    }
                    val editorEnabled = when (actionTarget) {
                        UtsTemplateTarget.RuntimeGlobal ->
                            supported == true && !screenState.status.bootLocked
                        UtsTemplateTarget.DenylistScoped -> supported == true
                        UtsTemplateTarget.BootGlobal ->
                            patchedBootConfigurationToken != null
                    }
                    val active = when (actionTarget) {
                        UtsTemplateTarget.RuntimeGlobal -> screenState.status.globalEnabled
                        UtsTemplateTarget.DenylistScoped -> screenState.status.scopedEnabled
                        UtsTemplateTarget.BootGlobal -> pendingBootFileExists
                    }
                    val updateDraft: (UtsTemplate) -> Unit = { updated ->
                        when (actionTarget) {
                            UtsTemplateTarget.RuntimeGlobal -> global = updated
                            UtsTemplateTarget.DenylistScoped -> deny = updated
                            UtsTemplateTarget.BootGlobal -> bootDraft = updated
                        }
                    }

                    TemplateEditorGroup(
                        selectedTarget = actionTarget,
                        onTargetSelected = { selectedTemplateTarget = it },
                        runtimeTargetsEnabled = supported == true,
                        description = description,
                        template = template,
                        onTemplateChange = updateDraft,
                        enabled = editorEnabled,
                        active = active,
                        onActiveChange = { enable ->
                            val templateSnapshot = template.normalizedForCommit()
                            updateDraft(templateSnapshot)
                            when (actionTarget) {
                                UtsTemplateTarget.RuntimeGlobal -> scope.launch {
                                    val ok = globalWriteMutex.withLock {
                                        val configured = !enable ||
                                            setUtsViewTemplate(true, templateSnapshot)
                                        configured && setUtsViewMode(true, enable)
                                    }
                                    snackbar.showSnackbar(
                                        if (ok) {
                                            if (enable) {
                                                runtimeEnabledMessage
                                            } else {
                                                runtimeDisabledMessage
                                            }
                                        } else {
                                            failedMessage
                                        }
                                    )
                                    refresh()
                                }
                                UtsTemplateTarget.DenylistScoped -> scope.launch {
                                    val ok = denyWriteMutex.withLock {
                                        val configured = !enable ||
                                            setUtsViewTemplate(false, templateSnapshot)
                                        configured && setUtsViewMode(false, enable)
                                    }
                                    snackbar.showSnackbar(
                                        if (ok) scopedChangedMessage else failedMessage
                                    )
                                    refresh()
                                }
                                UtsTemplateTarget.BootGlobal -> {
                                    val draftSaved = saveUtsBootDraft(templateSnapshot)
                                    val ok = draftSaved &&
                                        (!enable || templateSnapshot.mask != 0) &&
                                        savePendingUtsBootTemplate(
                                            if (enable) templateSnapshot else null
                                        )
                                    if (ok) {
                                        hasSavedBootDraft = true
                                        pendingBoot = if (enable) templateSnapshot else null
                                        pendingBootFileExists = enable
                                    } else {
                                        scope.launch {
                                            snackbar.showSnackbar(failedMessage)
                                        }
                                    }
                                }
                            }
                        },
                        onTemplateCommit = { rawTemplate ->
                            val templateSnapshot = rawTemplate.normalizedForCommit()
                            updateDraft(templateSnapshot)
                            when (actionTarget) {
                                UtsTemplateTarget.RuntimeGlobal -> {
                                    globalCommitRevision += 1
                                    val commitRevision = globalCommitRevision
                                    globalCommitJob = scope.launch {
                                        var restoredStatus: UtsViewStatus? = null
                                        var restoredEffective: UtsTemplate? = null
                                        val ok = globalWriteMutex.withLock {
                                            if (commitRevision != globalCommitRevision) {
                                                return@withLock null
                                            }
                                            val saved = setUtsViewTemplate(
                                                true,
                                                templateSnapshot
                                            )
                                            if (saved && templateSnapshot.mask == 0) {
                                                restoredStatus = getUtsViewStatus()
                                                restoredEffective = getUtsViewEffective()
                                            }
                                            saved
                                        }
                                        if (
                                            ok == true &&
                                            templateSnapshot.mask == 0 &&
                                            commitRevision == globalCommitRevision
                                        ) {
                                            val latestStatus = restoredStatus
                                            val latestEffective = restoredEffective
                                            screenState = if (
                                                latestStatus != null &&
                                                latestEffective != null
                                            ) {
                                                screenState.copy(
                                                    status = latestStatus,
                                                    effective = latestEffective,
                                                )
                                            } else {
                                                screenState.copy(
                                                    status = screenState.status.copy(
                                                        source = "none",
                                                        mode = screenState.status.mode and 1L.inv(),
                                                        globalMask = 0,
                                                    )
                                                )
                                            }
                                        }
                                        if (ok == false) {
                                            snackbar.showSnackbar(failedMessage)
                                        }
                                    }
                                }
                                UtsTemplateTarget.DenylistScoped -> {
                                    denyCommitRevision += 1
                                    val commitRevision = denyCommitRevision
                                    denyCommitJob = scope.launch {
                                        val ok = denyWriteMutex.withLock {
                                            if (commitRevision != denyCommitRevision) {
                                                return@withLock null
                                            }
                                            setUtsViewTemplate(false, templateSnapshot)
                                        }
                                        if (ok == true && templateSnapshot.mask == 0) {
                                            screenState = screenState.copy(
                                                status = screenState.status.copy(
                                                    mode = screenState.status.mode and 2L.inv(),
                                                    denyMask = 0,
                                                )
                                            )
                                        }
                                        if (ok == false) {
                                            snackbar.showSnackbar(failedMessage)
                                        }
                                    }
                                }
                                UtsTemplateTarget.BootGlobal -> {
                                    val wasEnabled = pendingBootFileExists
                                    val stagedTemplate = templateSnapshot.takeIf {
                                        wasEnabled && it.mask != 0
                                    }
                                    val draftSaved = saveUtsBootDraft(templateSnapshot)
                                    val ok = draftSaved && (
                                        !wasEnabled ||
                                            savePendingUtsBootTemplate(stagedTemplate)
                                        )
                                    if (ok) {
                                        hasSavedBootDraft = true
                                        pendingBoot = stagedTemplate
                                        pendingBootFileExists =
                                            wasEnabled && stagedTemplate != null
                                    }
                                    if (!ok) {
                                        scope.launch {
                                            snackbar.showSnackbar(failedMessage)
                                        }
                                    }
                                }
                            }
                        },
                        onRepatch = {
                            launchBootRepatch()
                        },
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun UtsViewTopBar(
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    refreshEnabled: Boolean,
    scrollBehavior: TopAppBarScrollBehavior,
) {
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.uts_view_title),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
        }
    }
    val actions: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit = {
        IconButton(onClick = onRefresh, enabled = refreshEnabled) {
            Icon(
                Icons.Filled.Refresh,
                contentDescription = stringResource(R.string.uts_view_refresh),
            )
        }
    }
    val containerColor = if (CardConfig.isCustomBackgroundEnabled) {
        MaterialTheme.colorScheme.surfaceContainerLow
    } else {
        MaterialTheme.colorScheme.background
    }
    val colors = getTopAppBarColors(
        containerColor = containerColor,
        scrolledContainerColor = containerColor,
    )
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            actions = actions,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    } else {
        TopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            actions = actions,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    }
}

@Composable
private fun StatusGroup(status: UtsViewStatus) {
    val warningCount = listOf(status.lateCapture, status.lateGaps).count { it }
    val source = when (status.source) {
        "boot" -> stringResource(R.string.uts_view_source_boot)
        "runtime" -> stringResource(R.string.uts_view_source_runtime)
        else -> stringResource(R.string.uts_view_source_none)
    }
    val mode = when {
        status.globalEnabled && status.scopedEnabled ->
            stringResource(R.string.uts_view_mode_global_scoped)
        status.globalEnabled -> stringResource(R.string.uts_view_mode_global)
        status.scopedEnabled -> stringResource(R.string.uts_view_mode_scoped)
        else -> stringResource(R.string.uts_view_mode_disabled)
    }
    val yes = stringResource(R.string.uts_view_value_yes)
    val no = stringResource(R.string.uts_view_value_no)
    val entries = listOf(
        stringResource(R.string.uts_view_status_source) to source,
        stringResource(R.string.uts_view_status_mode) to mode,
        stringResource(R.string.uts_view_status_boot_locked) to if (status.bootLocked) yes else no,
        stringResource(R.string.uts_view_status_original_valid) to if (status.originalValid) yes else no,
    )

    UtsListGroup(title = stringResource(R.string.uts_view_current_status)) {
        if (isExpressiveUi) {
            UtsGroupItem(index = 0, count = 1) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 18.dp, vertical = 14.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(
                            modifier = Modifier.weight(1f),
                            verticalArrangement = Arrangement.spacedBy(2.dp),
                        ) {
                            Text(
                                text = mode,
                                style = MaterialTheme.typography.titleLarge,
                                fontWeight = FontWeight.Normal,
                            )
                            Text(
                                text = stringResource(R.string.uts_view_status_mode),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Column(
                            horizontalAlignment = Alignment.End,
                            verticalArrangement = Arrangement.spacedBy(2.dp),
                        ) {
                            Text(
                                text = source,
                                style = MaterialTheme.typography.titleMedium,
                                fontWeight = FontWeight.Normal,
                            )
                            Text(
                                text = stringResource(R.string.uts_view_status_source),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        UtsStatusMetric(
                            label = stringResource(R.string.uts_view_status_boot_locked),
                            value = if (status.bootLocked) yes else no,
                            modifier = Modifier.weight(1f),
                        )
                        UtsStatusMetric(
                            label = stringResource(R.string.uts_view_status_original_valid),
                            value = if (status.originalValid) yes else no,
                            modifier = Modifier.weight(1f),
                        )
                    }
                    if (status.lateCapture) {
                        UtsStatusWarning(
                            text = stringResource(R.string.uts_view_late_capture_warning)
                        )
                    }
                    if (status.lateGaps) {
                        UtsStatusWarning(
                            text = stringResource(R.string.uts_view_late_gaps_warning)
                        )
                    }
                }
            }
        } else {
            val itemCount = entries.size + warningCount
            entries.forEachIndexed { index, (label, value) ->
                UtsGroupItem(index = index, count = itemCount) {
                    ListItem(
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        content = { Text(label) },
                        trailingContent = {
                            Text(
                                text = value,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        },
                    )
                }
            }
            var warningIndex = entries.size
            if (status.lateCapture) {
                UtsWarningItem(
                    text = stringResource(R.string.uts_view_late_capture_warning),
                    index = warningIndex++,
                    count = itemCount,
                )
            }
            if (status.lateGaps) {
                UtsWarningItem(
                    text = stringResource(R.string.uts_view_late_gaps_warning),
                    index = warningIndex,
                    count = itemCount,
                )
            }
        }
    }
}

@Composable
private fun UtsStatusWarning(text: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(MaterialTheme.shapes.medium)
            .background(MaterialTheme.colorScheme.errorContainer)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            imageVector = Icons.Filled.WarningAmber,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onErrorContainer,
            modifier = Modifier.padding(end = 10.dp),
        )
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onErrorContainer,
        )
    }
}

@Composable
private fun UtsStatusMetric(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Text(
            text = value,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Normal,
        )
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun IdentityGroup(state: UtsScreenState) {
    UtsListGroup(title = stringResource(R.string.uts_view_identity_group)) {
        IdentityItem(
            title = stringResource(R.string.uts_view_original_identity),
            template = state.original,
            index = 0,
            count = 2,
        )
        IdentityItem(
            title = stringResource(R.string.uts_view_effective_identity),
            template = state.effective,
            index = 1,
            count = 2,
        )
    }
}

@Composable
private fun IdentityItem(
    title: String,
    template: UtsTemplate,
    index: Int,
    count: Int,
) {
    val emptyValue = stringResource(R.string.uts_view_value_empty)
    UtsGroupItem(index = index, count = count) {
        SelectionContainer {
            ListItem(
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                content = { Text(title) },
                supportingContent = {
                    Column(
                        modifier = Modifier.padding(top = 6.dp),
                        verticalArrangement = Arrangement.spacedBy(3.dp),
                    ) {
                        utsEditorFields.forEach { field ->
                            val value = field.value(template).ifEmpty { emptyValue }
                            Text(
                                text = "${stringResource(field.label)} · $value",
                                style = MaterialTheme.typography.bodyMedium,
                            )
                        }
                    }
                },
            )
        }
    }
}

@Composable
private fun TemplateEditorGroup(
    selectedTarget: UtsTemplateTarget,
    onTargetSelected: (UtsTemplateTarget) -> Unit,
    runtimeTargetsEnabled: Boolean,
    description: String,
    template: UtsTemplate,
    onTemplateChange: (UtsTemplate) -> Unit,
    enabled: Boolean,
    active: Boolean,
    onActiveChange: (Boolean) -> Unit,
    onTemplateCommit: (UtsTemplate) -> Unit,
    onRepatch: () -> Unit,
) {
    val focusManager = LocalFocusManager.current
    val itemCount = 1 + utsEditorFields.size +
        if (selectedTarget == UtsTemplateTarget.BootGlobal) 1 else 0
    UtsListGroup(title = stringResource(R.string.uts_view_configuration_group)) {
        UtsTemplateTargetSelector(
            selectedTarget = selectedTarget,
            runtimeTargetsEnabled = runtimeTargetsEnabled,
            onTargetSelected = { target ->
                focusManager.clearFocus(force = true)
                val normalized = template.normalizedForCommit()
                if (normalized != template) {
                    onTemplateChange(normalized)
                    onTemplateCommit(normalized)
                }
                onTargetSelected(target)
            },
        )
        UtsGroupItem(index = 0, count = itemCount) {
            ListItem(
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                content = { Text(stringResource(R.string.uts_view_enable)) },
                supportingContent = { Text(description) },
                trailingContent = {
                    YukiSwitch(
                        checked = active,
                        onCheckedChange = { checked ->
                            focusManager.clearFocus(force = true)
                            onActiveChange(checked)
                        },
                        enabled = enabled,
                    )
                },
            )
        }
        key(selectedTarget) {
            utsEditorFields.forEachIndexed { fieldIndex, field ->
                UtsFieldEditorItem(
                    field = field,
                    template = template,
                    onTemplateChange = onTemplateChange,
                    onTemplateCommit = onTemplateCommit,
                    enabled = enabled,
                    index = fieldIndex + 1,
                    count = itemCount,
                )
            }
            if (selectedTarget == UtsTemplateTarget.BootGlobal) {
                UtsGroupItem(
                    index = itemCount - 1,
                    count = itemCount,
                ) {
                    Button(
                        onClick = {
                            focusManager.clearFocus(force = true)
                            onRepatch()
                        },
                        enabled = enabled &&
                            (!active || template.normalizedForCommit().mask != 0),
                        modifier = Modifier
                            .padding(12.dp)
                            .fillMaxWidth(),
                    ) {
                        Text(stringResource(R.string.uts_view_repatch))
                    }
                }
            }
        }
    }
}

@Composable
private fun UtsTemplateTargetSelector(
    selectedTarget: UtsTemplateTarget,
    runtimeTargetsEnabled: Boolean,
    onTargetSelected: (UtsTemplateTarget) -> Unit,
) {
    val targets = listOf(
        UtsTemplateTarget.BootGlobal,
        UtsTemplateTarget.RuntimeGlobal,
        UtsTemplateTarget.DenylistScoped,
    )
    SingleChoiceSegmentedButtonRow(
        modifier = Modifier
            .fillMaxWidth()
            .padding(
                horizontal = if (isExpressiveUi) 6.dp else 12.dp,
                vertical = 4.dp,
            ),
    ) {
        targets.forEachIndexed { index, target ->
            val label = when (target) {
                UtsTemplateTarget.RuntimeGlobal ->
                    stringResource(R.string.uts_view_runtime_global)
                UtsTemplateTarget.DenylistScoped ->
                    stringResource(R.string.uts_view_deny_scoped)
                UtsTemplateTarget.BootGlobal ->
                    stringResource(R.string.uts_view_boot_global)
            }
            SegmentedButton(
                selected = selectedTarget == target,
                onClick = { onTargetSelected(target) },
                enabled = runtimeTargetsEnabled || target == UtsTemplateTarget.BootGlobal,
                shape = SegmentedButtonDefaults.itemShape(index, targets.size),
                icon = {},
                modifier = Modifier.weight(1f),
            ) {
                Text(
                    text = label,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = MaterialTheme.typography.labelLarge,
                )
            }
        }
    }
}

private data class UtsField(
    val bit: Int,
    val label: Int,
    val value: (UtsTemplate) -> String,
    val update: (UtsTemplate, String) -> UtsTemplate,
)

private val utsEditorFields = listOf(
    UtsField(UTS_FIELD_SYSNAME, R.string.uts_field_sysname, { it.sysname }) { value, text ->
        value.copy(sysname = text)
    },
    UtsField(UTS_FIELD_NODENAME, R.string.uts_field_nodename, { it.nodename }) { value, text ->
        value.copy(nodename = text)
    },
    UtsField(UTS_FIELD_RELEASE, R.string.uts_field_release, { it.release }) { value, text ->
        value.copy(release = text)
    },
    UtsField(UTS_FIELD_VERSION, R.string.uts_field_version, { it.version }) { value, text ->
        value.copy(version = text)
    },
    UtsField(UTS_FIELD_MACHINE, R.string.uts_field_machine, { it.machine }) { value, text ->
        value.copy(machine = text)
    },
    UtsField(UTS_FIELD_DOMAINNAME, R.string.uts_field_domainname, { it.domainname }) { value, text ->
        value.copy(domainname = text)
    },
)

@Composable
private fun UtsFieldEditorItem(
    field: UtsField,
    template: UtsTemplate,
    onTemplateChange: (UtsTemplate) -> Unit,
    onTemplateCommit: (UtsTemplate) -> Unit,
    enabled: Boolean,
    index: Int,
    count: Int,
) {
    val selected = template.mask and field.bit != 0
    val value = field.value(template)
    var fieldFocused by remember(field.bit) { mutableStateOf(false) }
    var fieldDirty by remember(field.bit) { mutableStateOf(false) }
    var latestEditedTemplate by remember(field.bit) { mutableStateOf(template) }
    var requestInputFocus by remember(field.bit) { mutableStateOf(false) }
    val focusRequester = remember(field.bit) { FocusRequester() }
    val focusManager = LocalFocusManager.current
    LaunchedEffect(template) {
        latestEditedTemplate = template
    }
    LaunchedEffect(selected, requestInputFocus) {
        if (selected && requestInputFocus) {
            focusRequester.requestFocus()
            requestInputFocus = false
        }
    }
    UtsGroupItem(index = index, count = count) {
        Column(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = stringResource(field.label),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                    modifier = Modifier.weight(1f).padding(end = 12.dp),
                )
                YukiSwitch(
                    checked = selected,
                    onCheckedChange = { checked ->
                        focusManager.clearFocus(force = true)
                        val base = template.normalizedForCommit()
                        val mask = if (checked) {
                            base.mask or field.bit
                        } else {
                            base.mask and field.bit.inv()
                        }
                        val updated = base.copy(mask = mask)
                        requestInputFocus =
                            checked && field.value(updated).isEmpty()
                        onTemplateChange(updated)
                        if (!checked || field.value(updated).isNotEmpty()) {
                            onTemplateCommit(updated)
                        }
                    },
                    enabled = enabled,
                )
            }
            YukiExpandableContent(
                visible = selected,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column {
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = value,
                        onValueChange = { text ->
                            val updated = field.update(template, text)
                            latestEditedTemplate = updated
                            fieldDirty = true
                            onTemplateChange(updated)
                        },
                        enabled = enabled,
                        singleLine = true,
                        shape = if (isExpressiveUi) {
                            CircleShape
                        } else {
                            OutlinedTextFieldDefaults.shape
                        },
                        supportingText = {
                            Text(
                                stringResource(
                                    R.string.uts_field_byte_count,
                                    value.toByteArray(Charsets.UTF_8).size,
                                )
                            )
                        },
                        isError = value.toByteArray(Charsets.UTF_8).size > 64,
                        modifier = Modifier
                            .fillMaxWidth()
                            .focusRequester(focusRequester)
                            .onFocusChanged { focusState ->
                                val lostFocus = fieldFocused && !focusState.isFocused
                                if (!fieldFocused && focusState.isFocused) {
                                    latestEditedTemplate = template
                                    fieldDirty = false
                                }
                                fieldFocused = focusState.isFocused
                                if (lostFocus) {
                                    val normalized =
                                        latestEditedTemplate.normalizedForCommit()
                                    val shouldCommit =
                                        fieldDirty || normalized != latestEditedTemplate
                                    fieldDirty = false
                                    if (shouldCommit) {
                                        onTemplateChange(normalized)
                                        onTemplateCommit(normalized)
                                    }
                                }
                            },
                    )
                }
            }
        }
    }
}

@Composable
private fun UtsListGroup(
    title: String,
    content: @Composable ColumnScope.() -> Unit,
) {
    if (isExpressiveUi) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = utsHorizontalPadding, vertical = 12.dp),
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp),
            )
            Column(content = content)
        }
    } else {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = utsHorizontalPadding, vertical = 8.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation(),
        ) {
            Column(modifier = Modifier.padding(vertical = 8.dp)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
                content()
            }
        }
    }
}

@Composable
private fun UtsGroupItem(
    index: Int,
    count: Int,
    containerColor: Color = MaterialTheme.colorScheme.surfaceContainer,
    content: @Composable () -> Unit,
) {
    if (isExpressiveUi) {
        val shape = if (count == 1) {
            MaterialTheme.shapes.large
        } else {
            ListItemDefaults.segmentedShapes(index, count).shape
        }
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(
                    horizontal = 6.dp,
                    vertical = ListItemDefaults.SegmentedGap / 2,
                )
                .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                .clip(shape)
                .background(containerColor.copy(alpha = cardAlpha)),
        ) {
            content()
        }
    } else {
        Column(modifier = Modifier.fillMaxWidth()) {
            if (index > 0) {
                HorizontalDivider(modifier = Modifier.padding(horizontal = 16.dp))
            }
            content()
        }
    }
}

@Composable
private fun UtsWarningItem(
    text: String,
    index: Int,
    count: Int,
) {
    UtsGroupItem(
        index = index,
        count = count,
        containerColor = MaterialTheme.colorScheme.errorContainer,
    ) {
        ListItem(
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            leadingContent = {
                Icon(
                    imageVector = Icons.Filled.WarningAmber,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onErrorContainer,
                )
            },
            content = {
                Text(
                    text = text,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                )
            },
        )
    }
}
