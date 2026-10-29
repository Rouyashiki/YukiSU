package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.selection.toggleable
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.rounded.FolderDelete
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.core.content.edit
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import com.maxkeppeker.sheets.core.models.base.IconSource
import com.maxkeppeler.sheets.list.models.ListOption
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AboutScreenDestination
import com.ramcosta.composedestinations.generated.destinations.AppProfileTemplateScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FeatureControlScreenDestination
import com.anatdx.yukisu.ui.kasumi.util.KasumiManager
import com.ramcosta.composedestinations.generated.destinations.LogViewerScreenDestination
import com.ramcosta.composedestinations.generated.destinations.UmountManagerScreenDestination
import com.ramcosta.composedestinations.generated.destinations.UtsViewScreenDestination
import com.ramcosta.composedestinations.generated.destinations.MoreSettingsScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.magica.MagicaHelper
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.*
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.ManagerUi
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import ui.screen.feature.FeatureControlState

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
private val SPACING_SMALL = 3.dp
private val SPACING_MEDIUM = 8.dp
private val SPACING_LARGE = 16.dp

enum class SettingsItemPosition(val index: Int, val count: Int) {
    First(0, 3),
    Middle(1, 3),
    Last(2, 3),
    Only(0, 1)
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SettingScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val snackBarHost = rememberSnackbarController()
    val context = LocalContext.current
    val resources = LocalResources.current
    val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
    val isKsuManager = remember { Natives.isManager }
    val initialSuLogEnabled = remember { Natives.isSuLogEnabled() }
    val isSuLogEnabled = FeatureControlState.suLogEnabled ?: initialSuLogEnabled
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        FeatureControlState.refreshSuLog()
    }
    var selectedEngine by rememberSaveable {
        mutableStateOf(
            prefs.getString("webui_engine", "default") ?: "default"
        )
    }
    var autoJailbreak by rememberSaveable {
        mutableStateOf(MagicaHelper.isAutoJailbreakEnabled(context))
    }
    var useSoftReboot by rememberSaveable {
        mutableStateOf(isSoftRebootEnabled(context))
    }
    var softRebootBlocked by remember { mutableStateOf(isSoftRebootBlockedByKasumi()) }
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        softRebootBlocked = isSoftRebootBlockedByKasumi()
    }
    val isLateLoadMode = remember {
        runCatching { Natives.isLateLoadMode }.getOrDefault(false)
    }

    Scaffold(
        topBar = {
            TopBar(scrollBehavior = scrollBehavior)
        },
        snackbarHost = { SnackbarHost(snackBarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        val loadingDialog = rememberLoadingDialog()

        Column(
            modifier = Modifier
                .padding(paddingValues)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .verticalScroll(rememberScrollState())
        ) {
            val context = LocalContext.current
            val scope = rememberCoroutineScope()
            val exportBugreportLauncher = rememberLauncherForActivityResult(
                ActivityResultContracts.CreateDocument("application/gzip")
            ) { uri: Uri? ->
                if (uri == null) return@rememberLauncherForActivityResult
                scope.launch {
                    val saved = loadingDialog.withLoading {
                        runCatching {
                            withContext(Dispatchers.IO) {
                                val output = checkNotNull(
                                    context.contentResolver.openOutputStream(uri)
                                )
                                output.use { outputStream ->
                                    getBugreportFile(context).inputStream().use { input ->
                                        input.copyTo(outputStream)
                                    }
                                }
                            }
                        }.onFailure {
                            if (it is CancellationException) throw it
                        }.isSuccess
                    }
                    val message = if (saved) R.string.log_saved else R.string.operation_failed
                    snackBarHost.showSnackbar(resources.getString(message))
                }
            }

            KsuIsValid {
                SettingsGroupCard(
                    title = stringResource(R.string.configuration),
                    content = {
                        SettingItem(
                            icon = Icons.Filled.Fence,
                            title = stringResource(R.string.settings_profile_template),
                            summary = stringResource(R.string.settings_profile_template_summary),
                            groupPosition = SettingsItemPosition.First,
                            onClick = {
                                navigator.navigate(AppProfileTemplateScreenDestination)
                            }
                        )

                        SettingItem(
                            icon = Icons.Filled.Memory,
                            title = stringResource(R.string.feature_control),
                            summary = stringResource(R.string.feature_control_summary),
                            onClick = {
                                navigator.navigate(FeatureControlScreenDestination)
                            }
                        )

                        var builtinMountEnabled by rememberSaveable { mutableStateOf<Boolean?>(null) }
                        var builtinLoading by remember { mutableStateOf(true) }
                        var builtinSaving by remember { mutableStateOf(false) }
                        LaunchedEffect(Unit) {
                            try { builtinMountEnabled = KasumiManager.isBuiltinMountEnabled() }
                            catch (error: Exception) {
                                if (error is CancellationException) throw error
                            } finally {
                                builtinLoading = false
                            }
                        }
                        SwitchItem(
                            icon = Icons.Filled.Storage,
                            title = stringResource(R.string.kasumi_builtin_mount),
                            summary = stringResource(
                                if (builtinMountEnabled == null && !builtinLoading) {
                                    R.string.operation_failed
                                } else {
                                    R.string.kasumi_builtin_mount_desc
                                }
                            ),
                            checked = builtinMountEnabled,
                            enabled = !builtinLoading && !builtinSaving,
                            loading = builtinLoading,
                            onCheckedChange = { enable ->
                                if (!builtinSaving) {
                                    builtinSaving = true
                                    scope.launch {
                                        try {
                                            KasumiManager.setBuiltinMountEnabled(enable)
                                            builtinMountEnabled = enable
                                            snackBarHost.showSnackbar(resources.getString(
                                                if (enable) R.string.kasumi_toast_builtin_enabled else R.string.kasumi_toast_builtin_disabled
                                            ))
                                        } catch (error: Exception) {
                                            if (error is CancellationException) throw error
                                            snackBarHost.showSnackbar(resources.getString(R.string.kasumi_toast_builtin_failed))
                                        } finally { builtinSaving = false }
                                    }
                                }
                            }
                        )

                        val superKeyDialog = rememberCustomDialog { dismiss ->
                            SuperKeySettingsDialog(
                                onDismiss = dismiss,
                                onKeyCleared = {
                                    scope.launch {
                                        snackBarHost.showSnackbar(
                                            resources.getString(R.string.clear_super_key) + " ✓"
                                        )
                                    }
                                }
                            )
                        }
                        SettingItem(
                            icon = Icons.Filled.Key,
                            title = stringResource(R.string.settings_superkey_management),
                            summary = stringResource(R.string.settings_superkey_management_summary),
                            onClick = { superKeyDialog.show() }
                        )

                        var umountChecked by rememberSaveable { mutableStateOf(Natives.isDefaultUmountModules()) }
                        SwitchItem(
                            icon = Icons.Rounded.FolderDelete,
                            title = stringResource(id = R.string.settings_umount_modules_default),
                            summary = stringResource(id = R.string.settings_umount_modules_default_summary),
                            checked = umountChecked,
                            onCheckedChange = {
                                if (Natives.setDefaultUmountModules(it)) {
                                    umountChecked = it
                                }
                            }
                        )

                        val utsViewSupported by produceState<Boolean?>(initialValue = null) {
                            value = isUtsViewSupported()
                        }
                        SettingItem(
                            icon = Icons.Filled.Language,
                            title = stringResource(R.string.settings_uts_view),
                            groupPosition = SettingsItemPosition.Last,
                            summary = if (utsViewSupported == false) {
                                stringResource(R.string.feature_status_unsupported_summary)
                            } else {
                                stringResource(R.string.settings_uts_view_summary)
                            },
                            enabled = utsViewSupported == true,
                            onClick = {
                                if (utsViewSupported == true) {
                                    navigator.navigate(UtsViewScreenDestination)
                                }
                            }
                        )
                    }
                )
            }

            SettingsGroupCard(
                title = stringResource(R.string.app_settings),
                content = {
                    var checkUpdate by rememberSaveable {
                        mutableStateOf(prefs.getBoolean("check_update", true))
                    }
                    SwitchItem(
                        icon = Icons.Filled.Update,
                        title = stringResource(R.string.settings_check_update),
                        summary = stringResource(R.string.settings_check_update_summary),
                        checked = checkUpdate,
                        groupPosition = SettingsItemPosition.First,
                        onCheckedChange = { enabled ->
                            prefs.edit { putBoolean("check_update", enabled) }
                            checkUpdate = enabled
                        }
                    )

                    var checkCiUpdate by rememberSaveable {
                        mutableStateOf(prefs.getBoolean("check_ci_update", false))
                    }
                    YukiExpandableContent(visible = checkUpdate) {
                        SwitchItem(
                            icon = Icons.Filled.DeveloperMode,
                            title = stringResource(R.string.settings_check_ci_update),
                            summary = stringResource(R.string.settings_check_ci_update_summary),
                            checked = checkCiUpdate,
                            onCheckedChange = { enabled ->
                                prefs.edit { putBoolean("check_ci_update", enabled) }
                                checkCiUpdate = enabled
                            }
                        )
                    }

                    var autoUpdateKsud by rememberSaveable {
                        mutableStateOf(prefs.getBoolean("auto_update_ksud", false))
                    }
                    SwitchItem(
                        icon = Icons.Filled.Sync,
                        title = stringResource(R.string.settings_auto_update_ksud),
                        summary = stringResource(R.string.settings_auto_update_ksud_summary),
                        checked = autoUpdateKsud,
                        onCheckedChange = { enabled ->
                            prefs.edit { putBoolean("auto_update_ksud", enabled) }
                            autoUpdateKsud = enabled
                        }
                    )

                    KsuIsValid {
                        WebUIEngineSelector(
                            selectedEngine = selectedEngine,
                            onEngineSelected = { engine ->
                                selectedEngine = engine
                                prefs.edit { putString("webui_engine", engine) }
                            }
                        )
                    }

                    SwitchItem(
                        icon = Icons.Filled.ElectricalServices,
                        title = stringResource(R.string.settings_auto_jailbreak),
                        summary = stringResource(R.string.settings_auto_jailbreak_summary),
                        enabled = isLateLoadMode,
                        checked = autoJailbreak,
                        onCheckedChange = { enabled ->
                            MagicaHelper.setAutoJailbreakEnabled(context, enabled)
                            autoJailbreak = enabled
                        }
                    )

                    SwitchItem(
                        icon = Icons.Filled.RestartAlt,
                        title = stringResource(R.string.settings_soft_reboot),
                        summary = stringResource(
                            if (softRebootBlocked) R.string.settings_soft_reboot_kasumi_unavailable
                            else R.string.settings_soft_reboot_summary
                        ),
                        enabled = !isLateLoadMode && !softRebootBlocked,
                        checked = !softRebootBlocked && (isLateLoadMode || useSoftReboot),
                        onCheckedChange = { enabled ->
                            setSoftRebootEnabled(context, enabled)
                            useSoftReboot = enabled
                        }
                    )

                    SettingItem(
                        icon = Icons.Filled.Settings,
                        title = stringResource(R.string.more_settings),
                        summary = stringResource(R.string.more_settings),
                        groupPosition = SettingsItemPosition.Last,
                        onClick = {
                            navigator.navigate(MoreSettingsScreenDestination)
                        }
                    )
                }
            )

            SettingsGroupCard(
                title = stringResource(R.string.tools),
                content = {
                    var showBottomsheet by remember { mutableStateOf(false) }

                    SettingItem(
                        icon = Icons.Filled.BugReport,
                        title = stringResource(R.string.send_log),
                        groupPosition = if (isKsuManager) {
                            SettingsItemPosition.First
                        } else {
                            SettingsItemPosition.Only
                        },
                        onClick = {
                            showBottomsheet = true
                        }
                    )

                    KsuIsValid {
                        if (isSuLogEnabled) {
                            SettingItem(
                                icon = Icons.Filled.Visibility,
                                title = stringResource(R.string.log_viewer_view_logs),
                                summary = stringResource(R.string.log_viewer_view_logs_summary),
                                onClick = {
                                    navigator.navigate(LogViewerScreenDestination)
                                }
                            )
                        }
                    }
                    KsuIsValid {
                        SettingItem(
                            icon = Icons.Filled.FolderOff,
                            title = stringResource(R.string.umount_path_manager),
                            summary = stringResource(R.string.umount_path_manager_summary),
                            onClick = {
                                navigator.navigate(UmountManagerScreenDestination)
                            }
                        )
                    }

                    if (showBottomsheet) {
                        LogBottomSheet(
                            onDismiss = { showBottomsheet = false },
                            onSaveLog = {
                                val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm")
                                val current = LocalDateTime.now().format(formatter)
                                exportBugreportLauncher.launch("YukiSU_bugreport_${current}.tar.gz")
                                showBottomsheet = false
                            },
                            onShareLog = {
                                scope.launch {
                                    val bugreport = loadingDialog.withLoading {
                                        runCatching {
                                            withContext(Dispatchers.IO) {
                                                getBugreportFile(context)
                                            }
                                        }.onFailure {
                                            if (it is CancellationException) throw it
                                        }
                                    }.getOrElse {
                                        snackBarHost.showSnackbar(
                                            resources.getString(R.string.operation_failed)
                                        )
                                        return@launch
                                    }

                                    val shared = runCatching {
                                        val uri = FileProvider.getUriForFile(
                                            context,
                                            "${BuildConfig.APPLICATION_ID}.fileprovider",
                                            bugreport
                                        )

                                        val shareIntent = Intent(Intent.ACTION_SEND).apply {
                                            putExtra(Intent.EXTRA_STREAM, uri)
                                            setDataAndType(uri, "application/gzip")
                                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                        }

                                        context.startActivity(
                                            Intent.createChooser(
                                                shareIntent,
                                                resources.getString(R.string.send_log)
                                            )
                                        )
                                    }.isSuccess
                                    if (shared) {
                                        showBottomsheet = false
                                    } else {
                                        snackBarHost.showSnackbar(
                                            resources.getString(R.string.operation_failed)
                                        )
                                    }
                                }
                            }
                        )
                    }
                    KsuIsValid {
                        UninstallItem(
                            navigator = navigator,
                            groupPosition = SettingsItemPosition.Last
                        ) {
                            loadingDialog.withLoading(it)
                        }
                    }
                }
            )

            SettingsGroupCard(
                title = stringResource(R.string.about),
                content = {
                    SettingItem(
                        icon = Icons.Filled.Info,
                        title = stringResource(R.string.about),
                        groupPosition = SettingsItemPosition.Only,
                        onClick = {
                            navigator.navigate(AboutScreenDestination)
                        }
                    )
                }
            )

            Spacer(modifier = Modifier.height(SPACING_LARGE))
        }
    }
}

@Composable
private fun SettingsGroupCard(
    title: String,
    content: @Composable ColumnScope.() -> Unit
) {
    if (isExpressiveUi) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = SPACING_LARGE, vertical = 12.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
                modifier = Modifier.padding(horizontal = ManagerUi.SectionTitlePadding, vertical = ManagerUi.SectionTitlePadding),
                color = MaterialTheme.colorScheme.primary
            )
            Column(content = content)
        }
    } else {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation()
        ) {
            Column(modifier = Modifier.padding(vertical = SPACING_MEDIUM)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(
                        horizontal = SPACING_LARGE,
                        vertical = SPACING_MEDIUM
                    ),
                    color = MaterialTheme.colorScheme.primary
                )
                content()
            }
        }
    }
}

@Composable
private fun WebUIEngineSelector(
    selectedEngine: String,
    onEngineSelected: (String) -> Unit
) {
    var showDialog by remember { mutableStateOf(false) }
    val engineOptions = listOf(
        "default" to stringResource(R.string.engine_auto_select),
        "wx" to stringResource(R.string.engine_force_webuix),
        "ksu" to stringResource(R.string.engine_force_ksu)
    )

    SettingItem(
        icon = Icons.Filled.WebAsset,
        title = stringResource(R.string.use_webuix),
        summary = engineOptions.find { it.first == selectedEngine }?.second
            ?: stringResource(R.string.engine_auto_select),
        onClick = { showDialog = true }
    )

    if (showDialog) {
        YukiAlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(stringResource(R.string.use_webuix)) },
            text = {
                Column {
                    engineOptions.forEach { (value, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    onEngineSelected(value)
                                    showDialog = false
                                }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = selectedEngine == value,
                                onClick = null
                            )
                            Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                            Text(text = label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogBottomSheet(
    onDismiss: () -> Unit,
    onSaveLog: () -> Unit,
    onShareLog: () -> Unit
) {
    YukiModalBottomSheet(
        onDismissRequest = onDismiss,
        modifier = Modifier.clickHapticFeedback(),
        containerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(SPACING_LARGE),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            LogActionButton(
                icon = Icons.Filled.Save,
                text = stringResource(R.string.save_log),
                onClick = onSaveLog
            )

            LogActionButton(
                icon = Icons.Filled.Share,
                text = stringResource(R.string.send_log),
                onClick = onShareLog
            )
        }
        Spacer(modifier = Modifier.height(SPACING_LARGE))
    }
}

@Composable
fun LogActionButton(
    icon: ImageVector,
    text: String,
    onClick: () -> Unit
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(SPACING_MEDIUM)
    ) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(56.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primaryContainer)
        ) {
            YukiIcon(
                imageVector = icon,
                contentDescription = text,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(24.dp)
            )
        }
        Spacer(modifier = Modifier.height(SPACING_MEDIUM))
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
fun SettingItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    val expressiveShape = if (groupPosition == SettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.5f)
            .then(
                if (isExpressiveUi) {
                    Modifier
                        .padding(
                            horizontal = 6.dp,
                            vertical = ListItemDefaults.SegmentedGap / 2
                        )
                        .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                        .clip(expressiveShape)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                        )
                        .clickable(enabled = enabled, onClick = onClick)
                } else {
                    Modifier.clickable(enabled = enabled, onClick = onClick)
                }
            )
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = if (isExpressiveUi) Alignment.CenterVertically else Alignment.Top
    ) {
        SettingsLeadingIcon(icon = icon, tint = MaterialTheme.colorScheme.primary)

        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else null
            )
            if (summary != null) {
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (isExpressiveUi) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        Color.Unspecified
                    }
                )
            }
        }
        YukiIcon(
            imageVector = Icons.AutoMirrored.Filled.NavigateNext,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(24.dp)
        )
    }
}

@Composable
fun SwitchItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    checked: Boolean?,
    enabled: Boolean = true,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    loading: Boolean = false,
    onCheckedChange: (Boolean) -> Unit
) {
    val switchEnabled = enabled && checked != null && !loading
    val expressiveShape = if (groupPosition == SettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    val titleColor = if (switchEnabled) {
        MaterialTheme.colorScheme.onSurface
    } else {
        MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
    }
    val summaryColor = if (switchEnabled) {
        MaterialTheme.colorScheme.onSurfaceVariant
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
    }
    val iconTint = when {
        !switchEnabled -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
        checked == true -> MaterialTheme.colorScheme.primary
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    val toggleModifier = if (checked != null) {
        Modifier.toggleable(
            value = checked,
            enabled = switchEnabled,
            role = Role.Switch,
            onValueChange = onCheckedChange,
        )
    } else {
        Modifier
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (isExpressiveUi) {
                    Modifier
                        .padding(
                            horizontal = 6.dp,
                            vertical = ListItemDefaults.SegmentedGap / 2
                        )
                        .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                        .clip(expressiveShape)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                        )
                } else {
                    Modifier
                }
            )
            .then(toggleModifier)
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = if (isExpressiveUi) Alignment.CenterVertically else Alignment.Top
    ) {
        SettingsLeadingIcon(icon = icon, tint = iconTint)

        Column(modifier = Modifier.weight(1f).padding(end = 12.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                color = titleColor
            )
            if (summary != null) {
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = summaryColor
                )
            }
        }
        if (checked != null) {
            YukiSwitch(
                checked = checked,
                enabled = switchEnabled,
                onCheckedChange = null
            )
        } else {
            Box(
                modifier = Modifier.size(width = 52.dp, height = 32.dp),
                contentAlignment = Alignment.Center,
            ) {
                if (loading) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                } else {
                    Text(text = "—", color = summaryColor)
                }
            }
        }
    }
}

@Composable
private fun SettingsLeadingIcon(icon: ImageVector, tint: Color) {
    if (isExpressiveUi) {
        YukiIcon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .padding(end = SPACING_LARGE)
                .size(24.dp)
        )
    } else {
        YukiIcon(
            imageVector = icon,
            contentDescription = null,
            tint = tint,
            modifier = Modifier
                .padding(end = SPACING_LARGE)
                .size(24.dp)
        )
    }
}

@Composable
fun UninstallItem(
    navigator: DestinationsNavigator,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    withLoading: suspend (suspend () -> Unit) -> Unit
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val uninstallConfirmDialog = rememberConfirmDialog()
    val uninstallDialog = rememberUninstallDialog { uninstallType ->
        scope.launch {
            val result = uninstallConfirmDialog.awaitConfirm(
                title = resources.getString(uninstallType.title),
                content = resources.getString(uninstallType.message)
            )
            if (result == ConfirmResult.Confirmed) {
                withLoading {
                    when (uninstallType) {
                        UninstallType.PERMANENT -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashUninstall)
                        )
                        UninstallType.RESTORE_STOCK_IMAGE -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashRestore)
                        )
                        UninstallType.NONE -> Unit
                    }
                }
            }
        }
    }

    SettingItem(
        icon = Icons.Filled.Delete,
        title = stringResource(id = R.string.settings_uninstall),
        groupPosition = groupPosition,
        onClick = {
            uninstallDialog.show()
        }
    )
}

enum class UninstallType(val title: Int, val message: Int, val icon: ImageVector) {
    PERMANENT(
        R.string.settings_uninstall_permanent,
        R.string.settings_uninstall_permanent_message,
        Icons.Filled.DeleteForever
    ),
    RESTORE_STOCK_IMAGE(
        R.string.settings_restore_stock_image,
        R.string.settings_restore_stock_image_message,
        Icons.AutoMirrored.Filled.Undo
    ),
    NONE(0, 0, Icons.Filled.Delete)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun rememberUninstallDialog(onSelected: (UninstallType) -> Unit): DialogHandle {
    return rememberCustomDialog { dismiss ->
        val options = listOf(
            UninstallType.PERMANENT,
            UninstallType.RESTORE_STOCK_IMAGE
        )
        val listOptions = options.map {
            ListOption(
                titleText = stringResource(it.title),
                subtitleText = if (it.message != 0) stringResource(it.message) else null,
                icon = IconSource(it.icon)
            )
        }

        var selectedOption by remember { mutableStateOf<UninstallType?>(null) }

        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme.copy(
                surface = MaterialTheme.colorScheme.surfaceContainerHigh
            )
        ) {
            YukiAlertDialog(
                onDismissRequest = {
                    dismiss()
                },
                title = {
                    Text(
                        text = stringResource(R.string.settings_uninstall),
                    )
                },
                text = {
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        options.forEachIndexed { index, option ->
                            val isSelected = selectedOption == option
                            val backgroundColor = if (isSelected)
                                MaterialTheme.colorScheme.primaryContainer
                            else
                                Color.Transparent
                            val contentColor = if (isSelected)
                                MaterialTheme.colorScheme.onPrimaryContainer
                            else
                                MaterialTheme.colorScheme.onSurface

                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(MaterialTheme.shapes.medium)
                                    .background(backgroundColor)
                                    .clickable {
                                        selectedOption = option
                                    }
                                    .padding(vertical = 12.dp, horizontal = 8.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                YukiIcon(
                                    imageVector = option.icon,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier
                                        .padding(end = 16.dp)
                                        .size(24.dp)
                                )
                                Column(
                                    modifier = Modifier.weight(1f)
                                ) {
                                    Text(
                                        text = listOptions[index].titleText,
                                        style = MaterialTheme.typography.titleMedium,
                                    )
                                    listOptions[index].subtitleText?.let {
                                        Text(
                                            text = it,
                                            style = MaterialTheme.typography.bodyMedium,
                                            color = if (isSelected)
                                                contentColor.copy(alpha = 0.8f)
                                            else
                                                MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                                if (isSelected) {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonChecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.primary,
                                        modifier = Modifier.size(24.dp)
                                    )
                                } else {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonUnchecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.size(24.dp)
                                    )
                                }
                            }
                        }
                    }
                },
                confirmButton = {
                    Button(
                        onClick = {
                            selectedOption?.let { onSelected(it) }
                            dismiss()
                        },
                        enabled = selectedOption != null,
                    ) {
                        Text(
                            text = stringResource(android.R.string.ok)
                        )
                    }
                },
                dismissButton = {
                    TextButton(
                        onClick = {
                            dismiss()
                        }
                    ) {
                        Text(
                            text = stringResource(android.R.string.cancel),
                        )
                    }
                },
                shape = MaterialTheme.shapes.extraLarge,
                tonalElevation = 4.dp
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    val colors = getTopAppBarColors(
        containerColor = cardColor,
        scrolledContainerColor = cardColor
    )
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.settings),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null
        )
    }

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = title,
            colors = colors,
            windowInsets = WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal
            ),
            scrollBehavior = scrollBehavior
        )
    } else {
        TopAppBar(
            title = title,
            colors = colors,
            windowInsets = WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal
            ),
            scrollBehavior = scrollBehavior
        )
    }
}
