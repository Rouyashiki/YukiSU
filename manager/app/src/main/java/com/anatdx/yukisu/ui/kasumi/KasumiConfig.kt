package com.anatdx.yukisu.ui.kasumi

import android.annotation.SuppressLint
import android.content.Context
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.anatdx.yukisu.ui.util.SnackbarController
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.yukiAnimateContentSize
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.kasumi.util.KasumiUiAdapter as KasumiManager
import com.anatdx.yukisu.ui.kasumi.util.KasumiUiAdapter.KasumiStatus
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SwitchSettingItem


enum class KasumiTab(displayNameRes: Int) {
    STATUS(R.string.kasumi_tab_status),
    SETTINGS(R.string.kasumi_tab_settings),
    RULES(R.string.kasumi_tab_rules),
    LOGS(R.string.kasumi_tab_logs);

    val displayNameRes: Int = displayNameRes
}


@SuppressLint("SdCardPath")
@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Destination<RootGraph>
@Composable
fun KasumiConfigScreen(
    navigator: DestinationsNavigator
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val coroutineScope = rememberCoroutineScope()
    val snackbarHostState = rememberSnackbarController()

    var selectedTab by remember { mutableStateOf(KasumiTab.STATUS) }
    var hideDraftPath by rememberSaveable { mutableStateOf("") }
    var hideDraftBypass by rememberSaveable { mutableStateOf(false) }
    var isLoading by remember { mutableStateOf(true) }
    var configSaving by remember { mutableStateOf(false) }
    var dataReady by remember { mutableStateOf(false) }
    var runtimeApplyPending by remember { mutableStateOf(false) }

    var kasumiStatus by remember { mutableStateOf(KasumiStatus.NOT_PRESENT) }
    var version by remember { mutableStateOf("Unknown") }
    var config by remember { mutableStateOf(KasumiManager.KagamiConfig()) }
    var modules by remember { mutableStateOf(emptyList<KasumiManager.ModuleInfo>()) }
    var activeRules by remember { mutableStateOf(emptyList<KasumiManager.ActiveRule>()) }
    var systemInfo by remember { mutableStateOf(KasumiManager.SystemInfo("", "", emptyList(), emptyList(), false, null)) }
    var storageInfo by remember { mutableStateOf(KasumiManager.StorageInfo("-", "-", "-", "0%", "unknown")) }
    var features by remember { mutableStateOf<KasumiManager.FeaturesResult?>(null) }
    var logContent by remember { mutableStateOf("") }
    var showKernelLog by remember { mutableStateOf(false) }

    fun publish(loaded: KasumiManager.UiState) {
        version = loaded.version
        kasumiStatus = loaded.status
        config = loaded.config
        modules = loaded.modules
        systemInfo = loaded.system
        storageInfo = loaded.storage
        activeRules = loaded.rules
        features = loaded.features
        dataReady = true
    }

    fun loadData() {
        coroutineScope.launchUi(snackbarHostState) {
            isLoading = true
            try {
                val loaded = KasumiManager.load()
                publish(loaded)
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                val msg = resources.getString(
                    R.string.kasumi_toast_load_error,
                    e.message ?: "unknown"
                )
                snackbarHostState.showSnackbar(msg)
            }
            isLoading = false
        }
    }

    LaunchedEffect(Unit) {
        loadData()
    }

    Scaffold(
        topBar = {
            KasumiTopBar(
                onBack = { navigator.popBackStack() },
                onRefresh = { loadData() },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState.hostState) }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {

            KasumiTabs(selectedTab) { selectedTab = it }

            if (isLoading) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    if (isExpressiveUi) LoadingIndicator() else CircularProgressIndicator()
                }
            } else {
                when (selectedTab) {
                    KasumiTab.STATUS -> StatusTab(
                        kasumiStatus = kasumiStatus,
                        kasumiBuiltin = true,
                        version = version,
                        systemInfo = systemInfo,
                        storageInfo = storageInfo,
                        modules = modules,
                        onRefresh = { loadData() }
                    )
                    KasumiTab.SETTINGS -> SettingsTab(
                        config = config,
                        kasumiStatus = kasumiStatus,
                        features = features,
                        snackbarHostState = snackbarHostState,
                        controlsEnabled = dataReady && !configSaving,
                        runtimeApplyPending = runtimeApplyPending,
                        hideDraftPath = hideDraftPath,
                        hideDraftBypass = hideDraftBypass,
                        onHideDraftPathChange = { hideDraftPath = it },
                        onHideDraftBypassChange = { hideDraftBypass = it },
                        onRetryApply = {
                            if (!configSaving) {
                                configSaving = true
                                coroutineScope.launchUi(snackbarHostState) {
                                    try {
                                        runtimeApplyPending = !KasumiManager.retryApply()
                                        publish(KasumiManager.load())
                                    } finally { configSaving = false }
                                    if (runtimeApplyPending) snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_apply_failed))
                                }
                            }
                        },
                        onConfigChanged = onChange@{ newConfig ->
                            if (configSaving) return@onChange
                            configSaving = true
                            coroutineScope.launchUi(snackbarHostState) {
                                val message = try {
                                    val result = KasumiManager.saveConfig(newConfig)
                                    if (result.noChanges) return@launchUi
                                    if (result.persisted) runtimeApplyPending = result.error != null && newConfig.kernelAvailable
                                    publish(KasumiManager.load())
                                    when {
                                        result.error != null && result.persisted -> resources.getString(R.string.kasumi_saved_apply_failed, result.error)
                                        result.error != null -> result.error
                                        result.applied -> resources.getString(R.string.kasumi_saved_applied)
                                        else -> resources.getString(R.string.kasumi_saved_reboot)
                                    }
                                } finally { configSaving = false }
                                snackbarHostState.showSnackbar(message)
                            }
                        },
                    )
                    KasumiTab.RULES -> RulesTab(
                        activeRules = activeRules,
                        kasumiStatus = kasumiStatus,
                        onRefresh = {
                            coroutineScope.launchUi(snackbarHostState) {
                                activeRules = KasumiManager.getActiveRules()
                            }
                        },
                        onClearAll = {
                            coroutineScope.launchUi(snackbarHostState) {
                                if (KasumiManager.clearAllRules()) {
                                    publish(KasumiManager.load())
                                    runtimeApplyPending = config.kasumiEnabled && systemInfo.viewsEnabled == false
                                    snackbarHostState.showSnackbar("All rules cleared")
                                } else {
                                    snackbarHostState.showSnackbar("Failed to clear rules")
                                }
                            }
                        }
                    )
                    KasumiTab.LOGS -> LogsTab(
                        showKernelLog = showKernelLog,
                        onToggleLogType = { showKernelLog = !showKernelLog },
                        logContent = logContent,
                        onClearLog = {
                            coroutineScope.launchUi(snackbarHostState) {
                                if (KasumiManager.clearLog()) logContent = KasumiManager.readLog()
                                else snackbarHostState.showSnackbar(resources.getString(R.string.operation_failed))
                            }
                        },
                        onRefreshLog = {
                            coroutineScope.launchUi(snackbarHostState) {
                                logContent = if (showKernelLog) {
                                    KasumiManager.readKernelLog()
                                } else {
                                    KasumiManager.readLog()
                                }
                            }
                        }
                    )
                }
            }
        }
    }
}

@Composable
internal fun StatusTab(
    kasumiStatus: KasumiStatus,
    kasumiBuiltin: Boolean,
    version: String,
    systemInfo: KasumiManager.SystemInfo,
    storageInfo: KasumiManager.StorageInfo,
    modules: List<KasumiManager.ModuleInfo>,
    onRefresh: () -> Unit
) {
    val mountBaseText = systemInfo.mountBase.ifBlank { stringResource(R.string.kasumi_mount_base_none) }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {

        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = kasumiCardShape(),
            colors = CardDefaults.cardColors(
                containerColor = when (kasumiStatus) {
                    KasumiStatus.AVAILABLE -> if (isExpressiveUi) MaterialTheme.colorScheme.primaryContainer else Color(0xFF1B5E20).copy(alpha = 0.2f)
                    KasumiStatus.NOT_PRESENT -> MaterialTheme.colorScheme.surfaceVariant
                    else -> if (isExpressiveUi) MaterialTheme.colorScheme.errorContainer else Color(0xFFE65100).copy(alpha = 0.2f)
                }
            )
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    YukiIcon(
                        imageVector = when (kasumiStatus) {
                            KasumiStatus.AVAILABLE -> Icons.Filled.CheckCircle
                            KasumiStatus.NOT_PRESENT -> Icons.Filled.Info
                            else -> Icons.Filled.Warning
                        },
                        contentDescription = null,
                        tint = when (kasumiStatus) {
                            KasumiStatus.AVAILABLE -> if (isExpressiveUi) MaterialTheme.colorScheme.onPrimaryContainer else Color(0xFF4CAF50)
                            KasumiStatus.NOT_PRESENT -> MaterialTheme.colorScheme.onSurfaceVariant
                            else -> if (isExpressiveUi) MaterialTheme.colorScheme.onErrorContainer else Color(0xFFFF9800)
                        },
                        modifier = Modifier.size(if (isExpressiveUi) 40.dp else 32.dp)
                    )
                    Column {
                        Text(
                            text = stringResource(R.string.kasumi_kernel_title),
                            style = if (isExpressiveUi) MaterialTheme.typography.titleLarge else MaterialTheme.typography.titleMedium,
                            fontWeight = if (isExpressiveUi) FontWeight.Normal else FontWeight.Bold
                        )
                        Text(
                            text = stringResource(
                                when {
                                    kasumiStatus == KasumiStatus.AVAILABLE && kasumiBuiltin ->
                                        R.string.kasumi_status_builtin
                                    kasumiStatus == KasumiStatus.AVAILABLE ->
                                        R.string.kasumi_status_available
                                    kasumiStatus == KasumiStatus.NOT_PRESENT ->
                                        R.string.kasumi_status_not_present
                                    kasumiStatus == KasumiStatus.KERNEL_TOO_OLD ->
                                        R.string.kasumi_status_kernel_too_old
                                    else -> R.string.kasumi_status_module_too_old
                                }
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }

                if (kasumiStatus == KasumiStatus.AVAILABLE) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = stringResource(R.string.kasumi_version_label, version),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = kasumiCardShape(),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = stringResource(R.string.kasumi_storage),
                        style = MaterialTheme.typography.titleMedium
                    )
                    if (storageInfo.type != "unknown") {
                        Surface(
                            shape = if (isExpressiveUi) MaterialTheme.shapes.small else RoundedCornerShape(4.dp),
                            color = if (storageInfo.type == "tmpfs" || storageInfo.type == "kasumi")
                                MaterialTheme.colorScheme.primaryContainer
                            else
                                MaterialTheme.colorScheme.secondaryContainer
                        ) {
                            Text(
                                text = storageInfo.type.uppercase(),
                                modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.Bold
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.height(12.dp))

                LinearProgressIndicator(
                    progress = { storageInfo.percent.removeSuffix("%").toFloatOrNull()?.div(100) ?: 0f },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(8.dp),
                    trackColor = MaterialTheme.colorScheme.surfaceVariant
                )

                Spacer(modifier = Modifier.height(8.dp))

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = mountBaseText,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = "${storageInfo.used} / ${storageInfo.size}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            StatCard(
                modifier = Modifier.weight(1f),
                value = modules.size.toString(),
                label = stringResource(R.string.kasumi_modules_count)
            )
            StatCard(
                modifier = Modifier.weight(1f),
                value = if (kasumiStatus == KasumiStatus.AVAILABLE)
                    systemInfo.kasumiModuleIds.size.toString()
                else "❌",
                label = stringResource(R.string.kasumi_stats_kasumi)
            )
        }

        ConfigSection(stringResource(R.string.kasumi_system_info)) {

            InfoRow(label = stringResource(R.string.kasumi_info_kernel), value = systemInfo.kernel)
            InfoRow(label = stringResource(R.string.kasumi_info_mount_base), value = mountBaseText)
            InfoRow(label = stringResource(R.string.kasumi_runtime_views), value = when (systemInfo.viewsEnabled) {
                true -> stringResource(R.string.kasumi_views_on)
                false -> stringResource(R.string.kasumi_views_off)
                null -> "—"
            })

            if (systemInfo.activeMounts.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.kasumi_info_active_mounts),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                systemInfo.activeMounts.take(5).forEach { mount ->
                    Text(
                        text = "  • $mount",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                }
                if (systemInfo.activeMounts.size > 5) {
                    Text(
                        text = pluralStringResource(R.plurals.kasumi_info_more_mounts, systemInfo.activeMounts.size - 5, systemInfo.activeMounts.size - 5),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        KernelHooksCard(systemInfo.hooks)
        systemInfo.mountStats?.let { ms ->
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = kasumiCardShape(),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    StatCard(Modifier.weight(1f), ms.totalMounts.toString(), stringResource(R.string.kasumi_mount_total))
                    StatCard(Modifier.weight(1f), ms.overlayfsMounts.toString(), "OverlayFS")
                }
            }
        }

        if (systemInfo.kasumiMismatch) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = kasumiCardShape(),
                colors = CardDefaults.cardColors(
                    containerColor = if (isExpressiveUi) MaterialTheme.colorScheme.errorContainer else Color(0xFFE65100).copy(alpha = 0.2f)
                )
            ) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    YukiIcon(
                        Icons.Filled.Warning,
                        contentDescription = null,
                        tint = if (isExpressiveUi) MaterialTheme.colorScheme.onErrorContainer else Color(0xFFFF9800)
                    )
                    Text(
                        text = systemInfo.mismatchMessage ?: stringResource(R.string.kasumi_mismatch_default),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
        }
    }
}

@Composable
private fun StatCard(
    modifier: Modifier = Modifier,
    value: String,
    label: String
) {
    Card(
        modifier = modifier,
        shape = kasumiCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh),
        elevation = getCardElevation()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = value,
                style = if (isExpressiveUi) MaterialTheme.typography.displaySmall else MaterialTheme.typography.headlineMedium,
                color = if (isExpressiveUi) MaterialTheme.colorScheme.primary else Color.Unspecified,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else FontWeight.Bold
            )
            Text(
                text = label,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    if (isExpressiveUi) {
        Column(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(value, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
        }
        return
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
internal fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
    icon: ImageVector = Icons.Filled.Settings,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
) {
    if (isExpressiveUi) {
        SwitchSettingItem(icon = icon, title = title, summary = subtitle, checked = checked,
            enabled = enabled, groupPosition = position, onChange = onCheckedChange)
        return
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f).padding(end = 12.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge,
                color = if (enabled) Color.Unspecified else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
            )
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f)
            )
        }
        YukiSwitch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled
        )
    }
}

@Composable
internal fun SettingTextField(
    title: String,
    subtitle: String,
    value: String,
    onValueChange: (String) -> Unit,
    onConfirm: () -> Unit,
    enabled: Boolean = true,
    placeholder: String = "",
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
) {
    var isEditing by remember { mutableStateOf(false) }
    val motion = LocalYukiMotion.current
    KasumiControlGroup(position) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 4.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = title,
                        style = if (isExpressiveUi) MaterialTheme.typography.titleMedium else MaterialTheme.typography.bodyLarge,
                        color = if (enabled) Color.Unspecified else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                    )
                    Text(
                        text = subtitle,
                        style = if (isExpressiveUi) MaterialTheme.typography.bodyMedium else MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f)
                    )
                }
                IconButton(
                    onClick = { isEditing = !isEditing },
                    enabled = enabled
                ) {
                    YukiIcon(
                        imageVector = if (isEditing) Icons.Filled.Check else Icons.Filled.Edit,
                        contentDescription = null
                    )
                }
            }

            AnimatedContent(
                targetState = isEditing,
                modifier = Modifier.yukiAnimateContentSize(),
                transitionSpec = {
                    (fadeIn(motion.effects()) togetherWith fadeOut(motion.effects()))
                        .using(null)
                },
                label = "kasumiPathEditor",
            ) { editing ->
                Box(Modifier.then(if (editing != isEditing) Modifier.clearAndSetSemantics {} else Modifier)) {
                    if (editing) {
                        OutlinedTextField(
                            value = value,
                            onValueChange = onValueChange,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 8.dp),
                            placeholder = { if (placeholder.isNotEmpty()) Text(placeholder) },
                            singleLine = true,
                            enabled = enabled && isEditing,
                            trailingIcon = {
                                IconButton(enabled = enabled && isEditing, onClick = {
                                    onConfirm()
                                    isEditing = false
                                }) {
                                    YukiIcon(Icons.Filled.Done, contentDescription = null)
                                }
                            }
                        )
                    } else if (value.isNotEmpty()) {
                        Text(
                            text = value,
                            modifier = Modifier.padding(top = 4.dp),
                            style = MaterialTheme.typography.bodyMedium,
                            fontFamily = FontFamily.Monospace,
                            color = MaterialTheme.colorScheme.primary
                        )
                    }
                }
            }
        }
    }
}

@Composable
internal fun MapsSpoofCard(
    snackbarHostState: SnackbarController
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val coroutineScope = rememberCoroutineScope()
    var showClearConfirm by remember { mutableStateOf(false) }
    var showAddDialog by remember { mutableStateOf(false) }

    if (showClearConfirm) {
        YukiAlertDialog(
            onDismissRequest = { showClearConfirm = false },
            title = { Text(stringResource(R.string.kasumi_maps_clear)) },
            text = { Text(stringResource(R.string.kasumi_maps_clear_confirm)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearConfirm = false
                        coroutineScope.launchUi(snackbarHostState) {
                            if (KasumiManager.clearMapsRules()) {
                                snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_maps_cleared))
                            } else {
                                snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_maps_clear_failed))
                            }
                        }
                    }
                ) {
                    Text(stringResource(R.string.kasumi_rules_clear), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearConfirm = false }) {
                    Text(stringResource(R.string.kasumi_rules_cancel))
                }
            }
        )
    }

    if (showAddDialog) {
        var tIno by remember { mutableStateOf("") }
        var tDev by remember { mutableStateOf("0") }
        var sIno by remember { mutableStateOf("") }
        var sDev by remember { mutableStateOf("0") }
        var path by remember { mutableStateOf("") }
        YukiAlertDialog(
            onDismissRequest = { showAddDialog = false },
            title = { Text(stringResource(R.string.kasumi_maps_add_rule)) },
            text = {
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    OutlinedTextField(
                        value = tIno,
                        onValueChange = { tIno = it },
                        label = { Text(stringResource(R.string.kasumi_maps_target_ino)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = tDev,
                        onValueChange = { tDev = it },
                        label = { Text(stringResource(R.string.kasumi_maps_target_dev)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = sIno,
                        onValueChange = { sIno = it },
                        label = { Text(stringResource(R.string.kasumi_maps_spoof_ino)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = sDev,
                        onValueChange = { sDev = it },
                        label = { Text(stringResource(R.string.kasumi_maps_spoof_dev)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = path,
                        onValueChange = { path = it },
                        label = { Text(stringResource(R.string.kasumi_maps_spoof_path)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val ti = tIno.trim().toLongOrNull()
                        val td = tDev.trim().toLongOrNull() ?: 0L
                        val si = sIno.trim().toLongOrNull()
                        val sd = sDev.trim().toLongOrNull() ?: 0L
                        val p = path.trim()
                        if (ti != null && si != null && p.isNotEmpty()) {
                            showAddDialog = false
                            coroutineScope.launchUi(snackbarHostState) {
                                if (KasumiManager.addMapsRule(ti, td, si, sd, p)) {
                                    snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_maps_add_success))
                                } else {
                                    snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_maps_add_failed))
                                }
                            }
                        }
                    }
                ) {
                    Text(stringResource(R.string.kasumi_maps_add_action))
                }
            },
            dismissButton = {
                TextButton(onClick = { showAddDialog = false }) {
                    Text(stringResource(R.string.kasumi_rules_cancel))
                }
            }
        )
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = kasumiCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation()
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = stringResource(R.string.kasumi_maps_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(bottom = 4.dp)
            )
            Text(
                text = stringResource(R.string.kasumi_maps_desc),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 12.dp)
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = { showClearConfirm = true },
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.weight(1f)
                ) {
                    YukiIcon(Icons.Filled.Delete, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(stringResource(R.string.kasumi_maps_clear_action))
                }
                FilledTonalButton(
                    onClick = { showAddDialog = true },
                    modifier = Modifier.weight(1f)
                ) {
                    YukiIcon(Icons.Filled.Add, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(stringResource(R.string.kasumi_maps_add_action))
                }
            }
        }
    }
}

@Composable
internal fun RulesTab(
    activeRules: List<KasumiManager.ActiveRule>,
    kasumiStatus: KasumiStatus,
    onRefresh: () -> Unit,
    onClearAll: () -> Unit
) {
    var showClearDialog by remember { mutableStateOf(false) }

    if (showClearDialog) {
        YukiAlertDialog(
            onDismissRequest = { showClearDialog = false },
            title = { Text(stringResource(R.string.kasumi_rules_clear_all)) },
            text = { Text(stringResource(R.string.kasumi_rules_clear_confirm)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearDialog = false
                        onClearAll()
                    }
                ) {
                    Text(stringResource(R.string.kasumi_rules_clear), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearDialog = false }) {
                    Text(stringResource(R.string.kasumi_rules_cancel))
                }
            }
        )
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        if (kasumiStatus != KasumiStatus.AVAILABLE) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = kasumiCardShape(),
                colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                elevation = getCardElevation()
            ) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    YukiIcon(Icons.Filled.Info, contentDescription = null)
                    Text(stringResource(R.string.kasumi_rules_not_available))
                }
            }
            return
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            OutlinedButton(
                onClick = onRefresh,
                modifier = Modifier.weight(1f)
            ) {
                YukiIcon(Icons.Filled.Refresh, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.kasumi_rules_refresh))
            }

            OutlinedButton(
                onClick = { showClearDialog = true },
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.outlinedButtonColors(
                    contentColor = MaterialTheme.colorScheme.error
                )
            ) {
                YukiIcon(Icons.Filled.Delete, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.kasumi_rules_clear_all))
            }
        }

        Text(
            text = pluralStringResource(R.plurals.kasumi_rules_count, activeRules.size, activeRules.size),
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(bottom = 8.dp)
        )

        LazyColumn(
            verticalArrangement = Arrangement.spacedBy(if (isExpressiveUi) ListItemDefaults.SegmentedGap else 8.dp)
        ) {
            itemsIndexed(activeRules) { index, rule ->
                RuleItem(rule, index, activeRules.size)
            }

            if (activeRules.isEmpty()) {
                item {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(32.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = stringResource(R.string.kasumi_rules_empty),
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun RuleItem(rule: KasumiManager.ActiveRule, index: Int = 0, count: Int = 1) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = if (isExpressiveUi) ListItemDefaults.segmentedShapes(index, count).shape else RoundedCornerShape(8.dp),
        colors = getCardColors(if (isExpressiveUi) MaterialTheme.colorScheme.surfaceContainer else MaterialTheme.colorScheme.surfaceContainerHigh),
        elevation = getCardElevation()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(if (isExpressiveUi) 16.dp else 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Surface(
                shape = if (isExpressiveUi) MaterialTheme.shapes.small else RoundedCornerShape(4.dp),
                color = if (isExpressiveUi) when (rule.type) {
                    "hide", "mount_hide", "stealth" -> MaterialTheme.colorScheme.tertiaryContainer
                    "add", "inject", "merge" -> MaterialTheme.colorScheme.primaryContainer
                    else -> MaterialTheme.colorScheme.secondaryContainer
                } else when (rule.type) {
                    "add" -> Color(0xFF1B5E20).copy(alpha = 0.3f)
                    "hide" -> Color(0xFFB71C1C).copy(alpha = 0.3f)
                    "inject" -> Color(0xFF1565C0).copy(alpha = 0.3f)
                    "merge" -> Color(0xFF4A148C).copy(alpha = 0.3f)
                    "mount_hide" -> Color(0xFF0D47A1).copy(alpha = 0.3f)
                    "maps_spoof" -> Color(0xFF1A237E).copy(alpha = 0.3f)
                    "statfs_spoof" -> Color(0xFF311B92).copy(alpha = 0.3f)
                    "stealth" -> Color(0xFF37474F).copy(alpha = 0.3f)
                    else -> MaterialTheme.colorScheme.secondaryContainer
                }
            ) {
                Text(
                    text = when (rule.type) {
                        "mount_hide" -> "MOUNT_HIDE"
                        "maps_spoof" -> "MAPS_SPOOF"
                        "statfs_spoof" -> "STATFS_SPOOF"
                        "stealth" -> "STEALTH"
                        else -> rule.type.uppercase()
                    },
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold
                )
            }

            Column(modifier = Modifier.weight(1f)) {
                val displayText = when (rule.type) {
                    "mount_hide" -> stringResource(R.string.kasumi_rule_mount_hide)
                    "maps_spoof" -> stringResource(R.string.kasumi_rule_maps_spoof)
                    "statfs_spoof" -> stringResource(R.string.kasumi_rule_statfs_spoof)
                    "stealth" -> stringResource(R.string.kasumi_rule_stealth)
                    else -> rule.src
                }
                ScrollableRuleText(
                    text = displayText,
                    fontFamily = if (rule.type in listOf("mount_hide", "maps_spoof", "statfs_spoof", "stealth"))
                        FontFamily.Default else FontFamily.Monospace,
                )
                if (rule.hideState != null) {
                    Text(userHideStatus(rule), style = MaterialTheme.typography.bodySmall,
                        color = if (rule.hideState == 3) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (rule.target != null) {
                    ScrollableRuleText(
                        text = "→ ${rule.target}",
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                    itemVerticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = stringResource(if (rule.isUserDefined) R.string.kasumi_rule_user else R.string.kasumi_rule_module),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (rule.bypass) {
                        Surface(
                            shape = MaterialTheme.shapes.extraSmall,
                            color = MaterialTheme.colorScheme.secondaryContainer,
                            contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                        ) {
                            Text(
                                text = stringResource(R.string.kasumi_hide_scope_global),
                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                style = MaterialTheme.typography.labelSmall,
                            )
                        }
                    }
                }
            }
        }
    }
}


@Composable
private fun ScrollableRuleText(text: String, fontFamily: FontFamily, color: Color = LocalContentColor.current) {
    val scrollState = rememberScrollState()
    LaunchedEffect(text) { scrollState.scrollTo(0) }
    Text(
        text = text,
        modifier = Modifier.fillMaxWidth().horizontalScroll(scrollState),
        style = MaterialTheme.typography.bodySmall,
        fontFamily = fontFamily,
        color = color,
        maxLines = 1,
        softWrap = false,
        overflow = TextOverflow.Clip,
    )
}

enum class LogLevel(displayNameRes: Int, color: Color, tag: String) {
    VERBOSE(R.string.kasumi_logs_filter_verbose, Color(0xFF9C27B0), "VERBOSE"),
    DEBUG(R.string.kasumi_logs_filter_debug, Color(0xFF4CAF50), "DEBUG"),
    INFO(R.string.kasumi_logs_filter_info, Color(0xFF2196F3), "INFO"),
    WARN(R.string.kasumi_logs_filter_warn, Color(0xFFFF9800), "WARN"),
    ERROR(R.string.kasumi_logs_filter_error, Color(0xFFF44336), "ERROR");

    val displayNameRes: Int = displayNameRes
    val color: Color = color
    val tag: String = tag
}

private val taggedLogLevel = Regex("\\[(VERBOSE|DEBUG|INFO|WARN(?:ING)?|ERROR)]", RegexOption.IGNORE_CASE)

private fun logLevelOf(line: String): LogLevel? {
    val tag = taggedLogLevel.find(line)?.groupValues?.get(1)?.uppercase()
    if (tag != null) return LogLevel.entries.firstOrNull { it.tag == if (tag == "WARNING") "WARN" else tag }
    return LogLevel.entries.asReversed().firstOrNull { line.contains(it.tag, ignoreCase = true) }
}

@Composable
internal fun LogsTab(
    showKernelLog: Boolean,
    onToggleLogType: () -> Unit,
    logContent: String,
    onRefreshLog: () -> Unit,
    onClearLog: () -> Unit,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val snackbarHostState = rememberSnackbarController()
    val coroutineScope = rememberCoroutineScope()

    var selectedLogLevels by remember { mutableStateOf(emptySet<LogLevel>()) }
    var filterExpanded by remember { mutableStateOf(false) }
    var sourceExpanded by remember { mutableStateOf(false) }
    var searchExpanded by remember { mutableStateOf(false) }
    var search by remember { mutableStateOf("") }
    var clearConfirm by remember { mutableStateOf(false) }
    val searchFocus = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current

    fun closeSearch() {
        search = ""
        searchExpanded = false
        focusManager.clearFocus()
        keyboard?.hide()
    }

    BackHandler(enabled = searchExpanded) { closeSearch() }

    LaunchedEffect(showKernelLog) {
        onRefreshLog()
    }

    val filteredLogContent = remember(logContent, selectedLogLevels, search) {
        logContent.lineSequence().filter { line ->
            (search.isEmpty() || line.contains(search, ignoreCase = true)) &&
                (selectedLogLevels.isEmpty() || logLevelOf(line) in selectedLogLevels)
        }.joinToString("\n")
    }

    if (clearConfirm) YukiAlertDialog(
        onDismissRequest = { clearConfirm = false },
        title = { Text(stringResource(R.string.kasumi_logs_clear)) },
        text = { Text(stringResource(R.string.kasumi_logs_clear_confirm)) },
        confirmButton = { TextButton(onClick = { clearConfirm = false; onClearLog() }) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { clearConfirm = false }) { Text(stringResource(android.R.string.cancel)) } },
    )

    val annotatedLogContent = remember(filteredLogContent) {
        buildAnnotatedString {
            filteredLogContent.lines().forEach { line ->
                val color = logLevelOf(line)?.color ?: Color.White
                withStyle(style = SpanStyle(color = color)) {
                    append(line)
                }
                append("\n")
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {

        KasumiLogActions {
            val motion = LocalYukiMotion.current
            AnimatedContent(
                targetState = searchExpanded,
                modifier = Modifier.weight(1f).yukiAnimateContentSize(),
                transitionSpec = {
                    (fadeIn(motion.effects()) togetherWith fadeOut(motion.effects()))
                        .using(null)
                },
                label = "kasumiLogSearch",
            ) { searching ->
                Row(
                    modifier = Modifier.fillMaxWidth().then(
                        if (searching != searchExpanded) Modifier.clearAndSetSemantics {} else Modifier
                    ),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (searching) {
                        LaunchedEffect(searchExpanded) {
                            if (searchExpanded) searchFocus.requestFocus()
                        }
                        OutlinedTextField(
                            value = search,
                            onValueChange = { search = it },
                            enabled = searchExpanded,
                            modifier = Modifier.weight(1f).focusRequester(searchFocus),
                            singleLine = true,
                            placeholder = {
                                Text(stringResource(R.string.kasumi_logs_search), maxLines = 1, overflow = TextOverflow.Ellipsis)
                            },
                            trailingIcon = {
                                IconButton(enabled = searchExpanded, onClick = { closeSearch() }) {
                                    YukiIcon(Icons.Filled.Close, stringResource(R.string.close))
                                }
                            },
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                            keyboardActions = KeyboardActions(onSearch = {
                                focusManager.clearFocus()
                                keyboard?.hide()
                            }),
                        )
                    } else {
                        Box(Modifier.weight(1f)) {
                            FilledTonalButton(enabled = !searchExpanded, onClick = { sourceExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                                Text(
                                    stringResource(if (showKernelLog) R.string.kasumi_logs_kernel else R.string.kasumi_logs_daemon),
                                    modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis,
                                )
                                YukiExpandArrow(sourceExpanded && !searchExpanded, Modifier.size(18.dp))
                            }
                            DropdownMenu(expanded = sourceExpanded && !searchExpanded, onDismissRequest = { sourceExpanded = false }) {
                                listOf(false, true).forEach { kernel ->
                                    DropdownMenuItem(
                                        text = { Text(stringResource(if (kernel) R.string.kasumi_logs_kernel else R.string.kasumi_logs_daemon)) },
                                        trailingIcon = {
                                            if (showKernelLog == kernel) YukiIcon(Icons.Filled.Check, null)
                                        },
                                        onClick = {
                                            sourceExpanded = false
                                            if (showKernelLog != kernel) onToggleLogType()
                                        },
                                    )
                                }
                            }
                        }
                        IconButton(enabled = !searchExpanded, onClick = { searchExpanded = true }) {
                            YukiIcon(Icons.Filled.Search, stringResource(R.string.kasumi_logs_search))
                        }
                    }
                }
            }
            IconButton(onClick = onRefreshLog) {
                YukiIcon(Icons.Filled.Refresh, stringResource(R.string.kasumi_rules_refresh))
            }

            Box {
                IconButton(onClick = { filterExpanded = true }) {
                    YukiIcon(
                        Icons.Filled.MoreVert,
                        contentDescription = stringResource(R.string.kasumi_logs_actions),
                        tint = if (selectedLogLevels.isEmpty()) LocalContentColor.current else MaterialTheme.colorScheme.primary,
                    )
                }
                DropdownMenu(
                    expanded = filterExpanded,
                    onDismissRequest = { filterExpanded = false },
                    modifier = Modifier.widthIn(min = 180.dp)
                ) {
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.kasumi_logs_copy)) },
                        leadingIcon = { YukiIcon(Icons.Filled.ContentCopy, null) },
                        onClick = {
                            filterExpanded = false
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                            val clip = android.content.ClipData.newPlainText("Kasumi Log", filteredLogContent)
                            clipboard.setPrimaryClip(clip)
                            coroutineScope.launchUi(snackbarHostState) {
                                snackbarHostState.showSnackbar(resources.getString(R.string.kasumi_logs_copy_success))
                            }
                        },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.kasumi_logs_clear)) },
                        leadingIcon = { YukiIcon(Icons.Filled.Delete, null) },
                        enabled = !showKernelLog,
                        onClick = { filterExpanded = false; clearConfirm = true },
                    )
                    HorizontalDivider()
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        Text(
                            stringResource(R.string.kasumi_logs_filter),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(horizontal = 12.dp),
                        )
                        FilterChip(
                            selected = selectedLogLevels.isEmpty(),
                            onClick = {
                                selectedLogLevels = emptySet()
                                filterExpanded = false
                            },
                            label = { Text(stringResource(R.string.kasumi_logs_filter_all)) },
                            modifier = Modifier.padding(horizontal = 12.dp)
                        )
                        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
                        FlowRow(
                            modifier = Modifier.padding(horizontal = 12.dp),
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            LogLevel.entries.forEach { level ->
                                FilterChip(
                                    selected = level in selectedLogLevels,
                                    onClick = {
                                        selectedLogLevels = if (level in selectedLogLevels) {
                                            selectedLogLevels - level
                                        } else {
                                            selectedLogLevels + level
                                        }
                                    },
                                    label = { Text(stringResource(level.displayNameRes)) },
                                    leadingIcon = {
                                        Box(
                                            modifier = Modifier
                                                .size(8.dp)
                                                .background(level.color, shape = RoundedCornerShape(4.dp))
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }

        Card(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            shape = kasumiCardShape(),
            colors = CardDefaults.cardColors(
                containerColor = Color(0xFF1E1E1E)
            )
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                val scrollState = rememberScrollState()

                Text(
                    text = annotatedLogContent,
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(scrollState)
                        .padding(12.dp),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    fontSize = if (isExpressiveUi) 12.sp else 11.sp
                )

                SnackbarHost(
                    hostState = snackbarHostState.hostState,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(16.dp)
                )
            }
        }
    }
}

private fun CoroutineScope.launchUi(snackbar: SnackbarController, action: suspend () -> Unit) = launch {
    try { action() }
    catch (error: Exception) {
        if (error is CancellationException) throw error
        snackbar.showSnackbar(error.message ?: "Operation failed")
    }
}
