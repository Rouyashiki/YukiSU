package ui.screen.moreSettings

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.ui.component.KsuIsValid
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.*
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.yalantis.ucrop.UCrop
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.ColorCircle
import ui.screen.moreSettings.component.LanguageSelectionDialog
import ui.screen.moreSettings.component.MoreSettingsDialogs
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingItem
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SettingsControlGroup
import ui.screen.moreSettings.component.SettingsDivider
import ui.screen.moreSettings.component.SwitchSettingItem
import ui.screen.moreSettings.state.MoreSettingsState
import ui.screen.moreSettings.util.LocaleHelper
import kotlin.math.roundToInt

@SuppressLint("LocalContextConfigurationRead", "LocalContextResourcesRead", "ObsoleteSdkInt")
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun MoreSettingsScreen(
    navigator: DestinationsNavigator
) {

    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    val systemIsDark = isSystemInDarkTheme()

    val settingsState = remember { MoreSettingsState(context, prefs, systemIsDark) }
    val settingsHandlers = remember { MoreSettingsHandlers(context, prefs, settingsState) }
    val cropToolbarColor = MaterialTheme.colorScheme.surfaceContainerHighest
    val cropToolbarWidgetColor = MaterialTheme.colorScheme.onSurface
    val cropAccentColor = MaterialTheme.colorScheme.primary
    val cropToolbarTitle = stringResource(R.string.settings_custom_background)

    val cropImageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        when (result.resultCode) {
            Activity.RESULT_OK -> {
                val croppedUri = result.data?.let(UCrop::getOutput)
                if (croppedUri != null) {
                    settingsHandlers.handleCustomBackground(croppedUri)
                } else {
                    Log.e("WallpaperCropper", "Crop completed without an output Uri")
                    Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
                }
            }

            UCrop.RESULT_ERROR -> {
                val error = result.data?.let(UCrop::getError)
                Log.e("WallpaperCropper", "Failed to crop custom background", error)
                Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    val pickImageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri ->
        uri?.let {
            try {
                cropImageLauncher.launch(
                    createWallpaperCropIntent(
                        context = context,
                        sourceUri = it,
                        toolbarTitle = cropToolbarTitle,
                        style = WallpaperCropStyle(
                            toolbarColor = cropToolbarColor.toArgb(),
                            toolbarWidgetColor = cropToolbarWidgetColor.toArgb(),
                            accentColor = cropAccentColor.toArgb(),
                            lightStatusBar = cropToolbarColor.luminance() > 0.5f,
                        ),
                    )
                )
            } catch (error: Exception) {
                Log.e("WallpaperCropper", "Failed to launch custom background cropper", error)
                Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    LaunchedEffect(Unit) {
        settingsHandlers.initializeSettings()
    }

    MoreSettingsDialogs(
        state = settingsState,
        handlers = settingsHandlers
    )

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            MoreSettingsTopBar(
                onBack = { navigator.popBackStack() },
                scrollBehavior = scrollBehavior
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(top = 8.dp)
        ) {

            AppearanceSettings(
                state = settingsState,
                handlers = settingsHandlers,
                pickImageLauncher = pickImageLauncher,
                coroutineScope = coroutineScope
            )


            CustomizationSettings(
                state = settingsState,
                handlers = settingsHandlers
            )


            KsuIsValid {
                AdvancedSettings(
                    state = settingsState,
                    handlers = settingsHandlers,
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MoreSettingsTopBar(
    onBack: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior
) {
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.more_settings),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null
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

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior
        )
    } else {
        TopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior
        )
    }
}

@Composable
private fun AppearanceSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    SettingsCard(title = stringResource(R.string.appearance_settings)) {

        LanguageSetting(state = state)

        SettingItem(
            icon = Icons.Default.AutoAwesome,
            title = stringResource(R.string.ui_style),
            subtitle = state.uiStyleOptions[state.uiStyleIndex],
            onClick = { state.showUiStyleDialog = true }
        )

        SettingItem(
            icon = Icons.Default.Animation,
            title = stringResource(R.string.motion_preset_title),
            subtitle = state.motionPresetOptions[ThemeConfig.motionPreset.ordinal],
            onClick = { state.showMotionPresetDialog = true },
        )

        SettingItem(
            icon = Icons.Default.OpenInFull,
            title = stringResource(R.string.page_transition_title),
            subtitle = state.pageTransitionStyleOptions[ThemeConfig.pageTransitionStyle.ordinal],
            onClick = { state.showPageTransitionStyleDialog = true },
        )

        SwitchSettingItem(
            icon = Icons.AutoMirrored.Filled.ArrowBack,
            title = stringResource(R.string.predictive_back_gesture),
            summary = stringResource(R.string.predictive_back_gesture_summary),
            checked = state.predictiveBackEnabled,
            onChange = handlers::handlePredictiveBackChange
        )

        SettingItem(
            icon = Icons.Default.DarkMode,
            title = stringResource(R.string.theme_mode),
            subtitle = state.themeOptions[state.themeMode],
            onClick = { state.showThemeModeDialog = true }
        )


        SwitchSettingItem(
            icon = Icons.Filled.ColorLens,
            title = stringResource(R.string.dynamic_color_title),
            summary = stringResource(R.string.dynamic_color_summary),
            checked = state.useDynamicColor,
            onChange = handlers::handleDynamicColorChange
        )


        YukiExpandableContent(visible = !state.useDynamicColor) {
            ThemeColorSelection(state = state)
        }

        SettingsDivider()


        DpiSettings(state = state, handlers = handlers)

        SettingsDivider()


        CustomBackgroundSettings(
            state = state,
            handlers = handlers,
            pickImageLauncher = pickImageLauncher,
            coroutineScope = coroutineScope
        )
    }
}

@Composable
private fun CustomizationSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsCard(title = stringResource(R.string.custom_settings)) {

        SwitchSettingItem(
            icon = Icons.Filled.Info,
            title = stringResource(R.string.show_more_module_info),
            summary = stringResource(R.string.show_more_module_info_summary),
            checked = state.showMoreModuleInfo,
            groupPosition = MoreSettingsItemPosition.First,
            onChange = handlers::handleShowMoreModuleInfoChange
        )

        ModuleDescriptionLinesSetting(state, handlers)


        SwitchSettingItem(
            icon = Icons.Filled.Brush,
            title = stringResource(R.string.simple_mode),
            summary = stringResource(R.string.simple_mode_summary),
            checked = state.isSimpleMode,
            onChange = handlers::handleSimpleModeChange
        )

        SwitchSettingItem(
            icon = Icons.Filled.Brush,
            title = stringResource(R.string.kernel_simple_kernel),
            summary = stringResource(R.string.kernel_simple_kernel_summary),
            checked = state.isKernelSimpleMode,
            onChange = handlers::handleKernelSimpleModeChange
        )

        HideOptionsSettings(state = state, handlers = handlers)
    }
}

@Composable
private fun ModuleDescriptionLinesSetting(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsControlGroup {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            YukiIcon(
                Icons.Outlined.Description,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(24.dp)
            )
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.settings_module_description_max_lines),
                    style = MaterialTheme.typography.titleSmall
                )
                Text(
                    text = stringResource(R.string.settings_module_description_max_lines_summary),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Text(
                text = "${state.moduleDescriptionMaxLines} " + stringResource(R.string.unit_lines),
                style = MaterialTheme.typography.labelMedium
            )
        }
        val sliderState = rememberSliderState(steps = 3, trackRange = 1f..5f)
        sliderState.value = state.moduleDescriptionMaxLines.toFloat()
        Slider(
            state = sliderState,
            onValueChange = { handlers.handleModuleDescriptionMaxLinesChange(it.toInt()) }
        )
    }
}

@Composable
private fun HideOptionsSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {

    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_kernel_kernelsu_version),
        summary = stringResource(R.string.hide_kernel_kernelsu_version_summary),
        checked = state.isHideVersion,
        onChange = handlers::handleHideVersionChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_other_info),
        summary = stringResource(R.string.hide_other_info_summary),
        checked = state.isHideOtherInfo,
        onChange = handlers::handleHideOtherInfoChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_seccomp_status),
        summary = stringResource(R.string.hide_seccomp_status_summary),
        checked = state.isHideSeccompStatus,
        onChange = handlers::handleHideSeccompStatusChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_zygisk_implement),
        summary = stringResource(R.string.hide_zygisk_implement_summary),
        checked = state.isHideZygiskImplement,
        onChange = handlers::handleHideZygiskImplementChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_meta_module_implement),
        summary = stringResource(R.string.hide_meta_module_implement_summary),
        checked = state.isHideMetaModuleImplement,
        onChange = handlers::handleHideMetaModuleImplementChange
    )

    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_link_card),
        summary = stringResource(R.string.hide_link_card_summary),
        checked = state.isHideLinkCard,
        onChange = handlers::handleHideLinkCardChange
    )


    SwitchSettingItem(
        icon = Icons.Filled.VisibilityOff,
        title = stringResource(R.string.hide_tag_card),
        summary = stringResource(R.string.hide_tag_card_summary),
        checked = state.isHideTagRow,
        groupPosition = MoreSettingsItemPosition.Last,
        onChange = handlers::handleHideTagRowChange
    )
}

@Composable
private fun AdvancedSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
) {
    SettingsCard(title = stringResource(R.string.advanced_settings)) {
        SwitchSettingItem(
            icon = Icons.Filled.Security,
            title = stringResource(R.string.selinux),
            summary = if (state.selinuxEnabled)
                stringResource(R.string.selinux_enabled) else
                stringResource(R.string.selinux_disabled),
            checked = state.selinuxEnabled,
            groupPosition = MoreSettingsItemPosition.First,
            onChange = handlers::handleSelinuxChange
        )

        SwitchSettingItem(
            icon = Icons.Filled.AdminPanelSettings,
            title = stringResource(R.string.allow_any_dynamic_manager),
            summary = stringResource(R.string.allow_any_dynamic_manager_summary),
            checked = state.allowAnyDynamicManager,
            groupPosition = if (BuildConfig.DEBUG) {
                MoreSettingsItemPosition.Middle
            } else {
                MoreSettingsItemPosition.Last
            },
            onChange = handlers::handleAllowAnyDynamicManagerChange
        )

        // Web debugging and the Eruda console it feeds are developer tools, and
        // the Eruda bundle is stripped from release APKs, so the toggles would
        // have nothing to switch on there.
        if (BuildConfig.DEBUG) {
            SettingsDivider()

            SwitchSettingItem(
                icon = Icons.Filled.DeveloperMode,
                title = stringResource(R.string.enable_web_debugging),
                summary = stringResource(R.string.enable_web_debugging_summary),
                checked = state.enableWebDebugging,
                groupPosition = if (state.enableWebDebugging && state.webuiEngine == "wx") {
                    MoreSettingsItemPosition.Middle
                } else {
                    MoreSettingsItemPosition.Last
                },
                onChange = handlers::handleWebDebuggingChange
            )

            YukiExpandableContent(visible = state.enableWebDebugging && state.webuiEngine == "wx") {
                SwitchSettingItem(
                    icon = Icons.Filled.FormatListNumbered,
                    title = stringResource(R.string.use_webuix_eruda),
                    summary = stringResource(R.string.use_webuix_eruda_summary),
                    checked = state.useWebUIXEruda,
                    groupPosition = MoreSettingsItemPosition.Last,
                    onChange = handlers::handleWebUIXErudaChange
                )
            }
        }
    }
}

@Composable
private fun ThemeColorSelection(state: MoreSettingsState) {
    SettingItem(
        icon = Icons.Default.Palette,
        title = stringResource(R.string.theme_color),
        subtitle = when (ThemeConfig.currentTheme) {
            is ThemeColors.Green -> stringResource(R.string.color_green)
            is ThemeColors.Purple -> stringResource(R.string.color_purple)
            is ThemeColors.Orange -> stringResource(R.string.color_orange)
            is ThemeColors.Pink -> stringResource(R.string.color_pink)
            is ThemeColors.Gray -> stringResource(R.string.color_gray)
            is ThemeColors.Yellow -> stringResource(R.string.color_yellow)
            is ThemeColors.TransPride -> stringResource(R.string.color_trans_pride)  // 🏳️‍⚧️
            else -> stringResource(R.string.color_default)
        },
        onClick = { state.showThemeColorDialog = true },
        trailingContent = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(start = 8.dp)
            ) {
                val theme = ThemeConfig.currentTheme
                val isDark = isSystemInDarkTheme()

                ColorCircle(
                    color = if (isDark) theme.primaryDark else theme.primaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.secondaryDark else theme.secondaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.tertiaryDark else theme.tertiaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
            }
        }
    )
}

@Composable
private fun DpiSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingItem(
        icon = Icons.Default.FormatSize,
        title = stringResource(R.string.app_dpi_title),
        subtitle = stringResource(R.string.app_dpi_summary),
        onClick = {},
        trailingContent = {
            Text(
                text = handlers.getDpiFriendlyName(state.tempDpi),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary
            )
        }
    )


    DpiSliderControls(state = state, handlers = handlers)
}

@Composable
private fun DpiSliderControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsControlGroup {
        val sliderValue by animateFloatAsState(
            targetValue = state.tempDpi.toFloat(),
            animationSpec = LocalYukiMotion.current.effects(),
            label = "DPI Slider Animation"
        )
        val sliderState = rememberSliderState(steps = 11, trackRange = 160f..600f)
        sliderState.value = sliderValue

        Slider(
            state = sliderState,
            onValueChange = { newValue ->
                state.tempDpi = newValue.toInt()
                state.isDpiCustom = !state.dpiPresets.containsValue(state.tempDpi)
            },
            colors = SliderDefaults.colors(
                thumbColor = MaterialTheme.colorScheme.primary,
                activeTrackColor = MaterialTheme.colorScheme.primary,
                inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
            )
        )

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
        ) {
            state.dpiPresets.forEach { (name, dpi) ->
                val isSelected = state.tempDpi == dpi
                val buttonColor = if (isSelected)
                    MaterialTheme.colorScheme.primaryContainer
                else
                    MaterialTheme.colorScheme.surfaceVariant

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 2.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(buttonColor)
                        .clickable {
                            state.tempDpi = dpi
                            state.isDpiCustom = false
                        }
                        .padding(vertical = 8.dp, horizontal = 4.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = name,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (isSelected)
                            MaterialTheme.colorScheme.onPrimaryContainer
                        else
                            MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }

        Text(
            text = if (state.isDpiCustom)
                "${stringResource(R.string.dpi_size_custom)}: ${state.tempDpi}"
            else
                "${handlers.getDpiFriendlyName(state.tempDpi)}: ${state.tempDpi}",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(top = 8.dp)
        )

        Button(
            onClick = { state.showDpiConfirmDialog = true },
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            enabled = state.tempDpi != state.currentDpi
        ) {
            YukiIcon(
                Icons.Default.Check,
                contentDescription = null,
                modifier = Modifier.size(16.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(stringResource(R.string.dpi_apply_settings))
        }
    }
}

@Composable
private fun CustomBackgroundSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    val hasCustomBackground = ThemeConfig.customBackgroundUri != null
    SwitchSettingItem(
        icon = Icons.Filled.Wallpaper,
        title = stringResource(id = R.string.settings_custom_background),
        summary = stringResource(id = R.string.settings_custom_background_summary),
        checked = state.isCustomBackgroundEnabled,
        groupPosition = if (hasCustomBackground) {
            MoreSettingsItemPosition.Middle
        } else {
            MoreSettingsItemPosition.Last
        },
        onChange = { isChecked ->
            if (isChecked) {
                pickImageLauncher.launch("image/*")
            } else {
                handlers.handleRemoveCustomBackground()
            }
        }
    )

    YukiExpandableContent(visible = hasCustomBackground, large = true) {
        Column {
            SettingItem(
                icon = Icons.Filled.Image,
                title = stringResource(R.string.settings_change_background),
                subtitle = stringResource(R.string.settings_change_background_summary),
                groupPosition = MoreSettingsItemPosition.Middle,
                onClick = { pickImageLauncher.launch("image/*") }
            )

            BackgroundAdjustmentControls(
                state = state,
                handlers = handlers,
                coroutineScope = coroutineScope
            )
        }
    }
}

@Composable
private fun BackgroundAdjustmentControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    SettingsControlGroup(groupPosition = MoreSettingsItemPosition.Last) {
        AlphaSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)

        DimSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)
    }
}

@Composable
private fun AlphaSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(bottom = 4.dp)
    ) {
        YukiIcon(
            Icons.Filled.Opacity,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_alpha),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardAlpha * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val alphaSliderValue by animateFloatAsState(
        targetValue = state.cardAlpha,
        animationSpec = LocalYukiMotion.current.effects(),
        label = "Alpha Slider Animation"
    )
    val sliderState = rememberSliderState(steps = 20, trackRange = 0f..1f)
    sliderState.value = alphaSliderValue

    Slider(
        state = sliderState,
        onValueChange = { newValue ->
            handlers.handleCardAlphaChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(Dispatchers.IO) {
                saveCardConfig(handlers.context)
            }
        },
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

@Composable
private fun DimSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(top = 16.dp, bottom = 4.dp)
    ) {
        YukiIcon(
            Icons.Filled.LightMode,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_dim),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardDim * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val dimSliderValue by animateFloatAsState(
        targetValue = state.cardDim,
        animationSpec = LocalYukiMotion.current.effects(),
        label = "Dim Slider Animation"
    )
    val sliderState = rememberSliderState(steps = 20, trackRange = 0f..1f)
    sliderState.value = dimSliderValue

    Slider(
        state = sliderState,
        onValueChange = { newValue ->
            handlers.handleCardDimChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(Dispatchers.IO) {
                saveCardConfig(handlers.context)
            }
        },
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

fun saveCardConfig(context: Context) {
    CardConfig.save(context)
}

@Composable
private fun LanguageSetting(state: MoreSettingsState) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val language = stringResource(id = R.string.settings_language)

    // Compute display name based on current app locale
    val currentLanguageDisplay = remember(state.currentAppLocale, resources.configuration) {
        val locale = state.currentAppLocale
        if (locale != null) {
            locale.getDisplayName(locale)
        } else {
            resources.getString(R.string.language_system_default)
        }
    }

    SettingItem(
        icon = Icons.Filled.Translate,
        title = language,
        subtitle = currentLanguageDisplay,
        groupPosition = MoreSettingsItemPosition.First,
        onClick = { state.showLanguageDialog = true }
    )

    // Language Selection Dialog
    if (state.showLanguageDialog) {
        LanguageSelectionDialog(
            onLanguageSelected = { newLocale ->
                // Update local state immediately
                state.currentAppLocale = LocaleHelper.getCurrentAppLocale(context)
                // Apply locale change immediately for Android < 13
                LocaleHelper.restartActivity(context)
            },
            onDismiss = { state.showLanguageDialog = false }
        )
    }
}
