package com.anatdx.yukisu.ui.kasumi

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Subject
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.ui.util.SnackbarController
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.kasumi.util.KasumiUiAdapter as Controller
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingsControlGroup

@Composable
internal fun SettingsTab(
    config: Controller.KagamiConfig,
    kasumiStatus: Controller.KasumiStatus,
    features: Controller.FeaturesResult?,
    snackbarHostState: SnackbarController,
    controlsEnabled: Boolean,
    runtimeApplyPending: Boolean,
    hideDraftPath: String,
    hideDraftBypass: Boolean,
    onHideDraftPathChange: (String) -> Unit,
    onHideDraftBypassChange: (Boolean) -> Unit,
    onRetryApply: () -> Unit,
    onConfigChanged: (Controller.KagamiConfig) -> Unit,
) {
    val enabled = controlsEnabled
    val mountEnabled = enabled && config.externalOwner.isEmpty()
    val kernelEnabled = enabled && kasumiStatus == Controller.KasumiStatus.AVAILABLE
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        ConfigSection(stringResource(R.string.kasumi_general), segmented = true) {
            if (config.externalOwner.isNotEmpty()) {
                KasumiControlGroup {
                    Text(stringResource(R.string.kasumi_external_mount, config.externalOwner),
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(12.dp))
                }
            }
            SettingSwitch(stringResource(R.string.kasumi_debug), stringResource(R.string.kasumi_debug_desc),
                config.debug, { onConfigChanged(config.copy(debug = it)) }, enabled,
                Icons.Filled.BugReport, MoreSettingsItemPosition.First)
            SettingsDivider()
            SettingSwitch(stringResource(R.string.kasumi_verbose), stringResource(R.string.kasumi_verbose_desc),
                config.verbose, { onConfigChanged(config.copy(verbose = it)) }, enabled, Icons.AutoMirrored.Filled.Subject)
            SettingsDivider()
            FileSystemChoice(config.fsType, mountEnabled) {
                onConfigChanged(config.copy(fsType = it))
            }
            SettingsDivider()
            SettingSwitch("OverlayFS", stringResource(R.string.kasumi_backend_enable_desc), config.overlayfsEnabled,
                { onConfigChanged(config.copy(overlayfsEnabled = it)) }, mountEnabled, Icons.Filled.Layers)
            SettingsDivider()
            SettingSwitch("Magic Mount", stringResource(R.string.kasumi_backend_enable_desc), config.magicMountEnabled,
                { onConfigChanged(config.copy(magicMountEnabled = it)) }, mountEnabled, Icons.Filled.AutoFixHigh)
            SettingsDivider()
            MountSourceInput(config.mountsource, mountEnabled) { onConfigChanged(config.copy(mountsource = it)) }
            SettingsDivider()
            EditableConfigPath(R.string.kasumi_tempdir, R.string.kasumi_tempdir_desc, config.tempdir, mountEnabled, true) {
                onConfigChanged(config.copy(tempdir = it))
            }
            SettingsDivider()
            EditableConfigPath(R.string.kasumi_mirror_path, R.string.kasumi_mirror_path_desc, config.mirrorPath, mountEnabled, true,
                MoreSettingsItemPosition.Last) {
                onConfigChanged(config.copy(mirrorPath = it))
            }
        }
        ConfigSection(stringResource(R.string.kasumi_advanced), segmented = true) {
            SettingSwitch(stringResource(R.string.kasumi_enable_title), stringResource(R.string.kasumi_enable_desc), config.kasumiEnabled,
                { onConfigChanged(config.copy(kasumiEnabled = it)) }, kernelEnabled,
                Icons.Filled.Memory, MoreSettingsItemPosition.First)
            SettingsDivider()
            SettingSwitch(stringResource(R.string.kasumi_kernel_debug), stringResource(R.string.kasumi_kernel_debug_desc), config.enableKernelDebug,
                { onConfigChanged(config.copy(enableKernelDebug = it)) }, kernelEnabled, Icons.Filled.BugReport)
            SettingsDivider()
            SettingSwitch(stringResource(R.string.kasumi_stealth), stringResource(R.string.kasumi_stealth_desc), config.enableStealth,
                { onConfigChanged(config.copy(enableStealth = it)) }, kernelEnabled, Icons.Filled.VisibilityOff)
            val toggles = listOf(
                FeatureToggle("mount_hide", R.string.kasumi_mount_hide, R.string.kasumi_mount_hide_desc, config.enableMountHide) { config.copy(enableMountHide = it) },
                FeatureToggle("maps_spoof", R.string.kasumi_maps_spoof, R.string.kasumi_maps_spoof_desc, config.enableMapsSpoof) { config.copy(enableMapsSpoof = it) },
                FeatureToggle("overlay_xattr_hide", R.string.kasumi_overlay_xattr, R.string.kasumi_overlay_xattr_desc, config.enableOverlayXattrHide) { config.copy(enableOverlayXattrHide = it) },
                FeatureToggle("statfs_spoof", R.string.kasumi_statfs_spoof, R.string.kasumi_statfs_spoof_desc, config.enableStatfsSpoof) { config.copy(enableStatfsSpoof = it) },
            )
            toggles.forEach { toggle ->
                SettingsDivider()
                val supported = features?.names?.contains(toggle.capability) == true
                SettingSwitch(stringResource(toggle.title),
                    stringResource(when {
                        supported -> toggle.subtitle
                        toggle.checked -> R.string.kasumi_disable_unsupported
                        else -> R.string.feature_status_unsupported_summary
                    }),
                    toggle.checked, { onConfigChanged(toggle.changed(it)) }, enabled && ((kernelEnabled && supported) || (!supported && toggle.checked)),
                    icon = when (toggle.capability) {
                        "mount_hide" -> Icons.Filled.FolderOff
                        "maps_spoof" -> Icons.Filled.Memory
                        "statfs_spoof" -> Icons.Filled.Storage
                        else -> Icons.Filled.Security
                    },
                    position = if (toggle == toggles.last() && !runtimeApplyPending) MoreSettingsItemPosition.Last else MoreSettingsItemPosition.Middle)
            }
            if (runtimeApplyPending) {
                SettingsDivider()
                KasumiControlGroup(MoreSettingsItemPosition.Last) {
                    Text(stringResource(R.string.kasumi_apply_failed), color = MaterialTheme.colorScheme.error)
                    OutlinedButton(onClick = onRetryApply, enabled = kernelEnabled, modifier = Modifier.fillMaxWidth()) {
                        YukiIcon(Icons.Filled.Refresh, null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.kasumi_retry_apply))
                    }
                }
            }
        }
        UserHideRulesCard(enabled, features?.names?.contains("rule_bypass") == true, snackbarHostState,
            hideDraftPath, hideDraftBypass, onHideDraftPathChange, onHideDraftBypassChange)
        if (config.kernelAvailable && features?.names?.contains("maps_spoof") == true) MapsSpoofCard(snackbarHostState)

    }
}

@Composable
private fun FileSystemChoice(value: String, enabled: Boolean, onSelect: (String) -> Unit) {
    val options = listOf(
        "auto" to R.string.kasumi_fs_type_auto,
        "ext4" to R.string.kasumi_fs_type_ext4,
        "erofs" to R.string.kasumi_fs_type_erofs,
        "tmpfs" to R.string.kasumi_fs_type_tmpfs,
    )
    KasumiControlGroup(MoreSettingsItemPosition.Middle) {
        Text(
            stringResource(R.string.kasumi_fs_type_title),
            style = if (isExpressiveUi) MaterialTheme.typography.titleMedium else MaterialTheme.typography.bodyLarge,
        )
        Spacer(Modifier.height(4.dp))
        if (isExpressiveUi) {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                options.forEach { (option, title) ->
                    FilterChip(
                        selected = value == option,
                        onClick = { onSelect(option) },
                        enabled = enabled,
                        label = { Text(stringResource(title)) },
                        leadingIcon = if (value == option) {
                            { YukiIcon(Icons.Filled.Check, null, Modifier.size(FilterChipDefaults.IconSize)) }
                        } else null,
                    )
                }
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                options.forEach { (option, title) ->
                    FilledTonalButton(
                        onClick = { onSelect(option) },
                        modifier = Modifier.fillMaxWidth(),
                        enabled = enabled,
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = if (value == option) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant,
                            contentColor = if (value == option) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                        ),
                    ) { Text(stringResource(title)) }
                }
            }
        }
    }
}

private data class FeatureToggle(val capability: String, val title: Int, val subtitle: Int, val checked: Boolean,
    val changed: (Boolean) -> Controller.KagamiConfig)

@Composable
internal fun ConfigSection(title: String, segmented: Boolean = false, content: @Composable ColumnScope.() -> Unit) {
    if (isExpressiveUi) {
        Column(Modifier.fillMaxWidth()) {
            if (title.isNotEmpty()) {
                Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(start = 14.dp, top = 8.dp, bottom = 12.dp))
            }
            if (segmented) content() else SettingsControlGroup(MoreSettingsItemPosition.Only, content)
        }
        return
    }
    Card(Modifier.fillMaxWidth(), shape = kasumiCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow), elevation = getCardElevation()) {
        Column(Modifier.padding(16.dp)) {
            if (title.isNotEmpty()) Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(bottom = 12.dp))
            content()
        }
    }
}

@Composable
private fun SettingsDivider() {
    if (!isExpressiveUi) HorizontalDivider(Modifier.padding(vertical = 8.dp))
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ConfigChoice(title: String, value: String, values: List<String>, enabled: Boolean,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Only, selected: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    val labels = mapOf("auto" to stringResource(R.string.kasumi_mount_mode_auto), "kasumi" to "Kasumi",
        "overlay" to "OverlayFS", "magic" to "Magic Mount", "none" to stringResource(R.string.kasumi_mount_mode_none),
        "magisk" to "Magisk",
        "all" to stringResource(R.string.kasumi_filter_all), "kasumi-active" to stringResource(R.string.kasumi_filter_active))
    KasumiControlGroup(position) {
        Text(title, style = if (isExpressiveUi) MaterialTheme.typography.titleMedium else MaterialTheme.typography.bodyLarge)
        Spacer(Modifier.height(4.dp))
        ExposedDropdownMenuBox(expanded = expanded && enabled, onExpandedChange = { if (enabled) expanded = it }) {
            OutlinedTextField(value = labels[value] ?: value, onValueChange = {}, readOnly = true, enabled = enabled,
                modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
                trailingIcon = { YukiExpandArrow(expanded && enabled) })
            ExposedDropdownMenu(expanded = expanded && enabled, onDismissRequest = { expanded = false }) {
                values.forEach { choice -> DropdownMenuItem(text = { Text(labels[choice] ?: choice) },
                    onClick = { expanded = false; selected(choice) }) }
            }
        }
    }
}

@Composable
internal fun MountSourceInput(value: String, enabled: Boolean, onSave: (String) -> Unit) {
    var draft by remember(value) { mutableStateOf(value) }
    val changed = draft != value
    val valid = draft.isNotEmpty() && draft.none { it.isWhitespace() || it.isISOControl() || it == '\\' }

    fun save() {
        if (enabled && changed && valid) onSave(draft)
    }

    KasumiControlGroup(MoreSettingsItemPosition.Middle) {
        Text(
            stringResource(R.string.kasumi_mountsource),
            style = if (isExpressiveUi) MaterialTheme.typography.titleMedium else MaterialTheme.typography.bodyLarge,
        )
        Spacer(Modifier.height(4.dp))
        OutlinedTextField(
            value = draft,
            onValueChange = { draft = it },
            modifier = Modifier.fillMaxWidth(),
            enabled = enabled,
            singleLine = true,
            isError = changed && !valid,
            placeholder = { Text("KSU") },
            trailingIcon = if (changed) {
                {
                    IconButton(onClick = { save() }, enabled = enabled && valid) {
                        YukiIcon(Icons.Filled.Check, stringResource(R.string.kasumi_rule_save))
                    }
                }
            } else null,
            supportingText = if (changed && !valid) {
                { Text(stringResource(R.string.kasumi_mountsource_invalid)) }
            } else null,
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { save() }),
        )
    }
}

@Composable
private fun EditableConfigPath(title: Int, subtitle: Int, value: String, enabled: Boolean, allowEmpty: Boolean,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle, save: (String) -> Unit) {
    var draft by remember(value) { mutableStateOf(value) }
    var invalid by remember(value) { mutableStateOf(false) }
    SettingTextField(stringResource(title), stringResource(subtitle), draft, { draft = it; invalid = false }, {
        if (draft.startsWith('/') || (allowEmpty && draft.isEmpty())) save(draft)
        else invalid = true
    }, enabled, position = position)
    if (invalid) Text(stringResource(R.string.kasumi_absolute_path), color = MaterialTheme.colorScheme.error)
}

@Composable
private fun UserHideRulesCard(
    enabled: Boolean, bypassSupported: Boolean, snackbar: SnackbarController,
    newPath: String, newBypass: Boolean, onNewPathChange: (String) -> Unit, onNewBypassChange: (Boolean) -> Unit,
) {
    val scope = rememberCoroutineScope()
    val resources = LocalResources.current
    var states by remember { mutableStateOf(emptyList<Controller.ActiveRule>()) }
    val paths = states.map { it.src }
    var working by remember { mutableStateOf(false) }
    var draftOptionsExpanded by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        try { states = Controller.userHideStates() }
        catch (error: Exception) { if (error is CancellationException) throw error; snackbar.showSnackbar(error.message.orEmpty()) }
    }
    fun change(path: String, remove: Boolean, bypass: Boolean = false) {
        if (!enabled || working) return
        working = true
        scope.launch {
            try {
                val ok = if (remove) Controller.removeUserHideRule(path) else Controller.addUserHideRule(path, bypass)
                states = Controller.userHideStates()
                working = false
                if (ok) {
                    if (!remove && path == newPath.trim()) onNewPathChange("")
                } else snackbar.showSnackbar(resources.getString(R.string.operation_failed))
            } catch (error: Exception) { working = false; if (error is CancellationException) throw error; snackbar.showSnackbar(error.message.orEmpty()) }
            finally { working = false }
        }
    }
    fun refresh(path: String? = null) {
        if (working) return
        working = true
        scope.launch {
            try {
                val ok = when {
                    path == null -> true
                    states.find { it.src == path }?.hideState == -1 -> Controller.addUserHideRule(path, states.find { it.src == path }?.bypass == true)
                    else -> Controller.retryUserHide(path)
                }
                states = Controller.userHideStates()
                working = false
                if (!ok) snackbar.showSnackbar(resources.getString(R.string.operation_failed))
            } catch (error: Exception) {
                working = false
                if (error is CancellationException) throw error
                snackbar.showSnackbar(error.message.orEmpty())
            } finally { working = false }
        }
    }
    ConfigSection(stringResource(R.string.kasumi_user_hide_title), segmented = true) {
        KasumiControlGroup(MoreSettingsItemPosition.First) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.kasumi_quick_hide), Modifier.weight(1f), style = MaterialTheme.typography.titleSmall)
                IconButton(onClick = { refresh() }, enabled = !working) {
                    YukiIcon(Icons.Filled.Refresh, stringResource(R.string.kasumi_rules_refresh))
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val externalStorage = android.os.Environment.getExternalStorageDirectory()
                listOf("/dev/scene", "/dev/cpuset/scene-daemon", externalStorage.resolve("Download/advanced").path, externalStorage.resolve("MT2").path).forEach { path ->
                    AssistChip(onClick = { change(path, false) }, enabled = enabled && !working && path !in paths, label = { Text(path) })
                }
            }
        }
        states.forEach { rule -> key(rule.src) {
            var expanded by rememberSaveable { mutableStateOf(false) }
            val expansionState = stringResource(if (expanded) R.string.motion_expanded else R.string.motion_collapsed)
            KasumiControlGroup(MoreSettingsItemPosition.Middle) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Row(
                        Modifier.weight(1f).heightIn(min = 56.dp).clip(MaterialTheme.shapes.medium)
                            .semantics { stateDescription = expansionState }
                            .clickable(role = Role.Button, onClickLabel = hideOptionsAction(expanded)) { expanded = !expanded }
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(rule.src, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace,
                                maxLines = 2, overflow = TextOverflow.Ellipsis)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (rule.hideState != null) Text(userHideStatus(rule), style = MaterialTheme.typography.bodySmall,
                                    color = if (rule.hideState == 3) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
                                if (rule.bypass) Text(stringResource(R.string.kasumi_hide_scope_global),
                                    style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                            }
                        }
                        HideOptionsArrow(expanded)
                    }
                    IconButton(onClick = { change(rule.src, true) }, enabled = enabled && !working) {
                        YukiIcon(Icons.Filled.Delete, stringResource(R.string.kasumi_user_hide_remove))
                    }
                }
                HideOptionsExpansion(expanded) {
                    KasumiBypassSwitch(rule.bypass, { change(rule.src, false, it) }, enabled && !working && (bypassSupported || rule.bypass))
                    if (rule.hideState == 3 || rule.hideState == 0 || rule.hideState == -1) {
                        TextButton(onClick = { refresh(rule.src) }, enabled = enabled && !working) {
                            YukiIcon(Icons.Filled.Refresh, null, Modifier.size(18.dp))
                            Spacer(Modifier.width(8.dp))
                            Text(stringResource(R.string.kasumi_hide_retry))
                        }
                    }
                }
            }
        } }
        KasumiControlGroup(MoreSettingsItemPosition.Last) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(newPath, onNewPathChange, Modifier.weight(1f), enabled = enabled && !working,
                    singleLine = true, placeholder = { Text(stringResource(R.string.kasumi_user_hide_placeholder)) })
                Button(onClick = { change(newPath.trim(), false, newBypass) }, enabled = enabled && !working && newPath.startsWith('/') && newPath != "/system/bin/su") {
                    Text(stringResource(R.string.kasumi_user_hide_add))
                }
            }
            val expansionState = stringResource(if (draftOptionsExpanded) R.string.motion_expanded else R.string.motion_collapsed)
            Row(
                Modifier.fillMaxWidth().heightIn(min = 48.dp).clip(MaterialTheme.shapes.medium)
                    .semantics { stateDescription = expansionState }
                    .clickable(role = Role.Button, onClickLabel = hideOptionsAction(draftOptionsExpanded)) { draftOptionsExpanded = !draftOptionsExpanded }
                    .padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(stringResource(R.string.kasumi_hide_options), Modifier.weight(1f), style = MaterialTheme.typography.labelLarge)
                Text(stringResource(if (newBypass) R.string.kasumi_hide_scope_global else R.string.kasumi_hide_scope_policy),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (newBypass) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
                HideOptionsArrow(draftOptionsExpanded)
            }
            HideOptionsExpansion(draftOptionsExpanded) {
                KasumiBypassSwitch(newBypass, onNewBypassChange, enabled && !working && bypassSupported,
                    label = stringResource(R.string.kasumi_hide_draft_bypass))
                Text(stringResource(if (bypassSupported) R.string.kasumi_hide_draft_summary else R.string.kasumi_rule_bypass_unavailable),
                    Modifier.padding(horizontal = 8.dp, vertical = 4.dp), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun hideOptionsAction(expanded: Boolean): String =
    stringResource(if (expanded) R.string.kasumi_hide_collapse_options else R.string.kasumi_hide_expand_options)

@Composable
private fun HideOptionsArrow(expanded: Boolean) {
    YukiExpandArrow(expanded, tint = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun HideOptionsExpansion(expanded: Boolean, content: @Composable ColumnScope.() -> Unit) {
    YukiExpandableContent(visible = expanded) {
        Column(Modifier.fillMaxWidth().padding(top = 4.dp), content = content)
    }
}

@Composable
internal fun userHideStatus(rule: Controller.ActiveRule): String = when (rule.hideState) {
    -1 -> stringResource(R.string.kasumi_hide_saved)
    0 -> stringResource(R.string.kasumi_hide_pending)
    1 -> stringResource(R.string.kasumi_hide_binding)
    2 -> stringResource(R.string.kasumi_hide_bound)
    4 -> stringResource(R.string.kasumi_hide_suspended)
    else -> when (rule.hideError) {
        -13, -1 -> stringResource(R.string.kasumi_hide_permission)
        -95 -> stringResource(R.string.kasumi_hide_unsupported)
        else -> stringResource(R.string.kasumi_hide_error, -rule.hideError)
    }
}

@Composable
internal fun KernelHooksCard(hooks: String) {
    if (hooks.isEmpty()) return
    var expanded by rememberSaveable { mutableStateOf(false) }
    val expansionState = stringResource(if (expanded) R.string.motion_expanded else R.string.motion_collapsed)
    val expansionAction = stringResource(if (expanded) R.string.collapse_menu else R.string.expand_menu)
    Card(Modifier.fillMaxWidth(), shape = kasumiCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow), elevation = getCardElevation()) {
        Column(Modifier.padding(16.dp)) {
            Row(
                Modifier.fillMaxWidth().heightIn(min = 48.dp)
                    .semantics { stateDescription = expansionState }
                    .clickable(role = Role.Button, onClickLabel = expansionAction) { expanded = !expanded },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(stringResource(R.string.kasumi_kernel_hooks), Modifier.weight(1f), style = MaterialTheme.typography.titleMedium,
                    fontWeight = if (isExpressiveUi) FontWeight.Normal else FontWeight.Bold)
                YukiExpandArrow(expanded)
            }
            YukiExpandableContent(visible = expanded, large = true) {
                Text(hooks, Modifier.padding(top = 8.dp), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
