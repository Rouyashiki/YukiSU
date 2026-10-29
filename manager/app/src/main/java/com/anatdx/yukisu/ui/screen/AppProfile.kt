package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.annotation.SuppressLint
import androidx.annotation.StringRes
import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.AdminPanelSettings
import androidx.compose.material.icons.filled.Android
import androidx.compose.material.icons.filled.Security
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.dropUnlessResumed
import androidx.lifecycle.viewmodel.compose.LocalViewModelStoreOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AppProfileTemplateScreenDestination
import com.ramcosta.composedestinations.generated.destinations.TemplateEditorScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.DynamicManagerCompatDialog
import com.anatdx.yukisu.ui.component.ConfirmResult
import com.anatdx.yukisu.ui.component.SwitchItem
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.clickHapticFeedback
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.component.profile.AppProfileConfig
import com.anatdx.yukisu.ui.component.profile.RootProfileConfig
import com.anatdx.yukisu.ui.component.profile.TemplateConfig
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.*
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import com.anatdx.yukisu.ui.viewmodel.getTemplateInfoById
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/**
 * @author weishu
 * @date 2023/5/16.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun AppProfileScreen(
    navigator: DestinationsNavigator,
    appInfo: SuperUserViewModel.AppInfo,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val snackBarHost = LocalSnackbarHost.current
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val scope = rememberCoroutineScope()
    val viewModelStoreOwner = checkNotNull(LocalViewModelStoreOwner.current)
    val superUserViewModel = viewModel<SuperUserViewModel>(viewModelStoreOwner = viewModelStoreOwner)
    val failToUpdateAppProfile = stringResource(R.string.failed_to_update_app_profile).format(appInfo.label)
    val failToUpdateSepolicy = stringResource(R.string.failed_to_update_sepolicy).format(appInfo.label)
    val suNotAllowed = stringResource(R.string.su_not_allowed).format(appInfo.label)
    val failedToSetDynamicManager = stringResource(R.string.dynamic_manager_set_failed).format(appInfo.label)
    val failedToDelDynamicManager = stringResource(R.string.dynamic_manager_del_failed).format(appInfo.label)
    val dynamicManagerSignatureUnavailable = stringResource(R.string.dynamic_manager_signature_unavailable).format(appInfo.label)
    val dynamicManagerConfirmDialog = rememberConfirmDialog()
    var dynamicManagerError by rememberSaveable { mutableStateOf<String?>(null) }
    var dynamicManagerErrorTitle by rememberSaveable { mutableStateOf("") }

    dynamicManagerError?.let { error ->
        YukiAlertDialog(
            onDismissRequest = { dynamicManagerError = null },
            title = { Text(dynamicManagerErrorTitle) },
            text = {
                SelectionContainer {
                    Text(error, Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState()))
                }
            },
            confirmButton = {
                TextButton(onClick = { dynamicManagerError = null }) {
                    Text(stringResource(android.R.string.ok))
                }
            }
        )
    }

    val packageName = appInfo.packageName
    val profileKey = appInfo.profileKey
    val uidApps = SuperUserViewModel.appGroups
        .firstOrNull { it.uid == appInfo.uid }
        ?.apps
        ?: listOf(appInfo)
    val isSharedUid = uidApps.size > 1
    // Both calls cross JNI/shell, so keep them out of recomposition.
    val initialProfile = remember(profileKey, appInfo.uid) {
        Natives.getAppProfile(profileKey, appInfo.uid).let {
            if (appInfo.special) it.copy(allowSu = false) else it
        }.also { p ->
            if (p.allowSu) p.rules = getSepolicy(profileKey)
        }
    }
    var savedProfile by rememberSaveable {
        mutableStateOf(initialProfile)
    }
    val profile = savedProfile.withoutLegacySepolicyDiagnostic(profileKey)
    var dynamicManagerChecked by rememberSaveable(packageName, appInfo.uid) {
        mutableStateOf(false)
    }
    var dynamicManagerFlags by rememberSaveable(packageName, appInfo.uid) {
        mutableIntStateOf(0)
    }
    var dynamicManagerBusy by rememberSaveable(packageName, appInfo.uid) {
        mutableStateOf(false)
    }
    var dynamicManagerSignature by remember(packageName, appInfo.uid) {
        mutableStateOf<KsuCli.DynamicManagerSignature?>(null)
    }

    var dynamicManagerCompat by remember(packageName, appInfo.uid) {
        mutableStateOf<KsuCli.DynamicManagerCompat?>(null)
    }

    fun editDynamicManagerCompat() {
        scope.launch {
            dynamicManagerBusy = true
            try {
                dynamicManagerCompat = KsuCli.getDynamicManagerCompat(appInfo.uid)
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                dynamicManagerErrorTitle = failedToSetDynamicManager
                dynamicManagerError = e.message ?: e.javaClass.simpleName
            } finally {
                dynamicManagerBusy = false
            }
        }
    }

    dynamicManagerCompat?.let { compat ->
        DynamicManagerCompatDialog(
            initial = compat,
            onDismiss = { dynamicManagerCompat = null },
            onSave = { updated ->
                dynamicManagerCompat = null
                scope.launch {
                    dynamicManagerBusy = true
                    try {
                        val result = KsuCli.setDynamicManagerUid(appInfo.uid, updated)
                        if (result.success) {
                            dynamicManagerChecked = true
                            dynamicManagerSignature = KsuCli.getDynamicManagerSignatureForUid(appInfo.uid)
                            superUserViewModel.refreshAppConfigurations()
                        } else {
                            dynamicManagerErrorTitle = failedToSetDynamicManager
                            dynamicManagerError = result.error
                        }
                    } finally {
                        dynamicManagerBusy = false
                    }
                }
            },
        )
    }

    LaunchedEffect(packageName, appInfo.uid) {
        if (appInfo.special) return@LaunchedEffect
        val flags = KsuCli.getDynamicManagerFlagsForUid(appInfo.uid)
        dynamicManagerFlags = flags
        val trusted = flags and Natives.DYNAMIC_MANAGER_FLAG_TRUSTED != 0
        dynamicManagerChecked = trusted
        dynamicManagerSignature = if (trusted) {
            KsuCli.getDynamicManagerSignatureForUid(appInfo.uid)
        } else {
            null
        }
    }

    val showDynamicManagerSwitch = remember(context, appInfo.special, dynamicManagerFlags, dynamicManagerChecked) {
        !appInfo.special && (DynamicManagerSettings.allowAnyApp(context) ||
            dynamicManagerChecked ||
            dynamicManagerFlags and Natives.DYNAMIC_MANAGER_FLAG_PRESET != 0)
    }

    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (isExpressiveUi || CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    Scaffold(
        topBar = {
            TopBar(
                title = if (isSharedUid) "UID ${appInfo.uid}" else appInfo.label,
                colors = getTopAppBarColors(
                    containerColor = cardColor,
                    scrolledContainerColor = cardColor
                ),
                onBack = dropUnlessResumed { navigator.popBackStack() },
                scrollBehavior = scrollBehavior
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        AppProfileInner(
            modifier = Modifier
                .padding(paddingValues)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .verticalScroll(rememberScrollState()),
            packageName = appInfo.displayIdentifier,
            isSpecialApp = appInfo.special,
            appLabel = appInfo.label,
            uid = appInfo.uid,
            uidApps = uidApps,
            appIcon = {
                AsyncImage(
                    model = ImageRequest.Builder(context).data(appInfo.packageInfo).crossfade(true).build(),
                    contentDescription = appInfo.label,
                    modifier = Modifier
                        .padding(4.dp)
                        .width(48.dp)
                        .height(48.dp)
                )
            },
            profile = profile,
            showDynamicManagerSwitch = showDynamicManagerSwitch,
            isDynamicManager = dynamicManagerChecked,
            dynamicManagerEnabled = !dynamicManagerBusy && dynamicManagerCompat == null,
            onDynamicManagerCompat = ::editDynamicManagerCompat,
            onViewTemplate = {
                getTemplateInfoById(it)?.let { info ->
                    navigator.navigate(TemplateEditorScreenDestination(info))
                }
            },
            onManageTemplate = {
                navigator.navigate(AppProfileTemplateScreenDestination())
            },
            onProfileChange = { updatedProfile ->
                scope.launch {
                    val normalizedProfile = updatedProfile.withoutLegacySepolicyDiagnostic(profileKey)
                    val profileToSave = if (appInfo.special) normalizedProfile.copy(allowSu = false) else normalizedProfile
                    if (profileToSave.allowSu) {
                        // sync with allowlist.c - forbid_system_uid
                        if (appInfo.uid < 2000 && appInfo.uid != 1000) {
                            snackBarHost.showSnackbar(suNotAllowed)
                            return@launch
                        }
                        val rulesSaved = saveProfileSepolicy(profile.rules, profileToSave) { rules ->
                            setSepolicy(profileKey, rules)
                        }
                        if (!rulesSaved) {
                            snackBarHost.showSnackbar(failToUpdateSepolicy)
                            return@launch
                        }
                    }
                    if (!Natives.setAppProfile(profileToSave)) {
                        snackBarHost.showSnackbar(failToUpdateAppProfile.format(appInfo.uid))
                    } else {
                        savedProfile = profileToSave
                        superUserViewModel.updateUidProfileLocally(appInfo.uid, profileToSave)
                        scope.launch {
                            superUserViewModel.refreshAppConfigurations()
                        }
                    }
                }
            },
            onDynamicManagerChange = { checked ->
                scope.launch {
                    dynamicManagerBusy = true
                    try {
                        if (checked) {
                            val hasKsud = try {
                                KsuCli.hasBundledKsud(appInfo.packageInfo.applicationInfo!!)
                            } catch (e: Exception) {
                                if (e is CancellationException) throw e
                                dynamicManagerErrorTitle = failedToSetDynamicManager
                                dynamicManagerError = resources.getString(
                                    R.string.dynamic_manager_error_read,
                                    e.message ?: e.javaClass.simpleName
                                )
                                return@launch
                            }
                            if (!hasKsud && dynamicManagerConfirmDialog.awaitConfirm(
                                    title = resources.getString(R.string.set_as_dynamic_manager),
                                    content = resources.getString(R.string.dynamic_manager_missing_ksud_warning),
                                    confirm = resources.getString(R.string.yes),
                                    dismiss = resources.getString(R.string.no)
                                ) != ConfirmResult.Confirmed
                            ) {
                                return@launch
                            }
                            try {
                                dynamicManagerCompat = KsuCli.getDynamicManagerCompat(appInfo.uid)
                            } catch (e: Exception) {
                                if (e is CancellationException) throw e
                                dynamicManagerErrorTitle = failedToSetDynamicManager
                                dynamicManagerError = e.message ?: e.javaClass.simpleName
                            }
                        } else {
                            val signature = dynamicManagerSignature
                                ?: KsuCli.getDynamicManagerSignatureForUid(appInfo.uid)
                            if (signature == null) {
                                snackBarHost.showSnackbar(dynamicManagerSignatureUnavailable)
                                return@launch
                            }
                            val result = KsuCli.deleteDynamicManager(signature)
                            if (result.success) {
                                dynamicManagerSignature = signature
                                dynamicManagerChecked = false
                                superUserViewModel.refreshAppConfigurations()
                            } else {
                                dynamicManagerErrorTitle = failedToDelDynamicManager
                                dynamicManagerError = result.error
                            }
                        }
                    } finally {
                        dynamicManagerBusy = false
                    }
                }
            },
        )
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun AppProfileInner(
    modifier: Modifier = Modifier,
    packageName: String,
    appLabel: String,
    uid: Int,
    uidApps: List<SuperUserViewModel.AppInfo>,
    appIcon: @Composable () -> Unit,
    profile: Natives.Profile,
    isSpecialApp: Boolean = false,
    showDynamicManagerSwitch: Boolean = false,
    isDynamicManager: Boolean = false,
    dynamicManagerEnabled: Boolean = true,
    onViewTemplate: (id: String) -> Unit = {},
    onManageTemplate: () -> Unit = {},
    onProfileChange: (Natives.Profile) -> Unit,
    onDynamicManagerChange: (Boolean) -> Unit = {},
    onDynamicManagerCompat: () -> Unit = {},
) {
    val isRootGranted = !isSpecialApp && profile.allowSu
    val cardColors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh)

    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            surface = if (isExpressiveUi || CardConfig.isCustomBackgroundEnabled) {
                Color.Transparent
            } else {
                MaterialTheme.colorScheme.surfaceContainerHigh
            }
        )
    ) {
        Column(modifier = modifier) {
            ProfileSurface(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                shape = MaterialTheme.shapes.medium,
                colors = cardColors,
                elevation = getCardElevation(),
            ) {
                if (uidApps.size > 1) {
                    ListItem(
                        content = {
                            Text(
                                text = "UID $uid",
                                style = MaterialTheme.typography.titleMedium
                            )
                        },
                        supportingContent = {
                            Text(
                                text = stringResource(R.string.group_contains_apps, uidApps.size),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        },
                        leadingContent = {
                            YukiIcon(
                                imageVector = Icons.Filled.Android,
                                contentDescription = null,
                                modifier = Modifier.size(48.dp),
                            )
                        },
                    )
                } else {
                    AppMenuBox(packageName, uid / 100000, enabled = !isSpecialApp) {
                        ListItem(
                            content = {
                                Text(
                                    text = appLabel,
                                    style = MaterialTheme.typography.titleMedium
                                )
                            },
                            supportingContent = {
                                Text(
                                    text = packageName,
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            },
                            leadingContent = appIcon,
                        )
                    }
                }
            }

            if (!isSpecialApp) ProfileSurface(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                shape = MaterialTheme.shapes.medium,
                colors = cardColors,
                elevation = getCardElevation(),
            ) {
                SwitchItem(
                    icon = Icons.Filled.Security,
                    title = stringResource(id = R.string.superuser),
                    checked = isRootGranted,
                    onCheckedChange = { onProfileChange(profile.copy(allowSu = it)) },
                )
            }

            if (showDynamicManagerSwitch) {
                ProfileSurface(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                    shape = MaterialTheme.shapes.medium,
                    colors = cardColors,
                    elevation = getCardElevation(),
                ) {
                    SwitchItem(
                        icon = Icons.Filled.AdminPanelSettings,
                        title = stringResource(id = R.string.set_as_dynamic_manager),
                        summary = stringResource(id = R.string.set_as_dynamic_manager_summary),
                        checked = isDynamicManager,
                        enabled = dynamicManagerEnabled,
                        onCheckedChange = onDynamicManagerChange,
                    )
                    YukiExpandableContent(visible = isDynamicManager) {
                        FilledTonalButton(
                            enabled = dynamicManagerEnabled,
                            onClick = onDynamicManagerCompat,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
                            shapes = if (isExpressiveUi) {
                                ButtonDefaults.shapes()
                            } else {
                                ButtonDefaults.shapes(
                                    shape = ButtonDefaults.shape,
                                    pressedShape = ButtonDefaults.shape,
                                )
                            },
                        ) {
                            Text(stringResource(R.string.dynamic_manager_compat))
                        }
                    }
                }
            }

            Crossfade(
                targetState = isRootGranted,
                animationSpec = LocalYukiMotion.current.effects(),
                label = "RootAccess"
            ) { current ->
                Column(
                    modifier = Modifier.padding(bottom = 6.dp + 48.dp + 6.dp /* SnackBar height */)
                ) {
                    if (current) {
                        val initialMode = if (profile.rootUseDefault) {
                            Mode.Default
                        } else if (profile.rootTemplate != null) {
                            Mode.Template
                        } else {
                            Mode.Custom
                        }
                        var mode by rememberSaveable {
                            mutableStateOf(initialMode)
                        }

                        ProfileSurface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 8.dp),
                            shape = MaterialTheme.shapes.medium,
                            colors = cardColors,
                            elevation = getCardElevation(),
                        ) {
                            ProfileBox(mode, true) {
                                // template mode shouldn't change profile here!
                                if (it == Mode.Default || it == Mode.Custom) {
                                    onProfileChange(
                                        profile.copy(
                                            rootUseDefault = it == Mode.Default,
                                            rootTemplate = null
                                        )
                                    )
                                }
                                mode = it
                            }
                        }

                        YukiExpandableContent(
                            visible = mode != Mode.Default,
                            large = true,
                        ) {
                            ProfileSurface(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 16.dp, vertical = 8.dp),
                                shape = MaterialTheme.shapes.medium,
                                colors = cardColors,
                                elevation = getCardElevation(),
                            ) {
                                Column(modifier = Modifier.padding(vertical = 8.dp)) {
                                    Crossfade(
                                        targetState = mode,
                                        animationSpec = LocalYukiMotion.current.effects(),
                                        label = "ProfileMode"
                                    ) { currentMode ->
                                        when (currentMode) {
                                            Mode.Template -> {
                                                TemplateConfig(
                                                    profile = profile,
                                                    onViewTemplate = onViewTemplate,
                                                    onManageTemplate = onManageTemplate,
                                                    onProfileChange = onProfileChange
                                                )
                                            }

                                            Mode.Custom -> {
                                                RootProfileConfig(
                                                    fixedName = true,
                                                    profile = profile,
                                                    onProfileChange = onProfileChange
                                                )
                                            }

                                            else -> {}
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        val mode = if (profile.nonRootUseDefault) Mode.Default else Mode.Custom

                        ProfileSurface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 8.dp),
                            shape = MaterialTheme.shapes.medium,
                            colors = cardColors,
                            elevation = getCardElevation(),
                        ) {
                            ProfileBox(mode, false) {
                                onProfileChange(profile.copy(nonRootUseDefault = (it == Mode.Default)))
                            }
                        }

                        YukiExpandableContent(
                            visible = mode == Mode.Custom,
                        ) {
                            ProfileSurface(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 16.dp, vertical = 8.dp),
                                shape = MaterialTheme.shapes.medium,
                                colors = cardColors,
                                elevation = getCardElevation(),
                            ) {
                                Column(modifier = Modifier.padding(vertical = 8.dp)) {
                                    AppProfileConfig(
                                        fixedName = true,
                                        profile = profile,
                                        enabled = mode == Mode.Custom,
                                        onProfileChange = onProfileChange
                                    )
                                }
                            }
                        }
                    }

                    if (uidApps.size > 1) {
                        SharedUidAppsCard(
                            apps = uidApps,
                            cardColors = cardColors,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ProfileSurface(
    modifier: Modifier = Modifier,
    shape: Shape = MaterialTheme.shapes.medium,
    colors: CardColors = CardDefaults.elevatedCardColors(),
    elevation: CardElevation = CardDefaults.elevatedCardElevation(),
    content: @Composable ColumnScope.() -> Unit,
) {
    if (isExpressiveUi) {
        Column(
            modifier = modifier
                .clip(MaterialTheme.shapes.large)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                ),
            content = content,
        )
    } else {
        ElevatedCard(
            modifier = modifier,
            shape = shape,
            colors = colors,
            elevation = elevation,
            content = content,
        )
    }
}

@Composable
private fun SharedUidAppsCard(
    apps: List<SuperUserViewModel.AppInfo>,
    cardColors: CardColors,
) {
    val context = LocalContext.current

    if (isExpressiveUi) {
        Text(
            text = stringResource(R.string.group_contains_apps, apps.size),
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Normal,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier
                .fillMaxWidth()
                .height(48.dp)
                .padding(horizontal = 24.dp, vertical = 14.dp),
        )
        apps.forEachIndexed { index, app ->
            ListItem(
                modifier = Modifier
                    .padding(
                        horizontal = 16.dp,
                        vertical = ListItemDefaults.SegmentedGap / 2,
                    )
                    .clip(ListItemDefaults.segmentedShapes(index, apps.size).shape)
                    .background(
                        MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                    ),
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                content = {
                    Text(
                        text = app.label,
                        fontWeight = FontWeight.Normal,
                        maxLines = 1,
                    )
                },
                supportingContent = {
                    Text(
                        text = app.packageName,
                        maxLines = 1,
                    )
                },
                leadingContent = {
                    AsyncImage(
                        model = ImageRequest.Builder(context)
                            .data(app.packageInfo)
                            .crossfade(true)
                            .build(),
                        contentDescription = app.label,
                        modifier = Modifier.size(40.dp),
                    )
                },
            )
        }
    } else {
        ElevatedCard(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            shape = MaterialTheme.shapes.medium,
            colors = cardColors,
            elevation = getCardElevation(),
        ) {
            Text(
                text = stringResource(R.string.group_contains_apps, apps.size),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            )
            apps.forEachIndexed { index, app ->
                if (index > 0) {
                    HorizontalDivider(modifier = Modifier.padding(horizontal = 16.dp))
                }
                ListItem(
                    content = {
                        Text(
                            text = app.label,
                            maxLines = 1,
                        )
                    },
                    supportingContent = {
                        Text(
                            text = app.packageName,
                            maxLines = 1,
                        )
                    },
                    leadingContent = {
                        AsyncImage(
                            model = ImageRequest.Builder(context)
                                .data(app.packageInfo)
                                .crossfade(true)
                                .build(),
                            contentDescription = app.label,
                            modifier = Modifier.size(40.dp),
                        )
                    },
                )
            }
        }
    }
}

private enum class Mode(@param:StringRes private val res: Int) {
    Default(R.string.profile_default), Template(R.string.profile_template), Custom(R.string.profile_custom);

    val text: String
        @Composable get() = stringResource(res)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    title: String,
    onBack: () -> Unit,
    colors: TopAppBarColors,
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val titleContent: @Composable () -> Unit = {
        Text(
            text = title,
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
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
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = titleContent,
            colors = colors,
            navigationIcon = navigationIcon,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    } else {
        TopAppBar(
            title = titleContent,
            colors = colors,
            navigationIcon = navigationIcon,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
            modifier = Modifier.shadow(
                elevation = if ((scrollBehavior?.state?.overlappedFraction ?: 0f) > 0.01f) {
                    4.dp
                } else {
                    0.dp
                },
            ),
        )
    }
}

@Composable
private fun ProfileBox(
    mode: Mode,
    hasTemplate: Boolean,
    onModeChange: (Mode) -> Unit,
) {
    Column(modifier = Modifier.padding(vertical = 8.dp)) {
        ListItem(
            content = {
                Text(
                    text = stringResource(R.string.profile),
                    style = MaterialTheme.typography.titleMedium
                )
            },
            supportingContent = {
                Text(
                    text = mode.text,
                    style = MaterialTheme.typography.bodyMedium,
                )
            },
            leadingContent = {
                YukiIcon(
                    imageVector = Icons.Filled.AdminPanelSettings,
                    contentDescription = null,
                )
            },
        )

        HorizontalDivider(
            thickness = Dp.Hairline,
        )

        ListItem(
            content = {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally)
                ) {
                    FilterChip(
                        selected = mode == Mode.Default,
                        onClick = { onModeChange(Mode.Default) },
                        label = {
                            Text(
                                text = stringResource(R.string.profile_default),
                                style = MaterialTheme.typography.bodyMedium
                            )
                        },
                        shape = MaterialTheme.shapes.small
                    )

                    if (hasTemplate) {
                        FilterChip(
                            selected = mode == Mode.Template,
                            onClick = { onModeChange(Mode.Template) },
                            label = {
                                Text(
                                    text = stringResource(R.string.profile_template),
                                    style = MaterialTheme.typography.bodyMedium
                                )
                            },
                            shape = MaterialTheme.shapes.small
                        )
                    }

                    FilterChip(
                        selected = mode == Mode.Custom,
                        onClick = { onModeChange(Mode.Custom) },
                        label = {
                            Text(
                                text = stringResource(R.string.profile_custom),
                                style = MaterialTheme.typography.bodyMedium
                            )
                        },
                        shape = MaterialTheme.shapes.small
                    )
                }
            }
        )
    }
}

@SuppressLint("UnusedBoxWithConstraintsScope")
@Composable
private fun AppMenuBox(
    packageName: String,
    userId: Int,
    enabled: Boolean = true,
    content: @Composable () -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    var touchPoint: Offset by remember { mutableStateOf(Offset.Zero) }
    val density = LocalDensity.current

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures(
                    onLongPress = {
                        touchPoint = it
                        expanded = true
                    }
                )
            }
    ) {
        content()

        val (offsetX, offsetY) = with(density) {
            (touchPoint.x.toDp()) to (-touchPoint.y.toDp())
        }

        DropdownMenu(
            expanded = expanded,
            modifier = Modifier.clickHapticFeedback(),
            offset = DpOffset(offsetX, offsetY),
            onDismissRequest = {
                expanded = false
            }
        ) {
            AppMenuOption(
                text = stringResource(id = R.string.launch_app),
                onClick = {
                    expanded = false
                    launchApp(packageName, userId)
                }
            )

            AppMenuOption(
                text = stringResource(id = R.string.force_stop_app),
                onClick = {
                    expanded = false
                    forceStopApp(packageName, userId)
                }
            )

            AppMenuOption(
                text = stringResource(id = R.string.restart_app),
                onClick = {
                    expanded = false
                    restartApp(packageName, userId)
                }
            )
        }
    }
}

@Composable
private fun AppMenuOption(text: String, onClick: () -> Unit) {
    DropdownMenuItem(
        text = {
            Text(
                text = text,
                style = MaterialTheme.typography.bodyMedium
            )
        },
        onClick = onClick
    )
}

@Preview
@Composable
private fun AppProfilePreview() {
    var profile by remember { mutableStateOf(Natives.Profile("")) }
    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            surface = if (CardConfig.isCustomBackgroundEnabled) Color.Transparent else MaterialTheme.colorScheme.surfaceContainerHigh
        )
    ) {
        Surface {
            AppProfileInner(
                packageName = "icu.nullptr.test",
                appLabel = "Test",
                uid = 10000,
                uidApps = emptyList(),
                appIcon = {
                    YukiIcon(
                        imageVector = Icons.Filled.Android,
                        contentDescription = null,
                    )
                },
                profile = profile,
                onProfileChange = {
                    profile = it
                },
            )
        }
    }
}
