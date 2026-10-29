package com.anatdx.yukisu.ui.theme

import android.content.Context
import android.content.res.Configuration
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

@Stable
object CardConfig {
    var cardAlpha by mutableFloatStateOf(1f)
        internal set
    var cardDim by mutableFloatStateOf(0f)
        internal set

    private var defaultCardElevation by mutableStateOf(0.dp)

    val cardElevation: Dp
        get() = if (isShadowEnabled) defaultCardElevation else 0.dp

    val isShadowEnabled: Boolean
        get() = !isCustomBackgroundEnabled

    var isCustomBackgroundEnabled by mutableStateOf(false)
        internal set

    var isCustomAlphaSet by mutableStateOf(false)
        internal set
    var isCustomDimSet by mutableStateOf(false)
        internal set
    var isUserDarkModeEnabled by mutableStateOf(false)
        internal set
    var isUserLightModeEnabled by mutableStateOf(false)
        internal set

    private object Keys {
        const val CARD_ALPHA = "card_alpha"
        const val CARD_DIM = "card_dim"
        const val CUSTOM_BACKGROUND_ENABLED = "custom_background_enabled"
        const val IS_SHADOW_ENABLED = "is_shadow_enabled"
        const val IS_CUSTOM_ALPHA_SET = "is_custom_alpha_set"
        const val IS_CUSTOM_DIM_SET = "is_custom_dim_set"
        const val IS_USER_DARK_MODE_ENABLED = "is_user_dark_mode_enabled"
        const val IS_USER_LIGHT_MODE_ENABLED = "is_user_light_mode_enabled"
    }

    fun updateAlpha(alpha: Float, isCustom: Boolean = true) {
        cardAlpha = alpha.coerceIn(0f, 1f)
        if (isCustom) isCustomAlphaSet = true
    }

    fun updateDim(dim: Float, isCustom: Boolean = true) {
        cardDim = dim.coerceIn(0f, 1f)
        if (isCustom) isCustomDimSet = true
    }

    fun updateBackground(enabled: Boolean) {
        isCustomBackgroundEnabled = enabled
    }

    fun updateThemePreference(darkMode: Boolean?, lightMode: Boolean?) {
        isUserDarkModeEnabled = darkMode ?: false
        isUserLightModeEnabled = lightMode ?: false
    }

    fun reset() {
        cardAlpha = 1f
        cardDim = 0f
        defaultCardElevation = 0.dp
        isCustomBackgroundEnabled = false
        isCustomAlphaSet = false
        isCustomDimSet = false
        isUserDarkModeEnabled = false
        isUserLightModeEnabled = false
    }

    fun setThemeDefaults(isDarkMode: Boolean) {
        if (!isCustomAlphaSet) {
            updateAlpha(if (isDarkMode) 0.88f else 1f, false)
        }
        if (!isCustomDimSet) {
            updateDim(if (isDarkMode) 0.25f else 0f, false)
        }
        defaultCardElevation = if (isDarkMode) 2.dp else 0.dp
    }

    fun save(context: Context) {
        val prefs = context.getSharedPreferences("card_settings", Context.MODE_PRIVATE)
        prefs.edit().apply {
            putFloat(Keys.CARD_ALPHA, cardAlpha)
            putFloat(Keys.CARD_DIM, cardDim)
            putBoolean(Keys.CUSTOM_BACKGROUND_ENABLED, isCustomBackgroundEnabled)
            remove(Keys.IS_SHADOW_ENABLED)
            putBoolean(Keys.IS_CUSTOM_ALPHA_SET, isCustomAlphaSet)
            putBoolean(Keys.IS_CUSTOM_DIM_SET, isCustomDimSet)
            putBoolean(Keys.IS_USER_DARK_MODE_ENABLED, isUserDarkModeEnabled)
            putBoolean(Keys.IS_USER_LIGHT_MODE_ENABLED, isUserLightModeEnabled)
            apply()
        }
    }

    fun load(context: Context) {
        val prefs = context.getSharedPreferences("card_settings", Context.MODE_PRIVATE)
        cardAlpha = prefs.getFloat(Keys.CARD_ALPHA, 1f).coerceIn(0f, 1f)
        cardDim = prefs.getFloat(Keys.CARD_DIM, 0f).coerceIn(0f, 1f)
        isCustomBackgroundEnabled = prefs.getBoolean(Keys.CUSTOM_BACKGROUND_ENABLED, false)
        isCustomAlphaSet = prefs.getBoolean(Keys.IS_CUSTOM_ALPHA_SET, false)
        isCustomDimSet = prefs.getBoolean(Keys.IS_CUSTOM_DIM_SET, false)
        isUserDarkModeEnabled = prefs.getBoolean(Keys.IS_USER_DARK_MODE_ENABLED, false)
        isUserLightModeEnabled = prefs.getBoolean(Keys.IS_USER_LIGHT_MODE_ENABLED, false)

        // Restore elevation without changing saved opacity or dimming.
        val isDarkMode = when {
            isUserLightModeEnabled -> false
            isUserDarkModeEnabled -> true
            else -> (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
                Configuration.UI_MODE_NIGHT_YES
        }
        defaultCardElevation = if (isDarkMode) 2.dp else 0.dp
    }

}

object CardStyleProvider {

    fun getBarColor(originalColor: Color) = if (CardConfig.isCustomBackgroundEnabled) {
        originalColor.copy(alpha = originalColor.alpha * CardConfig.cardAlpha)
    } else {
        originalColor
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    fun getTopAppBarColors(
        containerColor: Color = Color.Unspecified,
        scrolledContainerColor: Color = containerColor,
    ) = TopAppBarDefaults.topAppBarColors(
        containerColor = containerColor,
        scrolledContainerColor = scrolledContainerColor,
    ).let { colors ->
        colors.copy(
            containerColor = getBarColor(colors.containerColor),
            scrolledContainerColor = getBarColor(colors.scrolledContainerColor),
        )
    }

    @Composable
    fun getElevatedCardColors() = CardDefaults.elevatedCardColors().let { colors ->
        colors.copy(
            containerColor = colors.containerColor.copy(alpha = CardConfig.cardAlpha),
            disabledContainerColor = colors.disabledContainerColor.copy(alpha = CardConfig.cardAlpha),
        )
    }

    @Composable
    fun getElevatedCardElevation() = if (CardConfig.isShadowEnabled && CardConfig.cardAlpha == 1f) {
        CardDefaults.elevatedCardElevation()
    } else {
        // Hardware shadows show through translucent surfaces as an extra frame.
        CardDefaults.cardElevation(
            defaultElevation = 0.dp,
            pressedElevation = 0.dp,
            focusedElevation = 0.dp,
            hoveredElevation = 0.dp,
            draggedElevation = 0.dp,
            disabledElevation = 0.dp,
        )
    }

    @Composable
    fun getCardColors(originalColor: Color) = CardDefaults.cardColors(
        containerColor = originalColor.copy(alpha = CardConfig.cardAlpha),
        contentColor = determineContentColor(originalColor),
        disabledContainerColor = originalColor.copy(alpha = CardConfig.cardAlpha * 0.38f),
        disabledContentColor = determineContentColor(originalColor).copy(alpha = 0.38f)
    )

    @Composable
    fun getCardElevation() = CardDefaults.cardElevation(
        defaultElevation = CardConfig.cardElevation,
        pressedElevation = CardConfig.cardElevation,
        focusedElevation = CardConfig.cardElevation,
        hoveredElevation = CardConfig.cardElevation,
        draggedElevation = CardConfig.cardElevation,
        disabledElevation = 0.dp
    )

    @Composable
    private fun determineContentColor(originalColor: Color): Color {
        val isDarkTheme = isSystemInDarkTheme()

        return when {
            ThemeConfig.isThemeChanging -> {
                if (isDarkTheme) Color.White else Color.Black
            }
            CardConfig.isUserLightModeEnabled -> Color.Black
            CardConfig.isUserDarkModeEnabled -> Color.White
            else -> {
                val luminance = originalColor.luminance()
                val threshold = if (isDarkTheme) 0.4f else 0.6f
                if (luminance > threshold) Color.Black else Color.White
            }
        }
    }
}

// 向后兼容
@Composable
fun getCardColors(originalColor: Color) = CardStyleProvider.getCardColors(originalColor)

@Composable
fun getCardElevation() = CardStyleProvider.getCardElevation()
