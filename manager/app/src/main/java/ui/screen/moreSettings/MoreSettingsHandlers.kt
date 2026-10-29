package ui.screen.moreSettings

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.res.Configuration
import android.net.Uri
import android.widget.Toast
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CleaningServices
import androidx.compose.material.icons.filled.Groups
import androidx.compose.material.icons.filled.Scanner
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.core.content.edit
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.activity.util.PREDICTIVE_BACK_PREFERENCE
import com.anatdx.yukisu.ui.component.ConfirmResult
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.screen.SettingItem
import com.anatdx.yukisu.ui.screen.SwitchItem
import com.anatdx.yukisu.ui.theme.*
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.util.DynamicManagerSettings
import com.anatdx.yukisu.ui.util.*
import com.anatdx.yukisu.ui.util.getRootShell
import com.anatdx.yukisu.ui.util.isSELinuxEnforcing
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.moreSettings.state.MoreSettingsState

class MoreSettingsHandlers(
    val context: Context,
    private val prefs: SharedPreferences,
    private val state: MoreSettingsState
) {

    fun initializeSettings() {
        CardConfig.load(context)
        state.cardAlpha = CardConfig.cardAlpha
        state.cardDim = CardConfig.cardDim
        state.isCustomBackgroundEnabled = ThemeConfig.customBackgroundUri != null

        state.themeMode = when (ThemeConfig.forceDarkMode) {
            true -> 2
            false -> 1
            null -> 0
        }
        state.uiStyleIndex = if (ThemeConfig.uiStyle == UiStyle.Expressive) 1 else 0

        // 确保卡片样式跟随主题模式
        when (state.themeMode) {
            2 -> { // 深色
                CardConfig.isUserDarkModeEnabled = true
                CardConfig.isUserLightModeEnabled = false
            }
            1 -> { // 浅色
                CardConfig.isUserDarkModeEnabled = false
                CardConfig.isUserLightModeEnabled = true
            }
            0 -> { // 跟随系统
                CardConfig.isUserDarkModeEnabled = false
                CardConfig.isUserLightModeEnabled = false
            }
        }

        // 如果启用了系统跟随且系统是深色模式，应用深色模式默认值
        if (state.themeMode == 0 && state.systemIsDark) {
            CardConfig.setThemeDefaults(true)
        }

        state.currentDpi = prefs.getInt("app_dpi", state.systemDpi)
        state.tempDpi = state.currentDpi

        CardConfig.save(context)

        state.selinuxEnabled = isSELinuxEnforcing()
    }

    fun handleThemeModeChange(index: Int) {
        state.themeMode = index
        val newThemeMode = when (index) {
            0 -> null // 跟随系统
            1 -> false // 浅色
            2 -> true // 深色
            else -> null
        }
        context.saveThemeMode(newThemeMode)
        ThemeConfig.updateTheme(darkMode = newThemeMode)

        when (index) {
            2 -> { // 深色
                ThemeConfig.updateTheme(darkMode = true)
                CardConfig.updateThemePreference(darkMode = true, lightMode = false)
                CardConfig.setThemeDefaults(true)
                CardConfig.save(context)
            }
            1 -> { // 浅色
                ThemeConfig.updateTheme(darkMode = false)
                CardConfig.updateThemePreference(darkMode = false, lightMode = true)
                CardConfig.setThemeDefaults(false)
                CardConfig.save(context)
            }
            0 -> { // 跟随系统
                ThemeConfig.updateTheme(darkMode = null)
                CardConfig.updateThemePreference(darkMode = null, lightMode = null)
                val isNightModeActive = (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
                CardConfig.setThemeDefaults(isNightModeActive)
                CardConfig.save(context)
            }
        }
    }

    fun handleUiStyleChange(index: Int) {
        val uiStyle = if (index == 1) UiStyle.Expressive else UiStyle.Classic
        state.uiStyleIndex = index.coerceIn(0, 1)
        context.saveUiStyle(uiStyle)
    }

    fun handleMotionPresetChange(index: Int) {
        val preset = MotionPreset.entries.getOrNull(index) ?: MotionPreset.QBouncy
        context.saveMotionPreset(preset)
    }

    fun handlePageTransitionStyleChange(index: Int) {
        val style = PageTransitionStyle.entries.getOrNull(index) ?: PageTransitionStyle.Reveal
        context.savePageTransitionStyle(style)
    }

    fun handleThemeColorChange(theme: ThemeColors) {
        context.saveThemeColors(when (theme) {
            ThemeColors.Green -> "green"
            ThemeColors.Purple -> "purple"
            ThemeColors.Orange -> "orange"
            ThemeColors.Pink -> "pink"
            ThemeColors.Gray -> "gray"
            ThemeColors.Yellow -> "yellow"
            ThemeColors.TransPride -> "trans"
            else -> "default"
        })
        ThemeConfig.updateTheme(theme = theme)
    }

    fun handleDynamicColorChange(enabled: Boolean) {
        state.useDynamicColor = enabled
        context.saveDynamicColorState(enabled)
        ThemeConfig.updateTheme(dynamicColor = enabled)
    }

    @Composable
    fun getDpiFriendlyName(dpi: Int): String {
        return when (dpi) {
            240 -> stringResource(R.string.dpi_size_small)
            320 -> stringResource(R.string.dpi_size_medium)
            420 -> stringResource(R.string.dpi_size_large)
            560 -> stringResource(R.string.dpi_size_extra_large)
            else -> stringResource(R.string.dpi_size_custom)
        }
    }

    fun handleDpiApply() {
        if (state.tempDpi != state.currentDpi) {
            prefs.edit {
                putInt("app_dpi", state.tempDpi)
            }

            state.currentDpi = state.tempDpi
            Toast.makeText(
                context,
                context.getString(R.string.dpi_applied_success, state.tempDpi),
                Toast.LENGTH_SHORT
            ).show()

            val restartIntent = context.packageManager.getLaunchIntentForPackage(context.packageName)
            restartIntent?.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(restartIntent)

            state.showDpiConfirmDialog = false
        }
    }

    fun handleCustomBackground(transformedUri: Uri) {
        context.saveAndApplyCustomBackground(transformedUri)
        state.isCustomBackgroundEnabled = true
        CardConfig.updateBackground(true)
        saveCardConfig(context)

        Toast.makeText(
            context,
            context.getString(R.string.background_set_success),
            Toast.LENGTH_SHORT
        ).show()
    }

    fun handleRemoveCustomBackground() {
        context.saveCustomBackground(null)
        state.isCustomBackgroundEnabled = false
        CardConfig.cardAlpha = 1f
        CardConfig.cardDim = 0f
        CardConfig.isCustomAlphaSet = false
        CardConfig.isCustomDimSet = false
        state.cardAlpha = CardConfig.cardAlpha
        state.cardDim = CardConfig.cardDim
        saveCardConfig(context)
        ThemeConfig.preventBackgroundRefresh = false

        context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE).edit {
            putBoolean("prevent_background_refresh", false)
        }

        Toast.makeText(
            context,
            context.getString(R.string.background_removed),
            Toast.LENGTH_SHORT
        ).show()
    }

    fun handleCardAlphaChange(newValue: Float) {
        state.cardAlpha = newValue
        CardConfig.cardAlpha = newValue
        CardConfig.isCustomAlphaSet = true
        prefs.edit {
            putBoolean("is_custom_alpha_set", true)
            putFloat("card_alpha", newValue)
        }
    }

    fun handleCardDimChange(newValue: Float) {
        state.cardDim = newValue
        CardConfig.cardDim = newValue
        CardConfig.isCustomDimSet = true
        prefs.edit {
            putBoolean("is_custom_dim_set", true)
            putFloat("card_dim", newValue)
        }
    }

    fun handleSimpleModeChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_simple_mode", newValue) }
        state.isSimpleMode = newValue
    }

    fun handlePredictiveBackChange(enabled: Boolean) {
        prefs.edit { putBoolean(PREDICTIVE_BACK_PREFERENCE, enabled) }
        state.predictiveBackEnabled = enabled
    }

    fun handleKernelSimpleModeChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_kernel_simple_mode", newValue) }
        state.isKernelSimpleMode = newValue
    }

    fun handleHideVersionChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_version", newValue) }
        state.isHideVersion = newValue
    }

    fun handleHideOtherInfoChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_other_info", newValue) }
        state.isHideOtherInfo = newValue
    }

    fun handleHideZygiskImplementChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_zygisk_Implement", newValue) }
        state.isHideZygiskImplement = newValue
    }

    fun handleHideSeccompStatusChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_seccomp_status", newValue) }
        state.isHideSeccompStatus = newValue
    }

    fun handleHideMetaModuleImplementChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_meta_module_Implement", newValue) }
        state.isHideMetaModuleImplement = newValue
    }

    fun handleHideLinkCardChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_link_card", newValue) }
        state.isHideLinkCard = newValue
    }

    fun handleHideTagRowChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_hide_tag_row", newValue) }
        state.isHideTagRow = newValue
    }

    fun handleShowMoreModuleInfoChange(newValue: Boolean) {
        prefs.edit { putBoolean("show_more_module_info", newValue) }
        state.showMoreModuleInfo = newValue
    }

    fun handleModuleDescriptionMaxLinesChange(newValue: Int) {
        val lines = newValue.coerceIn(1, 5)
        prefs.edit { putInt("module_description_max_lines", lines) }
        state.moduleDescriptionMaxLines = lines
    }

    fun handleWebDebuggingChange(newValue: Boolean) {
        prefs.edit { putBoolean("enable_web_debugging", newValue) }
        state.enableWebDebugging = newValue
    }

    fun handleWebUIXErudaChange(newValue: Boolean) {
        prefs.edit { putBoolean("use_webuix_eruda", newValue) }
        state.useWebUIXEruda = newValue
    }

    fun handleAllowAnyDynamicManagerChange(newValue: Boolean) {
        DynamicManagerSettings.setAllowAnyApp(context, newValue)
        state.allowAnyDynamicManager = newValue
    }

    fun handleSelinuxChange(enabled: Boolean) {
        val ok = ShellUtils.fastCmdResult(getRootShell(), if (enabled) "setenforce 1" else "setenforce 0")
        if (ok) {
            state.selinuxEnabled = enabled
            val msg = if (enabled) R.string.selinux_enabled_toast else R.string.selinux_disabled_toast
            Toast.makeText(context, context.getString(msg), Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(context, context.getString(R.string.selinux_change_failed), Toast.LENGTH_SHORT).show()
        }
    }
}
