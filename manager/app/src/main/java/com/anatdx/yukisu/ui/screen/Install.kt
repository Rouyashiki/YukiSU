package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DeveloperMode
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.net.toUri
import com.maxkeppeker.sheets.core.models.base.Header
import com.maxkeppeker.sheets.core.models.base.rememberUseCaseState
import com.maxkeppeler.sheets.list.ListDialog
import com.maxkeppeler.sheets.list.models.ListOption
import com.maxkeppeler.sheets.list.models.ListSelection
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.PartitionManagerScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.navigation.EmptyDestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.magica.MagicaHelper
import com.anatdx.yukisu.superkey.SuperKeyHelper
import com.anatdx.yukisu.ui.component.DialogHandle
import com.anatdx.yukisu.ui.component.SuperDropdown
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiDialogTheme
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.component.rememberCustomDialog
import com.anatdx.yukisu.ui.component.rememberLoadingDialog
import com.anatdx.yukisu.ui.component.performClickHapticFeedback
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.CardConfig.cardElevation
import com.anatdx.yukisu.ui.theme.ThemeConfig
import com.anatdx.yukisu.ui.theme.ThemeColors
import com.anatdx.yukisu.ui.theme.ThemeManager
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * @author ShirkNeko
 * @date 2025/5/31.
 */

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun InstallScreen(
    navigator: DestinationsNavigator,
    utsBootRepatch: Boolean = false,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val coroutineScope = rememberCoroutineScope()
    val loadingDialog = rememberLoadingDialog()
    var installMethod by remember { mutableStateOf<InstallMethod?>(null) }
    var lkmSelection by remember { mutableStateOf<LkmSelection>(LkmSelection.KmiNone) }
    var isResolvingKmi by remember { mutableStateOf(false) }
    var showRebootDialog by remember { mutableStateOf(false) }
    var showDownloadDialog by remember { mutableStateOf(false) }
    var downloadUrl by remember { mutableStateOf("") }
    var remotePartitions by remember { mutableStateOf<List<String>>(emptyList()) }
    var remotePartitionSelectionIndex by remember { mutableIntStateOf(0) }

    val seLinuxStatus by produceState(initialValue = resources.getString(R.string.selinux_status_unknown)) {
        value = withContext(Dispatchers.IO) {
            runCatching { getSELinuxStatus(context) }
                .getOrDefault(resources.getString(R.string.selinux_status_unknown))
        }
    }
    val isManager by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) {
            runCatching { Natives.isManager }.getOrDefault(false)
        }
    }
    val isRootShellAvailable by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) {
            runCatching { rootAvailable() }.getOrDefault(false)
        }
    }
    val isSelinuxPermissive = seLinuxStatus == resources.getString(R.string.selinux_status_permissive)
    val canJailbreakInstall = !isManager && isSelinuxPermissive
    val onJailbreakInstall: () -> Unit = {
        loadingDialog.show()
        coroutineScope.launch {
            val launched = if (isRootShellAvailable) {
                withContext(Dispatchers.IO) {
                    execKsud("late-load", true)
                }
            } else {
                MagicaHelper.launch(context, "install-manual")
            }

            if (!launched) {
                loadingDialog.hide()
                Toast.makeText(context, R.string.install_jailbreak_failed, Toast.LENGTH_LONG).show()
                return@launch
            }

            delay(30_000)
            loadingDialog.hide()
            Toast.makeText(context, R.string.jailbreak_timeout, Toast.LENGTH_LONG).show()
        }
    }

    if (showRebootDialog) {
        RebootDialog(
            show = true,
            onDismiss = { showRebootDialog = false },
            onConfirm = {
                showRebootDialog = false
                reboot()
            }
        )
    }

    var partitionSelectionIndex by remember { mutableIntStateOf(0) }
    var partitionsState by remember { mutableStateOf<List<String>>(emptyList()) }
    var hasCustomSelected by remember { mutableStateOf(false) }
    
    // SuperKey for APatch-style authentication.
    // If we've already saved one (previous patch), prefill with a `********`
    // placeholder so the user can re-patch with the same key by just hitting
    // next; any keystroke clears it and falls back to manual input.
    val savedSuperKey = remember {
        SuperKeyHelper.getSavedSuperKey(context)
    }
    var usingSavedKey by remember { mutableStateOf(savedSuperKey != null) }
    var superKey by remember { mutableStateOf("") }
    val effectiveSuperKey = if (usingSavedKey) savedSuperKey.orEmpty() else superKey
    var showSuperKeyInput by remember { mutableStateOf(false) }
    // Signature bypass - when enabled, only SuperKey authentication works
    var signatureBypass by remember { mutableStateOf(false) }
    var allowShell by remember { mutableStateOf(false) }
    var advancedOptionsExpanded by rememberSaveable { mutableStateOf(false) }
    var noInstall by rememberSaveable { mutableStateOf(false) }
    var noPreinit by rememberSaveable { mutableStateOf(false) }
    var noCustomRc by rememberSaveable { mutableStateOf(false) }
    var forcedFeatures by rememberSaveable { mutableStateOf(emptyList<String>()) }
    var blockModulesEnabled by rememberSaveable { mutableStateOf(false) }
    var blockModules by rememberSaveable { mutableStateOf("") }
    var blockModulesInitialized by rememberSaveable { mutableStateOf(false) }
    val normalizedBlockModules = normalizeBlockModules(if (blockModulesEnabled) blockModules else "")
    LaunchedEffect(Unit) {
        if (!blockModulesInitialized) {
            val loaded = getLoadedBlockableModules().joinToString(",")
            if (!blockModulesInitialized) {
                blockModules = loaded
                blockModulesInitialized = true
            }
        }
    }
    var enableAdb by remember { mutableStateOf(false) }
    var forceBackup by remember { mutableStateOf(false) }
    val embedLkmInBootByDefault = remember {
        runCatching { Natives.getLoadMode() == Natives.LOAD_MODE_IMAGE_PATCH }
            .getOrDefault(false)
    }
    var embedLkmInBoot by rememberSaveable {
        mutableStateOf(embedLkmInBootByDefault)
    }

    val configOnly = noInstall && installMethod is InstallMethod.SelectFile && !embedLkmInBoot
    val bootConfig = BootConfigOptions(configOnly, noPreinit, noCustomRc, forcedFeatures)
    val directKernelMethod = installMethod is InstallMethod.SelectFile ||
        installMethod is InstallMethod.DirectInstall ||
        installMethod is InstallMethod.DirectInstallToInactiveSlot

    LaunchedEffect(utsBootRepatch) {
        if (utsBootRepatch) {
            if (
                installMethod !is InstallMethod.DirectInstall &&
                installMethod !is InstallMethod.DirectInstallToInactiveSlot
            ) {
                installMethod = null
            }
            if (lkmSelection is LkmSelection.LkmUri) {
                lkmSelection = LkmSelection.KmiNone
            }
            hasCustomSelected = false
            partitionSelectionIndex = 0
            forceBackup = false
            embedLkmInBoot = embedLkmInBootByDefault
        }
    }

    LaunchedEffect(installMethod) {
        noInstall = false
        embedLkmInBoot = directKernelMethod && embedLkmInBootByDefault
    }

    val onInstall: (InstallMethod, LkmSelection, String) -> Unit =
        onInstall@ { method, selectedLkm, targetKmi ->
            val modules = normalizedBlockModules ?: return@onInstall
            if (
                utsBootRepatch &&
                method !is InstallMethod.DirectInstall &&
                method !is InstallMethod.DirectInstallToInactiveSlot
            ) {
                return@onInstall
            }
            val isOta = method is InstallMethod.DirectInstallToInactiveSlot
            if (embedLkmInBoot &&
                (method is InstallMethod.SelectFile ||
                    method is InstallMethod.DirectInstall ||
                    method is InstallMethod.DirectInstallToInactiveSlot)
            ) {
                val selectedBoot = (method as? InstallMethod.SelectFile)?.uri
                if (method is InstallMethod.SelectFile && selectedBoot == null) {
                    return@onInstall
                }
                navigator.navigate(
                    FlashScreenDestination(
                        FlashIt.FlashBoot(
                            boot = selectedBoot,
                            lkm = selectedLkm,
                            targetKmi = "",
                            ota = isOta,
                            partition = "boot",
                            allowShell = allowShell,
                            blockModules = modules,
                            bootConfig = bootConfig,
                            enableAdb = enableAdb,
                            backup = false,
                            superKey = effectiveSuperKey.ifBlank { null },
                            signatureBypass = signatureBypass,
                            embedLkmInBoot = true,
                        )
                    )
                )
                return@onInstall
            }
            // An untouched dropdown only presents ksud's current partition
            // recommendation. Let ksud choose from the resolved KMI.
            val partitionSelection = if (utsBootRepatch) {
                if (hasCustomSelected) {
                    partitionsState.getOrNull(partitionSelectionIndex)
                } else {
                    null
                }
            } else {
                partitionsState.getOrNull(partitionSelectionIndex)
            }
            val flashIt = if (method is InstallMethod.DownloadFile) {
                val url = method.url ?: return@onInstall
                val partition = method.partition ?: return@onInstall
                FlashIt.DownloadBoot(
                    url = url,
                    partition = partition,
                    targetKmi = targetKmi,
                    lkm = selectedLkm,
                    allowShell = allowShell,
                    blockModules = modules,
                    bootConfig = bootConfig,
                    enableAdb = enableAdb,
                    backup = forceBackup,
                    superKey = effectiveSuperKey.ifBlank { null },
                    signatureBypass = signatureBypass,
                )
            } else FlashIt.FlashBoot(
                boot = if (!utsBootRepatch && method is InstallMethod.SelectFile) {
                    method.uri
                } else {
                    null
                },
                lkm = if (utsBootRepatch && selectedLkm is LkmSelection.LkmUri) {
                    LkmSelection.KmiNone
                } else {
                    selectedLkm
                },
                targetKmi = targetKmi,
                ota = isOta,
                partition = partitionSelection,
                allowShell = allowShell,
                blockModules = modules,
                bootConfig = bootConfig,
                enableAdb = enableAdb,
                backup = !utsBootRepatch &&
                    method is InstallMethod.SelectFile &&
                    forceBackup,
                superKey = effectiveSuperKey.ifBlank { null },
                signatureBypass = signatureBypass
            )
            navigator.navigate(FlashScreenDestination(flashIt))
        }

    val selectKmiDialog = rememberSelectKmiDialog { kmi ->
        val method = installMethod
        if (kmi != null && method != null) {
            onInstall(method, lkmSelection, kmi)
        }
    }

    val onClickNext = onClickNext@{
        val method = installMethod
        if (method != null && !isResolvingKmi) {
            if (configOnly || (embedLkmInBoot && directKernelMethod)) {
                onInstall(method, lkmSelection, "")
                return@onClickNext
            }
            isResolvingKmi = true
            loadingDialog.show()
            coroutineScope.launch {
                val kmi = try {
                    val ota = method is InstallMethod.DirectInstallToInactiveSlot
                    val bootUri = (method as? InstallMethod.SelectFile)?.uri
                    val detectedKmi = if (method is InstallMethod.DownloadFile) {
                        method.remoteKmi.orEmpty()
                    } else {
                        getTargetKmi(ota, bootUri)
                    }
                    resolveTargetKmi(
                        detectedKmi = detectedKmi,
                        supportedKmis = getSupportedKmis(),
                        customLkm = lkmSelection is LkmSelection.LkmUri,
                    )
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Throwable) {
                    null
                } finally {
                    isResolvingKmi = false
                    loadingDialog.hide()
                }

                if (kmi == null) {
                    selectKmiDialog.show()
                } else {
                    onInstall(method, lkmSelection, kmi)
                }
            }
        }
    }

    val selectLkmLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode == Activity.RESULT_OK) {
            it.data?.data?.let { uri ->
                val isKo = isKoFile(context, uri)
                if (isKo) {
                    lkmSelection = LkmSelection.LkmUri(uri)
                } else {
                    lkmSelection = LkmSelection.KmiNone
                    Toast.makeText(
                        context,
                        resources.getString(R.string.install_only_support_ko_file),
                        Toast.LENGTH_SHORT
                    ).show()
                }
            }
        }
    }

    val onLkmUpload = {
        selectLkmLauncher.launch(Intent(Intent.ACTION_GET_CONTENT).apply {
            type = "application/octet-stream"
        })
    }

    val onDownload: () -> Unit = {
        val url = downloadUrl.trim()
        val parsedUrl = runCatching { url.toUri() }.getOrNull()
        if (parsedUrl?.scheme?.equals("https", ignoreCase = true) != true ||
            parsedUrl.host.isNullOrBlank()
        ) {
            Toast.makeText(context, R.string.download_dialog_msg, Toast.LENGTH_SHORT).show()
        } else {
            showDownloadDialog = false
            loadingDialog.show()
            coroutineScope.launch {
                try {
                    val probe = probeRemoteBootPartitions(url)
                    if (probe.partitions.isEmpty()) {
                        Toast.makeText(
                            context,
                            R.string.download_no_boot_partition,
                            Toast.LENGTH_LONG
                        ).show()
                    } else {
                        remotePartitions = probe.partitions
                        remotePartitionSelectionIndex = 0
                        installMethod = InstallMethod.DownloadFile(
                            url = url,
                            partition = probe.partitions.first(),
                            remoteKmi = probe.kmi,
                        )
                    }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Throwable) {
                    Toast.makeText(
                        context,
                        resources.getString(
                            R.string.download_probe_failed,
                            error.message ?: error.javaClass.simpleName
                        ),
                        Toast.LENGTH_LONG
                    ).show()
                } finally {
                    loadingDialog.hide()
                }
            }
        }
    }

    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    BackHandler(enabled = utsBootRepatch) {
        navigator.popBackStack()
    }

    if (showDownloadDialog) {
        YukiAlertDialog(
            onDismissRequest = { showDownloadDialog = false },
            title = { Text(stringResource(R.string.download_dialog_title)) },
            text = {
                OutlinedTextField(
                    value = downloadUrl,
                    onValueChange = { downloadUrl = it },
                    label = { Text(stringResource(R.string.download_dialog_msg)) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
            },
            confirmButton = {
                TextButton(onClick = onDownload) {
                    Text(stringResource(android.R.string.ok))
                }
            },
            dismissButton = {
                TextButton(onClick = { showDownloadDialog = false }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }

    Scaffold(
        topBar = {
            TopBar(
                onBack = { navigator.popBackStack() },
                scrollBehavior = scrollBehavior
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        )
    ) { innerPadding ->
        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme.copy(
                surface = if (isExpressiveUi) Color.Transparent else MaterialTheme.colorScheme.surface
            )
        ) {
            Column(
                modifier = Modifier
                    .padding(innerPadding)
                    .nestedScroll(scrollBehavior.nestedScrollConnection)
                    .verticalScroll(rememberScrollState())
                    .padding(top = 12.dp)
            ) {
            SelectInstallMethod(
                onSelected = {
                    installMethod = it
                    hasCustomSelected = false
                },
                onDownload = {
                    downloadUrl = ""
                    showDownloadDialog = true
                },
                selectedMethod = installMethod,
                directInstallOnly = utsBootRepatch,
            )

            // Select the target partition for direct LKM installation.
            YukiExpandableContent(
                visible = !configOnly && !embedLkmInBoot &&
                    (installMethod is InstallMethod.DirectInstall ||
                        installMethod is InstallMethod.DirectInstallToInactiveSlot),
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp)
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.surfaceVariant),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp),
                    ) {
                        val isOta = installMethod is InstallMethod.DirectInstallToInactiveSlot
                        val suffix = produceState(initialValue = "", isOta) {
                            value = getSlotSuffix(isOta)
                        }.value

                        val partitions = produceState(initialValue = emptyList()) {
                            value = getAvailablePartitions()
                        }.value

                        val defaultPartition = produceState(
                            initialValue = "",
                            isOta,
                            utsBootRepatch,
                        ) {
                            // The current-slot default is not evidence for an
                            // inactive-slot OTA. ksud will inspect target boot
                            // when the user has not explicitly overridden it.
                            value = if (utsBootRepatch && isOta) {
                                ""
                            } else {
                                getDefaultPartition()
                            }
                        }.value

                        partitionsState = partitions
                        val displayPartitions = partitions.map { name ->
                            if (defaultPartition == name) "$name (default)" else name
                        }

                        val defaultIndex = partitions.indexOf(defaultPartition).takeIf { it >= 0 } ?: 0
                        if (!hasCustomSelected) partitionSelectionIndex = defaultIndex

                        SuperDropdown(
                            items = displayPartitions,
                            selectedIndex = partitionSelectionIndex,
                            title = "${stringResource(R.string.install_select_partition)} (${suffix})",
                            onSelectedIndexChange = { index ->
                                hasCustomSelected = true
                                partitionSelectionIndex = index
                            },
                            leftAction = {
                                YukiIcon(
                                    Icons.Filled.Build,
                                    tint = MaterialTheme.colorScheme.onSurface,
                                    modifier = Modifier.padding(end = 16.dp),
                                    contentDescription = null
                                )
                            }
                        )
                    }
                }
            }

            YukiExpandableContent(
                visible = installMethod is InstallMethod.DownloadFile,
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp)
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.surfaceVariant),
                        elevation = getCardElevation(),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        val method = installMethod as? InstallMethod.DownloadFile
                        SuperDropdown(
                            items = remotePartitions,
                            selectedIndex = remotePartitionSelectionIndex,
                            title = stringResource(R.string.install_select_partition),
                            summary = method?.url,
                            onSelectedIndexChange = { index ->
                                remotePartitionSelectionIndex = index
                                method?.let {
                                    installMethod = it.copy(
                                        partition = remotePartitions.getOrNull(index)
                                    )
                                }
                            },
                            leftAction = {
                                YukiIcon(
                                    Icons.Filled.Build,
                                    tint = MaterialTheme.colorScheme.onSurface,
                                    modifier = Modifier.padding(end = 16.dp),
                                    contentDescription = null
                                )
                            }
                        )
                    }
                }
            }

            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp)
            ) {
                // Select a local LKM file.
                if (!utsBootRepatch && !configOnly) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.surfaceVariant),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp),
                    ) {
                        ListItem(
                            content = {
                                Text(stringResource(id = R.string.install_upload_lkm_file))
                            },
                            supportingContent = {
                                (lkmSelection as? LkmSelection.LkmUri)?.let {
                                    Text(
                                        stringResource(
                                            id = R.string.selected_lkm,
                                            it.uri.lastPathSegment ?: "(file)"
                                        )
                                    )
                                }
                            },
                            leadingContent = {
                                YukiIcon(
                                    Icons.Filled.Download,
                                    contentDescription = null
                                )
                            },
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onLkmUpload() }
                        )
                    }
                }

                // SuperKey input is shared by ramdisk and ImgPatch flows.
                YukiExpandableContent(
                    visible = !configOnly && (installMethod is InstallMethod.DirectInstall ||
                              installMethod is InstallMethod.DirectInstallToInactiveSlot ||
                              installMethod is InstallMethod.SelectFile ||
                              installMethod is InstallMethod.DownloadFile),
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.tertiaryContainer),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp),
                    ) {
                        Column(
                            modifier = Modifier.padding(16.dp)
                        ) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                modifier = Modifier.padding(bottom = 8.dp)
                            ) {
                                YukiIcon(
                                    Icons.Default.Security,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.tertiary
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = stringResource(id = R.string.superkey_optional_title),
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MaterialTheme.colorScheme.tertiary
                                )
                            }
                            Text(
                                text = stringResource(id = R.string.superkey_optional_desc),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onTertiaryContainer,
                                modifier = Modifier.padding(bottom = 12.dp)
                            )
                            OutlinedTextField(
                                value = if (usingSavedKey) "********" else superKey,
                                onValueChange = { newValue ->
                                    if (usingSavedKey) {
                                        // First edit drops the placeholder; the user has to
                                        // type the new key from scratch.
                                        usingSavedKey = false
                                        superKey = ""
                                        return@OutlinedTextField
                                    }
                                    superKey = newValue
                                    if (newValue.equals("transright", ignoreCase = true) && !ThemeConfig.isTransPrideUnlocked) {
                                        ThemeManager.unlockTransPride(context)
                                        ThemeConfig.currentTheme = ThemeColors.TransPride
                                        ThemeManager.saveThemeColors(context, "trans")
                                        Toast.makeText(context, "🏳️‍⚧️ Trans Rights! ✨", Toast.LENGTH_LONG).show()
                                    }
                                },
                                label = { Text(stringResource(id = R.string.superkey_input_hint)) },
                                placeholder = { Text(stringResource(id = R.string.superkey_input_placeholder)) },
                                modifier = Modifier.fillMaxWidth(),
                                singleLine = true
                            )
                            
                            // Signature bypass switch
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(top = 12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(
                                        text = stringResource(id = R.string.signature_bypass_title),
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.onTertiaryContainer
                                    )
                                    Text(
                                        text = stringResource(id = R.string.signature_bypass_desc),
                                        style = MaterialTheme.typography.bodySmall,
                                        color = MaterialTheme.colorScheme.onTertiaryContainer.copy(alpha = 0.7f)
                                    )
                                }
                                YukiSwitch(
                                    checked = signatureBypass,
                                    onCheckedChange = { signatureBypass = it },
                                    enabled = effectiveSuperKey.isNotBlank()
                                )
                            }
                        }
                    }
                }

                YukiExpandableContent(
                    visible = directKernelMethod && embedLkmInBoot,
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.errorContainer),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp),
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.Top,
                        ) {
                            YukiIcon(
                                Icons.Outlined.Warning,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.error,
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            Column {
                                Text(
                                    text = stringResource(R.string.install_direct_lkm_warning_title),
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MaterialTheme.colorScheme.onErrorContainer,
                                )
                                Spacer(modifier = Modifier.height(4.dp))
                                Text(
                                    text = stringResource(R.string.install_direct_lkm_warning_summary),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onErrorContainer,
                                )
                            }
                        }
                    }
                }

                YukiExpandableContent(
                    visible = installMethod != null,
                    large = true,
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.secondaryContainer),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp),
                    ) {
                        Column(
                            modifier = Modifier.padding(16.dp)
                        ) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .heightIn(min = 48.dp)
                                    .yukiExpansionSemantics(advancedOptionsExpanded)
                                    .clickable(
                                        role = Role.Button,
                                        onClickLabel = stringResource(
                                            if (advancedOptionsExpanded) R.string.collapse_menu else R.string.expand_menu
                                        ),
                                    ) { advancedOptionsExpanded = !advancedOptionsExpanded },
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                YukiIcon(
                                    Icons.Default.DeveloperMode,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.secondary
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = stringResource(id = R.string.advanced_options),
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MaterialTheme.colorScheme.secondary,
                                    modifier = Modifier.weight(1f),
                                )
                                YukiExpandArrow(
                                    expanded = advancedOptionsExpanded,
                                    tint = MaterialTheme.colorScheme.secondary,
                                )
                            }
                            YukiExpandableContent(
                                visible = advancedOptionsExpanded,
                                large = true,
                            ) {
                                Column {
                                    Spacer(modifier = Modifier.height(12.dp))

                                    Row(
                                        modifier = Modifier.fillMaxWidth(),
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.SpaceBetween,
                                    ) {
                                        Column(modifier = Modifier.weight(1f)) {
                                            Text(
                                                text = stringResource(R.string.install_block_modules),
                                                style = MaterialTheme.typography.bodyMedium,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer,
                                            )
                                            Text(
                                                text = stringResource(R.string.install_block_modules_warning),
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.75f),
                                            )
                                        }
                                        YukiSwitch(
                                            checked = blockModulesEnabled,
                                            onCheckedChange = { blockModulesEnabled = it },
                                        )
                                    }
                                    YukiExpandableContent(visible = blockModulesEnabled) {
                                        Spacer(modifier = Modifier.height(12.dp))
                                        OutlinedTextField(
                                            value = blockModules,
                                            onValueChange = {
                                                blockModules = it
                                                blockModulesInitialized = true
                                            },
                                            modifier = Modifier.fillMaxWidth(),
                                            label = { Text(stringResource(R.string.install_block_modules_list)) },
                                            supportingText = {
                                                Text(stringResource(
                                                    if (normalizedBlockModules == null) R.string.install_block_modules_invalid
                                                    else R.string.install_block_modules_summary
                                                ))
                                            },
                                            isError = normalizedBlockModules == null,
                                            singleLine = true,
                                        )
                                    }
                                    Spacer(modifier = Modifier.height(12.dp))

                                    BootConfigControls(
                                        allowConfigOnly = installMethod is InstallMethod.SelectFile,
                                        configOnly = configOnly,
                                        onConfigOnlyChange = {
                                            noInstall = it
                                            if (it) embedLkmInBoot = false
                                        },
                                        noPreinit = noPreinit,
                                        onNoPreinitChange = { noPreinit = it },
                                        noCustomRc = noCustomRc,
                                        onNoCustomRcChange = { noCustomRc = it },
                                        forcedFeatures = forcedFeatures,
                                        onForcedFeaturesChange = { forcedFeatures = it },
                                    )
                                    YukiExpandableContent(visible = directKernelMethod) {
                                        Row(
                                            modifier = Modifier.fillMaxWidth(),
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.SpaceBetween,
                                        ) {
                                            Column(modifier = Modifier.weight(1f)) {
                                                Text(
                                                    text = stringResource(R.string.install_direct_lkm_title),
                                                    style = MaterialTheme.typography.bodyMedium,
                                                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                                                )
                                                Text(
                                                    text = stringResource(R.string.install_direct_lkm_summary),
                                                    style = MaterialTheme.typography.bodySmall,
                                                    color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.75f),
                                                )
                                            }
                                            YukiSwitch(
                                                checked = embedLkmInBoot,
                                                onCheckedChange = {
                                                    embedLkmInBoot = it
                                                    if (it) noInstall = false
                                                },
                                            )
                                        }
                                        Spacer(modifier = Modifier.height(12.dp))
                                    }

                                    YukiExpandableContent(
                                        visible = installMethod is InstallMethod.SelectFile && !embedLkmInBoot && !configOnly,
                                    ) {
                                        Row(
                                            modifier = Modifier.fillMaxWidth(),
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.SpaceBetween
                                        ) {
                                            Column(modifier = Modifier.weight(1f)) {
                                                Text(
                                                    text = stringResource(id = R.string.install_force_backup),
                                                    style = MaterialTheme.typography.bodyMedium,
                                                    color = MaterialTheme.colorScheme.onSecondaryContainer
                                                )
                                                Text(
                                                    text = stringResource(id = R.string.install_force_backup_summary),
                                                    style = MaterialTheme.typography.bodySmall,
                                                    color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.75f)
                                                )
                                            }
                                            YukiSwitch(
                                                checked = forceBackup,
                                                onCheckedChange = { forceBackup = it }
                                            )
                                        }

                                        Spacer(modifier = Modifier.height(12.dp))
                                    }

                                    Row(
                                        modifier = Modifier.fillMaxWidth(),
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.SpaceBetween
                                    ) {
                                        Column(modifier = Modifier.weight(1f)) {
                                            Text(
                                                text = stringResource(id = R.string.allow_shell),
                                                style = MaterialTheme.typography.bodyMedium,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer
                                            )
                                            Text(
                                                text = stringResource(id = R.string.allow_shell_summary),
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.75f)
                                            )
                                        }
                                        YukiSwitch(
                                            checked = allowShell,
                                            onCheckedChange = { allowShell = it }
                                        )
                                    }

                                    Spacer(modifier = Modifier.height(12.dp))

                                    Row(
                                        modifier = Modifier.fillMaxWidth(),
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.SpaceBetween
                                    ) {
                                        Column(modifier = Modifier.weight(1f)) {
                                            Text(
                                                text = stringResource(id = R.string.enable_adb),
                                                style = MaterialTheme.typography.bodyMedium,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer
                                            )
                                            Text(
                                                text = stringResource(id = R.string.enable_adb_summary),
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.75f)
                                            )
                                        }
                                        YukiSwitch(
                                            checked = enableAdb,
                                            onCheckedChange = { enableAdb = it }
                                        )
                                    }

                                    YukiExpandableContent(visible = utsBootRepatch) {
                                        Spacer(modifier = Modifier.height(12.dp))
                                        HorizontalDivider(
                                            color = MaterialTheme.colorScheme.onSecondaryContainer
                                                .copy(alpha = 0.12f)
                                        )
                                        Spacer(modifier = Modifier.height(12.dp))
                                        Row(
                                            modifier = Modifier.fillMaxWidth(),
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.SpaceBetween
                                        ) {
                                            Text(
                                                text = stringResource(
                                                    R.string.uts_boot_repatch_option
                                                ),
                                                style = MaterialTheme.typography.bodyMedium,
                                                color = MaterialTheme.colorScheme.onSecondaryContainer,
                                                modifier = Modifier.weight(1f)
                                            )
                                            Text(
                                                text = stringResource(
                                                    R.string.uts_boot_repatch_changed
                                                ),
                                                style = MaterialTheme.typography.labelLarge,
                                                color = MaterialTheme.colorScheme.secondary
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                YukiExpandableContent(
                    visible = !utsBootRepatch && installMethod == null,
                ) {
                    Column(modifier = Modifier.fillMaxWidth()) {
                        Spacer(Modifier.height(16.dp))
                        Text(
                            text = stringResource(R.string.install_advanced_tools),
                            style = if (isExpressiveUi) {
                                MaterialTheme.typography.labelLarge
                            } else {
                                MaterialTheme.typography.titleMedium
                            },
                            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(bottom = 8.dp)
                        )

                        InstallSurface(
                            onClick = { navigator.navigate(PartitionManagerScreenDestination) },
                            colors = getCardColors(MaterialTheme.colorScheme.surfaceVariant),
                            elevation = getCardElevation(),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            ListItem(
                                content = {
                                    Text(stringResource(R.string.partition_manager))
                                },
                                supportingContent = {
                                    Text(stringResource(R.string.partition_manager_desc))
                                },
                                leadingContent = {
                                    YukiIcon(
                                        Icons.Filled.Folder,
                                        contentDescription = null
                                    )
                                },
                                trailingContent = {
                                    YukiIcon(
                                        Icons.AutoMirrored.Filled.NavigateNext,
                                        contentDescription = null
                                    )
                                }
                            )
                        }

                        Spacer(Modifier.height(16.dp))
                    }
                }

                YukiExpandableContent(
                    visible = !utsBootRepatch && !isManager,
                ) {
                    InstallSurface(
                        colors = getCardColors(MaterialTheme.colorScheme.tertiaryContainer),
                        elevation = getCardElevation(),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(bottom = 12.dp)
                    ) {
                        Column(
                            modifier = Modifier.padding(16.dp)
                        ) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                YukiIcon(
                                    Icons.Filled.DeveloperMode,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.tertiary
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = stringResource(R.string.install_jailbreak_title),
                                    style = MaterialTheme.typography.titleMedium,
                                    color = MaterialTheme.colorScheme.tertiary
                                )
                            }

                            Spacer(modifier = Modifier.height(8.dp))

                            Text(
                                text = stringResource(R.string.install_jailbreak_summary),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onTertiaryContainer
                            )

                            if (!isSelinuxPermissive) {
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = stringResource(R.string.install_jailbreak_requires_permissive),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onTertiaryContainer.copy(alpha = 0.75f)
                                )
                            }

                            Spacer(modifier = Modifier.height(12.dp))

                            Button(
                                onClick = onJailbreakInstall,
                                enabled = canJailbreakInstall,
                                modifier = Modifier.fillMaxWidth(),
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = MaterialTheme.colorScheme.tertiary,
                                    contentColor = MaterialTheme.colorScheme.onTertiary
                                )
                            ) {
                                Text(stringResource(R.string.install_jailbreak_button))
                            }
                        }
                    }
                }

                Button(
                    modifier = Modifier
                        .fillMaxWidth()
                        .defaultMinSize(minHeight = if (isExpressiveUi) 56.dp else 0.dp),
                    enabled = installMethod != null && !isResolvingKmi && normalizedBlockModules != null,
                    onClick = onClickNext,
                    shape = if (isExpressiveUi) {
                        MaterialTheme.shapes.extraLarge
                    } else {
                        MaterialTheme.shapes.medium
                    },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primary,
                        contentColor = MaterialTheme.colorScheme.onPrimary,
                        disabledContainerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f),
                        disabledContentColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                    )
                ) {
                    Text(
                        stringResource(id = R.string.install_next),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }

            }
            }
        }
    }
}

@Composable
private fun InstallMethodSectionSurface(
    modifier: Modifier = Modifier,
    colors: CardColors = CardDefaults.elevatedCardColors(),
    elevation: CardElevation = CardDefaults.elevatedCardElevation(),
    content: @Composable ColumnScope.() -> Unit,
) {
    if (isExpressiveUi) {
        Column(modifier = modifier, content = content)
    } else {
        ElevatedCard(
            modifier = modifier,
            colors = colors,
            elevation = elevation,
            content = content,
        )
    }
}

@Composable
private fun InstallSurface(
    modifier: Modifier = Modifier,
    colors: CardColors = CardDefaults.elevatedCardColors(),
    elevation: CardElevation = CardDefaults.elevatedCardElevation(),
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    if (isExpressiveUi) {
        Column(
            modifier = modifier
                .clip(MaterialTheme.shapes.large)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                )
                .then(
                    if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier
                ),
            content = content,
        )
    } else if (onClick != null) {
        ElevatedCard(
            onClick = onClick,
            modifier = modifier,
            colors = colors,
            elevation = elevation,
            content = content,
        )
    } else {
        ElevatedCard(
            modifier = modifier,
            colors = colors,
            elevation = elevation,
            content = content,
        )
    }
}

@Composable
private fun RebootDialog(
    show: Boolean,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit
) {
    if (show) {
        YukiAlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(stringResource(id = R.string.reboot_complete_title)) },
            text = { Text(stringResource(id = R.string.reboot_complete_msg)) },
            confirmButton = {
                TextButton(onClick = onConfirm) {
                    Text(stringResource(id = R.string.yes))
                }
            },
            dismissButton = {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(id = R.string.no))
                }
            }
        )
    }
}

sealed class InstallMethod {
    data class SelectFile(
        val uri: Uri? = null,
        @param:StringRes override val label: Int = R.string.select_file,
        override val summary: String?
    ) : InstallMethod()

    data class DownloadFile(
        val url: String? = null,
        val partition: String? = null,
        val remoteKmi: String? = null,
        @param:StringRes override val label: Int = R.string.download_file,
        override val summary: String? = null,
    ) : InstallMethod()

    data object DirectInstall : InstallMethod() {
        override val label: Int
            get() = R.string.direct_install
    }

    data object DirectInstallToInactiveSlot : InstallMethod() {
        override val label: Int
            get() = R.string.install_inactive_slot
    }

    abstract val label: Int
    open val summary: String? = null
}

@Composable
private fun SelectInstallMethod(
    onSelected: (InstallMethod) -> Unit = {},
    onDownload: () -> Unit = {},
    selectedMethod: InstallMethod? = null,
    directInstallOnly: Boolean = false,
) {
    val rootAvailable by produceState(initialValue = false) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            com.anatdx.yukisu.ui.util.rootAvailable()
        }
    }
    val isAbDevice = produceState(initialValue = false) {
        value = isAbDevice()
    }.value
    val defaultPartitionName = produceState(initialValue = "boot") {
        getDefaultPartition().takeIf { it.isNotBlank() }?.let { value = it }
    }.value
    val selectFileTip = stringResource(
        id = R.string.select_file_tip, defaultPartitionName
    )
    val radioOptions = mutableListOf<InstallMethod>()
    if (!directInstallOnly) {
        radioOptions.add(InstallMethod.SelectFile(summary = selectFileTip))
        radioOptions.add(
            InstallMethod.DownloadFile(
                summary = stringResource(R.string.download_file_summary)
            )
        )
    }

    if (rootAvailable) {
        radioOptions.add(InstallMethod.DirectInstall)
        if (isAbDevice) {
            radioOptions.add(InstallMethod.DirectInstallToInactiveSlot)
        }
    }

    var selectedOption by remember { mutableStateOf<InstallMethod?>(null) }
    var currentSelectingMethod by remember { mutableStateOf<InstallMethod?>(null) }

    LaunchedEffect(selectedMethod, directInstallOnly) {
        selectedOption = selectedMethod.takeIf {
            !directInstallOnly ||
                it is InstallMethod.DirectInstall ||
                it is InstallMethod.DirectInstallToInactiveSlot
        }
    }

    val selectImageLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode == Activity.RESULT_OK) {
            it.data?.data?.let { uri ->
                val option = when (currentSelectingMethod) {
                    is InstallMethod.SelectFile -> InstallMethod.SelectFile(
                        uri,
                        summary = selectFileTip
                    )
                    else -> null
                }
                option?.let { opt ->
                    selectedOption = opt
                    onSelected(opt)
                }
            }
        }
    }

    val confirmDialog = rememberConfirmDialog(
        onConfirm = {
            selectedOption = InstallMethod.DirectInstallToInactiveSlot
            onSelected(InstallMethod.DirectInstallToInactiveSlot)
        },
        onDismiss = null
    )

    val dialogTitle = stringResource(id = android.R.string.dialog_alert_title)
    val dialogContent = stringResource(id = R.string.install_inactive_slot_warning)

    val onClick = { option: InstallMethod ->
        currentSelectingMethod = option
        when (option) {
            is InstallMethod.SelectFile -> {
                selectImageLauncher.launch(Intent(Intent.ACTION_GET_CONTENT).apply {
                    type = "application/*"
                    putExtra(
                        Intent.EXTRA_MIME_TYPES,
                        arrayOf("application/octet-stream", "application/zip")
                    )
                })
            }

            is InstallMethod.DownloadFile -> onDownload()

            is InstallMethod.DirectInstall -> {
                selectedOption = option
                onSelected(option)
            }

            is InstallMethod.DirectInstallToInactiveSlot -> {
                confirmDialog.showConfirm(dialogTitle, dialogContent)
            }
        }
    }

    Column(
        modifier = Modifier.padding(horizontal = 16.dp)
    ) {
        // LKM installation and patching methods.
            InstallMethodSectionSurface(
                colors = getCardColors(MaterialTheme.colorScheme.surfaceVariant),
                elevation = getCardElevation(),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 16.dp)
            ) {
                MaterialTheme(
                    colorScheme = MaterialTheme.colorScheme.copy(
                        surface = if (isExpressiveUi || CardConfig.isCustomBackgroundEnabled) {
                            Color.Transparent
                        } else {
                            MaterialTheme.colorScheme.surfaceVariant
                        }
                    )
                ) {
                    ListItem(
                        leadingContent = {
                            YukiIcon(
                                Icons.Filled.AutoAwesome,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.primary
                            )
                        },
                        content = {
                            Text(
                                stringResource(R.string.Lkm_install_methods),
                                style = if (isExpressiveUi) {
                                    MaterialTheme.typography.labelLarge
                                } else {
                                    MaterialTheme.typography.titleMedium
                                },
                                fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                                color = if (isExpressiveUi) {
                                    MaterialTheme.colorScheme.primary
                                } else {
                                    MaterialTheme.colorScheme.onSurface
                                },
                            )
                        }
                    )
                }

                Column(
                    modifier = if (isExpressiveUi) {
                        Modifier
                    } else {
                        Modifier.padding(
                            start = 16.dp,
                            end = 16.dp,
                            bottom = 16.dp
                        )
                    }
                ) {
                    radioOptions.forEachIndexed { index, option ->
                            val interactionSource = remember { MutableInteractionSource() }
                            val optionShape = if (isExpressiveUi) {
                                if (radioOptions.size == 1) {
                                    MaterialTheme.shapes.large
                                } else {
                                    ListItemDefaults.segmentedShapes(index, radioOptions.size).shape
                                }
                            } else {
                                MaterialTheme.shapes.medium
                            }
                            Surface(
                                color = if (option.javaClass == selectedOption?.javaClass)
                                    MaterialTheme.colorScheme.secondaryContainer.copy(alpha = cardAlpha)
                                else if (isExpressiveUi)
                                    MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                                else
                                    MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = cardAlpha),
                                shape = optionShape,
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .then(
                                        if (isExpressiveUi) {
                                            Modifier
                                                .padding(
                                                    horizontal = 6.dp,
                                                    vertical = ListItemDefaults.SegmentedGap / 2,
                                                )
                                                .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                                        } else {
                                            Modifier.padding(vertical = 4.dp)
                                        }
                                    )
                                    .clip(optionShape)
                            ) {
                                Row(
                                    verticalAlignment = Alignment.CenterVertically,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .selectable(
                                            selected = option.javaClass == selectedOption?.javaClass,
                                            onClick = { onClick(option) },
                                            role = Role.RadioButton,
                                            indication = LocalIndication.current,
                                            interactionSource = interactionSource
                                        )
                                        .padding(vertical = 8.dp, horizontal = 12.dp)
                                ) {
                                    RadioButton(
                                        selected = option.javaClass == selectedOption?.javaClass,
                                        onClick = null,
                                        interactionSource = interactionSource,
                                        colors = RadioButtonDefaults.colors(
                                            selectedColor = MaterialTheme.colorScheme.primary,
                                            unselectedColor = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    )
                                    Column(
                                        modifier = Modifier
                                            .padding(start = 10.dp)
                                            .weight(1f)
                                    ) {
                                        Text(
                                            text = stringResource(id = option.label),
                                            style = MaterialTheme.typography.bodyLarge
                                        )
                                        option.summary?.let {
                                            Text(
                                                text = it,
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSurfaceVariant
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
        }
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun rememberSelectKmiDialog(onSelected: (String?) -> Unit): DialogHandle {
    return rememberCustomDialog { dismiss ->
        val feedbackView = LocalView.current
        val supportedKmi by produceState(initialValue = emptyList()) {
            value = getSupportedKmis()
        }
        val options = supportedKmi.map { value ->
            ListOption(
                titleText = value
            )
        }

        var selection by remember { mutableStateOf<String?>(null) }

        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme.copy(
                surface = MaterialTheme.colorScheme.surfaceContainerHigh
            )
        ) {
            YukiDialogTheme {
                ListDialog(state = rememberUseCaseState(visible = true, onFinishedRequest = {
                    feedbackView.performClickHapticFeedback()
                    onSelected(selection)
                }, onCloseRequest = {
                    dismiss()
                }), header = Header.Default(
                    title = stringResource(R.string.select_kmi),
                ), selection = ListSelection.Single(
                    showRadioButtons = true,
                    options = options,
                ) { _, option ->
                    feedbackView.performClickHapticFeedback()
                    selection = option.titleText
                })
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    onBack: () -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (isExpressiveUi || CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.install),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back)
            )
        }
    }
    val colors = getTopAppBarColors(
        containerColor = cardColor,
        scrolledContainerColor = cardColor
    )
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = title,
            colors = colors,
            navigationIcon = navigationIcon,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    } else {
        TopAppBar(
            title = title,
            colors = colors,
            navigationIcon = navigationIcon,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    }
}

private fun isKoFile(context: Context, uri: Uri): Boolean {
    val seg = uri.lastPathSegment ?: ""
    if (seg.endsWith(".ko", ignoreCase = true)) return true

    return try {
        context.contentResolver.query(
            uri,
            arrayOf(OpenableColumns.DISPLAY_NAME),
            null,
            null,
            null
        )?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx != -1 && cursor.moveToFirst()) {
                val name = cursor.getString(idx)
                name?.endsWith(".ko", ignoreCase = true) == true
            } else {
                false
            }
        } ?: false
    } catch (_: Throwable) {
        false
    }
}

@Preview
@Composable
fun SelectInstallPreview() {
    InstallScreen(EmptyDestinationsNavigator)
}
