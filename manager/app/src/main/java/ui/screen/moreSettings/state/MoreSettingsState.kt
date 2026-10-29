package ui.screen.moreSettings.state

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import ui.screen.moreSettings.util.LocaleHelper
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.activity.util.PREDICTIVE_BACK_PREFERENCE
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.ThemeConfig
import com.anatdx.yukisu.ui.theme.UiStyle
import com.anatdx.yukisu.ui.util.DynamicManagerSettings

@Stable
class MoreSettingsState(
    val context: Context,
    val prefs: SharedPreferences,
    val systemIsDark: Boolean
) {
    var themeMode by mutableIntStateOf(
        when (ThemeConfig.forceDarkMode) {
            true -> 2 // 深色
            false -> 1 // 浅色
            null -> 0 // 跟随系统
        }
    )

    var useDynamicColor by mutableStateOf(ThemeConfig.useDynamicColor)
    var uiStyleIndex by mutableIntStateOf(
        if (ThemeConfig.uiStyle == UiStyle.Expressive) 1 else 0
    )

    var showLanguageDialog by mutableStateOf(false)
    var currentAppLocale by mutableStateOf(LocaleHelper.getCurrentAppLocale(context))

    var showThemeModeDialog by mutableStateOf(false)
    var showUiStyleDialog by mutableStateOf(false)
    var showMotionPresetDialog by mutableStateOf(false)
    var showPageTransitionStyleDialog by mutableStateOf(false)
    var showThemeColorDialog by mutableStateOf(false)
    var showDpiConfirmDialog by mutableStateOf(false)

    var isSimpleMode by mutableStateOf(prefs.getBoolean("is_simple_mode", false))
    var predictiveBackEnabled by mutableStateOf(prefs.getBoolean(PREDICTIVE_BACK_PREFERENCE, false))
    var isHideVersion by mutableStateOf(prefs.getBoolean("is_hide_version", false))
    var isHideOtherInfo by mutableStateOf(prefs.getBoolean("is_hide_other_info", false))
    var isHideZygiskImplement by mutableStateOf(prefs.getBoolean("is_hide_zygisk_Implement", false))
    var isHideSeccompStatus by mutableStateOf(prefs.getBoolean("is_hide_seccomp_status", false))
    var isHideMetaModuleImplement by mutableStateOf(prefs.getBoolean("is_hide_meta_module_Implement", false))
    var isHideLinkCard by mutableStateOf(prefs.getBoolean("is_hide_link_card", false))
    var isHideTagRow by mutableStateOf(prefs.getBoolean("is_hide_tag_row", false))
    var isKernelSimpleMode by mutableStateOf(prefs.getBoolean("is_kernel_simple_mode", false))
    var showMoreModuleInfo by mutableStateOf(prefs.getBoolean("show_more_module_info", false))
    var moduleDescriptionMaxLines by mutableIntStateOf(
        prefs.getInt("module_description_max_lines", 4).coerceIn(1, 5)
    )
    var enableWebDebugging by mutableStateOf(prefs.getBoolean("enable_web_debugging", false))
    var useWebUIXEruda by mutableStateOf(prefs.getBoolean("use_webuix_eruda", false))
    var webuiEngine by mutableStateOf(prefs.getString("webui_engine", "default") ?: "default")
    var allowAnyDynamicManager by mutableStateOf(
        prefs.getBoolean(DynamicManagerSettings.KEY_ALLOW_ANY_DYNAMIC_MANAGER, false)
    )

    var selinuxEnabled by mutableStateOf(false)

    var cardAlpha by mutableFloatStateOf(CardConfig.cardAlpha)
    var cardDim by mutableFloatStateOf(CardConfig.cardDim)
    var isCustomBackgroundEnabled by mutableStateOf(ThemeConfig.customBackgroundUri != null)

    val systemDpi = context.resources.displayMetrics.densityDpi
    var currentDpi by mutableIntStateOf(prefs.getInt("app_dpi", systemDpi))
    var tempDpi by mutableIntStateOf(currentDpi)
    var isDpiCustom by mutableStateOf(true)

    val themeOptions = listOf(
        context.getString(R.string.theme_follow_system),
        context.getString(R.string.theme_light),
        context.getString(R.string.theme_dark)
    )

    val uiStyleOptions = listOf(
        context.getString(R.string.ui_style_classic),
        context.getString(R.string.ui_style_expressive)
    )

    val motionPresetOptions = listOf(
        context.getString(R.string.motion_preset_off),
        context.getString(R.string.motion_preset_soft),
        context.getString(R.string.motion_preset_qbouncy),
    )

    val pageTransitionStyleOptions = listOf(
        context.getString(R.string.page_transition_reveal),
        context.getString(R.string.page_transition_slide),
    )

    val dpiPresets = mapOf(
        context.getString(R.string.dpi_size_small) to 240,
        context.getString(R.string.dpi_size_medium) to 320,
        context.getString(R.string.dpi_size_large) to 420,
        context.getString(R.string.dpi_size_extra_large) to 560
    )
}
