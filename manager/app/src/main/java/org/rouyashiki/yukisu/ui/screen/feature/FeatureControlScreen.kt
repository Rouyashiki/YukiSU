package org.rouyashiki.yukisu.ui.screen.feature

import org.rouyashiki.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import org.rouyashiki.yukisu.ui.component.YukiLargeTopAppBar
import android.util.Log
import android.widget.Toast
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.rounded.EnhancedEncryption
import androidx.compose.material.icons.rounded.RemoveCircle
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.vectorResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.component.YukiAlertDialog
import org.rouyashiki.yukisu.ui.component.YukiIcon
import org.rouyashiki.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import org.rouyashiki.yukisu.ui.util.rememberSnackbarController
import org.rouyashiki.yukisu.ui.util.getFeatureStatus
import org.rouyashiki.yukisu.ui.util.featureStatusReader
import org.rouyashiki.yukisu.ui.util.getFeatureValueOrNull
import org.rouyashiki.yukisu.ui.util.migrateFeatureDefaults
import org.rouyashiki.yukisu.ui.util.restartAdbd
import org.rouyashiki.yukisu.ui.util.setFeatureValue
import org.rouyashiki.yukisu.ui.util.withNewRootShell
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.rouyashiki.yukisu.ui.screen.SettingsItemPosition
import org.rouyashiki.yukisu.ui.screen.SwitchItem
import org.rouyashiki.yukisu.ui.screen.moreSettings.component.SettingsCard

internal data class FeatureReading(val value: Boolean?, val status: String)

internal fun featureQuery(
    id: Int,
    migrateDefaults: suspend () -> Boolean = { withNewRootShell { migrateFeatureDefaults(this) } },
    readStatus: suspend (Int) -> String = ::getFeatureStatus,
    readValue: (Int) -> Boolean? = ::getFeatureValueOrNull,
) = FeatureQuery(
    blocksWrites = false,
    onQueued = featureStatusReader::keepSessionFor,
    prepare = {
        if (id == Natives.FEATURE_DEFAULT_NO_NEW_PRIVS) {
            try {
                migrateDefaults()
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                Log.w("FeatureControl", "Failed to migrate feature defaults", error)
            }
        }
    },
) {
    val status = readStatus(id)
    FeatureReading(readValue(id), status)
}

private val featureQueries = listOf(
    Natives.FEATURE_SU_COMPAT, Natives.FEATURE_KASUMI_SUCOMPAT, Natives.FEATURE_MAGISK_COMPAT,
    Natives.FEATURE_KASUMI, Natives.FEATURE_SELINUX_HIDE, Natives.FEATURE_KERNEL_UMOUNT,
    Natives.FEATURE_UNSHARE_MNT, Natives.FEATURE_SULOG, Natives.FEATURE_ADB_ROOT,
    Natives.FEATURE_SHELL_SU_ADB_ONLY, Natives.FEATURE_DEFAULT_NO_NEW_PRIVS,
    Natives.FEATURE_YUKIZYGISK, Natives.FEATURE_HIDE_BOOTLOADER,
).associateWith { featureQuery(it) }

private val kasumiInitialization = FeatureQuery(blocksWrites = false) { Natives.kasumiIsInitialized() }

private fun readSuCompat(unsupported: Set<Int>, read: (Int) -> Boolean?): SuCompatSnapshot {
    fun readOnce(id: Int) = try {
        read(id)
    } catch (error: Exception) {
        Log.w("FeatureControl", "Failed to read su compatibility feature $id", error)
        null
    }
    fun value(id: Int) = readOnce(id) ?: if (id in unsupported) false else readOnce(id)
    return SuCompatSnapshot(
        value(Natives.FEATURE_SU_COMPAT),
        value(Natives.FEATURE_KASUMI_SUCOMPAT),
        value(Natives.FEATURE_MAGISK_COMPAT),
    )
}

internal class FeatureToggleState(initialChecked: Boolean?, initialStatus: String) {
    var checked by mutableStateOf(initialChecked)
    var status by mutableStateOf(initialStatus)
    var saving by mutableStateOf(false)
    val displayStatus get() = if (status == "supported" && checked == null) "unknown" else status
    val value get() = checked ?: false.takeIf { status == "unsupported" }
}

private val defaultSuCompatController = SuCompatController(::setFeatureValue)

@Composable
internal fun rememberFeatureToggleState(
    featureId: Int,
    read: suspend (Int) -> FeatureReading,
    displayInverted: Boolean = false,
): FeatureToggleState {
    val state = remember(featureId, displayInverted) { FeatureToggleState(null, "loading") }
    LaunchedEffect(state) {
        val reading = try {
            read(featureId)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            Log.w("FeatureControl", "Failed to read feature $featureId", error)
            FeatureReading(null, "unknown")
        }
        state.checked = reading.value?.let { if (displayInverted) !it else it }
        state.status = reading.status
    }
    return state
}

internal fun CoroutineScope.persistFeature(
    state: FeatureToggleState,
    featureId: Int,
    featureName: String,
    kernelEnabled: Boolean,
    displayInverted: Boolean = false,
    relatedStates: List<FeatureToggleState> = emptyList(),
    optimistic: Boolean = false,
    afterPersist: suspend () -> Boolean = { true },
    afterPostRollback: suspend () -> Unit = {},
    onRuntimeSettled: suspend () -> Unit = {},
    onSuccess: suspend () -> Unit = {},
    onFailure: suspend () -> Unit = {},
    readValue: suspend () -> Boolean? = { getFeatureValueOrNull(featureId) },
    writeValue: suspend (Boolean) -> Boolean = { setFeatureValue(featureName, it) },
) {
    val savingStates = relatedStates + state
    if (savingStates.any { it.saving }) return

    if (optimistic) {
        state.checked = if (displayInverted) !kernelEnabled else kernelEnabled
    }
    savingStates.forEach { it.saving = true }
    launch(start = CoroutineStart.UNDISPATCHED) {
        val success = withContext(NonCancellable) {
            suspend fun persist(): Boolean {
                try {
                    val previous = readValue() ?: return false
                    if (!writeValue(kernelEnabled)) return false
                    if (afterPersist()) return true
                    writeValue(previous)
                    afterPostRollback()
                } catch (error: Exception) {
                    Log.w("FeatureControl", "Failed to update $featureName", error)
                }
                return false
            }
            suspend fun readChecked(): Boolean? = try {
                readValue()?.let { if (displayInverted) !it else it }
            } catch (error: Exception) {
                Log.w("FeatureControl", "Failed to read $featureName", error)
                null
            }
            try {
                val (saved, checked) = featureReads.change {
                    val saved = persist()
                    val checked = readChecked() ?: run {
                        Snapshot.withMutableSnapshot { state.checked = null }
                        readChecked()
                    }
                    saved to checked
                }
                state.checked = checked
                try {
                    onRuntimeSettled()
                } catch (error: Exception) {
                    Log.w("FeatureControl", "Failed to refresh $featureName", error)
                }
                saved
            } finally {
                savingStates.forEach { it.saving = false }
            }
        }
        if (success) onSuccess() else onFailure()
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun FeatureControlScreen(navigator: DestinationsNavigator) {
    LaunchedEffect(Unit) { featureStatusReader.withSession { awaitCancellation() } }
    FeatureControlPage(onBack = { navigator.popBackStack() }, load = featureReads::isManager)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun FeatureControlPage(
    onBack: () -> Unit,
    load: suspend () -> Boolean,
    readFeature: suspend (Int) -> FeatureReading = { featureReads.read(featureQueries.getValue(it)) },
    initialization: FeatureQuery<Boolean> = kasumiInitialization,
    suCompatController: SuCompatController = defaultSuCompatController,
    readSuCompatValue: (Int) -> Boolean? = ::getFeatureValueOrNull,
) {
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbarHost = rememberSnackbarController()

    val loaded by produceState<Result<Boolean>?>(initialValue = null) {
        value = try {
            Result.success(load())
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            Log.w("FeatureControl", "Failed to query features", error)
            Result.failure(error)
        }
    }
    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            FeatureControlTopBar(
                onBack = onBack,
                scrollBehavior = scrollBehavior
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        )
    ) { paddingValues ->
        if (loaded?.getOrNull() != true) {
            Box(Modifier.fillMaxSize().padding(paddingValues), contentAlignment = Alignment.Center) {
                when {
                    loaded == null -> CircularProgressIndicator()
                    loaded?.isFailure == true -> Text(stringResource(R.string.operation_failed))
                    else -> Text(stringResource(R.string.feature_status_unsupported_summary))
                }
            }
        } else {
            val selinuxHide = rememberFeatureToggleState(Natives.FEATURE_SELINUX_HIDE, readFeature)
            val traditional = rememberFeatureToggleState(Natives.FEATURE_SU_COMPAT, readFeature)
            val ksm = rememberFeatureToggleState(Natives.FEATURE_KASUMI_SUCOMPAT, readFeature)
            val magiskCompat = rememberFeatureToggleState(Natives.FEATURE_MAGISK_COMPAT, readFeature)
            val suMode = SuCompatSnapshot(traditional.value, ksm.value, magiskCompat.value).mode
            val suCompatKnown = suMode != null && magiskCompat.value != null
            fun unsupportedSuCompatFeatures() = buildSet {
                if (traditional.status == "unsupported") add(Natives.FEATURE_SU_COMPAT)
                if (ksm.status == "unsupported") add(Natives.FEATURE_KASUMI_SUCOMPAT)
                if (magiskCompat.status == "unsupported") add(Natives.FEATURE_MAGISK_COMPAT)
            }
            fun applySuCompat(values: SuCompatSnapshot) {
                traditional.checked = values.traditional
                ksm.checked = values.ksm
                magiskCompat.checked = values.magisk
            }
            var requestedSuCompat by remember { mutableStateOf<SuCompatMode?>(null) }
            var suPathSaving by remember { mutableStateOf(false) }
            var suPathRevision by remember { mutableIntStateOf(0) }
            val suCompatStatus = traditional.displayStatus
            val ksmStatus = ksm.displayStatus
            val suCompatForced = suCompatStatus == "forced"
            val suCompatSupported = suCompatStatus == "supported"
            val ksmSupported = ksmStatus == "supported"
            val kasumi = rememberFeatureToggleState(Natives.FEATURE_KASUMI, readFeature)
            var kasumiInitialized by remember { mutableStateOf(false) }
            LaunchedEffect(Unit) {
                try {
                    kasumiInitialized = featureReads.read(initialization)
                } catch (error: CancellationException) {
                    throw error
                } catch (error: Exception) {
                    Log.w("FeatureControl", "Failed to read Kasumi initialization", error)
                }
            }
            val kasumiAvailable = kasumi.status == "unsupported" || (kasumi.checked == true && kasumiInitialized)
            val kernelUmountDisabled = rememberFeatureToggleState(
                Natives.FEATURE_KERNEL_UMOUNT,
                readFeature,
                displayInverted = true
            )
            val unshareMnt = rememberFeatureToggleState(Natives.FEATURE_UNSHARE_MNT, readFeature)
            val suLog = rememberFeatureToggleState(Natives.FEATURE_SULOG, readFeature)
            val adbRoot = rememberFeatureToggleState(Natives.FEATURE_ADB_ROOT, readFeature)
            val shellSuAdbOnly = rememberFeatureToggleState(Natives.FEATURE_SHELL_SU_ADB_ONLY, readFeature)
            var showMagiskUmountWarning by rememberSaveable { mutableStateOf(false) }
            val defaultNoNewPrivs = rememberFeatureToggleState(Natives.FEATURE_DEFAULT_NO_NEW_PRIVS, readFeature)
            val yukiZygisk = rememberFeatureToggleState(Natives.FEATURE_YUKIZYGISK, readFeature)
            val hideBootloader = rememberFeatureToggleState(Natives.FEATURE_HIDE_BOOTLOADER, readFeature)

            val savedRebootMessage = stringResource(R.string.setting_change_saved_reboot)
            val failedMessage = stringResource(R.string.setting_change_failed)
            val yukiZygiskEnabledMessage = stringResource(R.string.settings_yukizygisk_toast_on)
            val yukiZygiskDisabledMessage = stringResource(R.string.settings_yukizygisk_toast_off)
            val yukiZygiskFailedMessage = stringResource(R.string.settings_yukizygisk_toast_failed)

            fun updateSuCompat(mode: SuCompatMode? = null, magisk: Boolean? = null) {
                if (magiskCompat.saving || kasumi.saving || suPathSaving) return
                val unsupported = unsupportedSuCompatFeatures()
                magiskCompat.saving = true
                requestedSuCompat = mode
                scope.launch(start = CoroutineStart.UNDISPATCHED) {
                    val success = withContext(NonCancellable) {
                        try {
                            val (saved, current, warn) = featureReads.change {
                                val (saved, confirmed) = suCompatController.change(mode ?: SuCompatMode.KSM, magisk) {
                                    readSuCompat(unsupported, readSuCompatValue)
                                }
                                val warn = saved && magisk == true && Natives.isDefaultUmountModules()
                                Triple(saved, confirmed, warn)
                            }
                            applySuCompat(current)
                            if (warn) showMagiskUmountWarning = true
                            saved
                        } catch (error: Exception) {
                            Log.w("FeatureControl", "Failed to update su compatibility", error)
                            false
                        } finally {
                            magiskCompat.saving = false
                            requestedSuCompat = null
                        }
                    }
                    if (!success) snackbarHost.showSnackbar(failedMessage)
                }
            }

            if (showMagiskUmountWarning) {
                YukiAlertDialog(
                    onDismissRequest = { showMagiskUmountWarning = false },
                    icon = { Icon(Icons.Filled.WarningAmber, contentDescription = null) },
                    title = { Text(stringResource(R.string.su_compat_magisk_title)) },
                    text = {
                        Text(
                            stringResource(
                                R.string.su_compat_magisk_umount_warning,
                                stringResource(R.string.settings_umount_modules_default),
                                stringResource(R.string.profile_umount_modules),
                            )
                        )
                    },
                    confirmButton = {
                        TextButton(onClick = { showMagiskUmountWarning = false }) {
                            Text(stringResource(R.string.confirm))
                        }
                    },
                )
            }

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp)
                    .padding(top = 8.dp)
            ) {
                SettingsCard(title = stringResource(R.string.kernelsu_features)) {
                    FeatureSwitchItem(
                        icon = Icons.Filled.Security,
                        title = stringResource(R.string.settings_selinux_hide),
                        summary = stringResource(R.string.settings_selinux_hide_summary),
                        state = selinuxHide,
                        groupPosition = SettingsItemPosition.First,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = selinuxHide,
                                featureId = Natives.FEATURE_SELINUX_HIDE,
                                featureName = "selinux_hide",
                                kernelEnabled = enabled,
                                onSuccess = { snackbarHost.showSnackbar(savedRebootMessage) },
                                onFailure = { snackbarHost.showSnackbar(failedMessage) }
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Rounded.RemoveCircle,
                        title = stringResource(R.string.settings_disable_kernel_umount),
                        summary = stringResource(R.string.settings_disable_kernel_umount_summary),
                        state = kernelUmountDisabled,
                        onChange = { disabled ->
                            scope.persistFeature(
                                state = kernelUmountDisabled,
                                featureId = Natives.FEATURE_KERNEL_UMOUNT,
                                featureName = "kernel_umount",
                                kernelEnabled = !disabled,
                                displayInverted = true
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.Visibility,
                        title = stringResource(R.string.settings_disable_sulog),
                        summary = stringResource(R.string.settings_disable_sulog_summary),
                        state = suLog,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = suLog,
                                featureId = Natives.FEATURE_SULOG,
                                featureName = "sulog",
                                kernelEnabled = enabled
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Rounded.EnhancedEncryption,
                        title = stringResource(R.string.settings_shell_su_adb_only),
                        summary = stringResource(R.string.settings_shell_su_adb_only_summary),
                        state = shellSuAdbOnly,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = shellSuAdbOnly,
                                featureId = Natives.FEATURE_SHELL_SU_ADB_ONLY,
                                featureName = "shell_su_adb_only",
                                kernelEnabled = enabled,
                                onFailure = { snackbarHost.showSnackbar(failedMessage) }
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.DeveloperMode,
                        title = stringResource(R.string.settings_adb_root),
                        summary = stringResource(R.string.settings_adb_root_summary),
                        state = adbRoot,
                        groupPosition = SettingsItemPosition.Last,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = adbRoot,
                                featureId = Natives.FEATURE_ADB_ROOT,
                                featureName = "adb_root",
                                kernelEnabled = enabled,
                                afterPersist = {
                                    withContext(Dispatchers.IO) { restartAdbd() }
                                },
                                afterPostRollback = {
                                    withContext(Dispatchers.IO) { restartAdbd() }
                                },
                                onFailure = {
                                    snackbarHost.showSnackbar(failedMessage)
                                }
                            )
                        }
                    )
                }

                SettingsCard(title = stringResource(R.string.yukisu_features)) {
                    FeatureSwitchItem(
                        icon = ImageVector.vectorResource(R.drawable.ic_mount_view_cleanup),
                        title = stringResource(R.string.settings_unshare_mnt),
                        summary = stringResource(R.string.settings_unshare_mnt_summary),
                        state = unshareMnt,
                        groupPosition = SettingsItemPosition.First,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = unshareMnt,
                                featureId = Natives.FEATURE_UNSHARE_MNT,
                                featureName = "unshare_mnt",
                                kernelEnabled = enabled,
                                onFailure = { snackbarHost.showSnackbar(failedMessage) }
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.Layers,
                        title = stringResource(R.string.kasumi_feature_title),
                        summary = stringResource(R.string.kasumi_feature_summary),
                        state = kasumi,
                        enabled = !magiskCompat.saving && !suPathSaving,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = kasumi,
                                featureId = Natives.FEATURE_KASUMI,
                                featureName = "kasumi",
                                kernelEnabled = enabled,
                                relatedStates = listOf(magiskCompat),
                                onRuntimeSettled = {
                                    val unsupported = unsupportedSuCompatFeatures()
                                    val (initialized, current) = withContext(Dispatchers.IO) {
                                        initialization.load() to readSuCompat(unsupported, readSuCompatValue)
                                    }
                                    kasumiInitialized = initialized
                                    applySuCompat(current)
                                    suPathRevision++
                                },
                                onFailure = { snackbarHost.showSnackbar(failedMessage) },
                            )
                        },
                    )

                    SuCompatSelector(
                        selected = requestedSuCompat ?: suMode,
                        loading = suCompatStatus == "loading" || ksmStatus == "loading",
                        forced = suCompatForced,
                        managed = suCompatStatus == "managed",
                        enabled = suCompatKnown && suCompatSupported && !magiskCompat.saving && !kasumi.saving && !suPathSaving,
                        ksmSupported = ksmSupported && kasumiAvailable,
                        onSelect = { mode ->
                            if (mode != suMode) updateSuCompat(mode = mode)
                        },
                    )

                    if (ksmSupported) {
                        SuPathSetting(
                            enabled = !suCompatForced && kasumiAvailable && suMode == SuCompatMode.KSM &&
                                !magiskCompat.saving && !kasumi.saving,
                            refreshKey = suPathRevision,
                            onSavingChange = { suPathSaving = it },
                        )
                    }

                    FeatureSwitchItem(
                        icon = Icons.Filled.Security,
                        title = stringResource(R.string.su_compat_magisk_title),
                        summary = stringResource(R.string.su_compat_magisk_summary),
                        state = magiskCompat,
                        enabled = !suCompatForced && kasumiAvailable && suMode == SuCompatMode.KSM &&
                            !kasumi.saving && !suPathSaving,
                        onChange = { enabled -> updateSuCompat(magisk = enabled) },
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.FrontHand,
                        title = stringResource(R.string.settings_default_no_new_privs),
                        summary = stringResource(R.string.settings_default_no_new_privs_summary),
                        state = defaultNoNewPrivs,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = defaultNoNewPrivs,
                                featureId = Natives.FEATURE_DEFAULT_NO_NEW_PRIVS,
                                featureName = "default_no_new_privs",
                                kernelEnabled = enabled
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.Extension,
                        title = stringResource(R.string.settings_yukizygisk),
                        summary = stringResource(R.string.settings_yukizygisk_summary),
                        state = yukiZygisk,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = yukiZygisk,
                                featureId = Natives.FEATURE_YUKIZYGISK,
                                featureName = "yukizygisk",
                                kernelEnabled = enabled,
                                optimistic = true,
                                onSuccess = {
                                    snackbarHost.showSnackbar(
                                        if (enabled) {
                                            yukiZygiskEnabledMessage
                                        } else {
                                            yukiZygiskDisabledMessage
                                        }
                                    )
                                },
                                onFailure = {
                                    snackbarHost.showSnackbar(yukiZygiskFailedMessage)
                                }
                            )
                        }
                    )

                    FeatureSwitchItem(
                        icon = Icons.Filled.Lock,
                        title = stringResource(R.string.hide_bl_title),
                        summary = when (hideBootloader.checked) {
                            true -> stringResource(R.string.hide_bl_enabled)
                            false -> stringResource(R.string.hide_bl_disabled)
                            null -> null
                        },
                        state = hideBootloader,
                        groupPosition = SettingsItemPosition.Last,
                        onChange = { enabled ->
                            scope.persistFeature(
                                state = hideBootloader,
                                featureId = Natives.FEATURE_HIDE_BOOTLOADER,
                                featureName = "hide_bootloader",
                                kernelEnabled = enabled,
                                onSuccess = {
                                    Toast.makeText(
                                        context,
                                        if (enabled) {
                                            R.string.hide_bl_enabled_toast
                                        } else {
                                            R.string.hide_bl_disabled_toast
                                        },
                                        Toast.LENGTH_SHORT
                                    ).show()
                                },
                                onFailure = {
                                    Toast.makeText(
                                        context,
                                        R.string.hide_bl_change_failed,
                                        Toast.LENGTH_SHORT
                                    ).show()
                                }
                            )
                        }
                    )
                }

                Spacer(modifier = Modifier.height(8.dp))
            }
        }
    }
}

@Composable
internal fun FeatureSwitchItem(
    icon: ImageVector,
    title: String,
    summary: String?,
    state: FeatureToggleState,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    enabled: Boolean = true,
    onChange: (Boolean) -> Unit
) {
    val status = state.displayStatus
    val renderedSummary = when (status) {
        "unsupported" -> stringResource(R.string.feature_status_unsupported_summary)
        "managed" -> stringResource(R.string.feature_status_managed_summary)
        "unknown" -> stringResource(R.string.feature_state_unavailable)
        "forced" -> stringResource(R.string.feature_status_forced_summary)
        else -> summary
    }

    SwitchItem(
        icon = icon,
        title = title,
        summary = renderedSummary,
        checked = state.checked,
        enabled = enabled && status == "supported" && !state.saving,
        loading = status == "loading" || (state.saving && state.checked == null),
        groupPosition = groupPosition,
        onCheckedChange = onChange
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FeatureControlTopBar(
    onBack: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior
) {
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.feature_control),
            fontWeight = FontWeight.Normal
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back)
            )
        }
    }
    val colors = getTopAppBarColors(
        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow
    )
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    YukiLargeTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        colors = colors,
        windowInsets = windowInsets,
        scrollBehavior = scrollBehavior
    )
}
