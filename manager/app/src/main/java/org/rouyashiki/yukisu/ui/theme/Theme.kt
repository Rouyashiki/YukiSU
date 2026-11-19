package org.rouyashiki.yukisu.ui.theme

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.core.*
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.paint
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.zIndex
import androidx.core.content.edit
import androidx.core.net.toUri
import coil.compose.AsyncImagePainter
import coil.compose.rememberAsyncImagePainter
import org.rouyashiki.yukisu.ui.component.clickHapticFeedback
import org.rouyashiki.yukisu.ui.motion.LocalYukiMotion
import org.rouyashiki.yukisu.ui.motion.rememberYukiMotion
import org.rouyashiki.yukisu.ui.motion.MotionPreset
import org.rouyashiki.yukisu.ui.motion.PageTransitionStyle
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

private const val USE_WALLPAPER_COLORS = "use_wallpaper_colors"
private const val WALLPAPER_SEED = "custom_background_seed"

internal data class CustomBackground(val uri: Uri, val seed: Int?)

@Stable
object ThemeConfig {
    internal var customBackground by mutableStateOf<CustomBackground?>(null)
    val customBackgroundUri: Uri? get() = customBackground?.uri
    var forceDarkMode by mutableStateOf<Boolean?>(null)
    var currentTheme by mutableStateOf<ThemeColors>(ThemeColors.Default)
    var useDynamicColor by mutableStateOf(false)
    var useWallpaperColors by mutableStateOf(false)
        internal set
    internal val wallpaperColorSeed: Int? get() = if (useWallpaperColors) customBackground?.seed else null
    var motionPreset by mutableStateOf(MotionPreset.QBouncy)
    var pageTransitionStyle by mutableStateOf(PageTransitionStyle.Reveal)

    // 背景状态
    var isTransPrideUnlocked by mutableStateOf(false)
    var backgroundImageLoaded by mutableStateOf(false)
    var isThemeChanging by mutableStateOf(false)
    var preventBackgroundRefresh by mutableStateOf(false)

    // 主题变化检测
    private var lastDarkModeState: Boolean? = null

    fun detectThemeChange(currentDarkMode: Boolean): Boolean {
        val hasChanged = lastDarkModeState != null && lastDarkModeState != currentDarkMode
        lastDarkModeState = currentDarkMode
        return hasChanged
    }

    fun resetBackgroundState() {
        if (!preventBackgroundRefresh) {
            backgroundImageLoaded = false
        }
        isThemeChanging = true
    }

    fun updateTheme(
        theme: ThemeColors? = null,
        dynamicColor: Boolean? = null,
        darkMode: Boolean? = null
    ) {
        theme?.let { currentTheme = it }
        dynamicColor?.let { useDynamicColor = it }
        darkMode?.let { forceDarkMode = it }
    }

    fun reset() {
        customBackground = null
        forceDarkMode = null
        currentTheme = ThemeColors.Default
        useDynamicColor = false
        useWallpaperColors = false
        motionPreset = MotionPreset.QBouncy
        pageTransitionStyle = PageTransitionStyle.Reveal
        backgroundImageLoaded = false
        isThemeChanging = false
        preventBackgroundRefresh = false
        lastDarkModeState = null
    }
}

object ThemeManager {
    private const val PREFS_NAME = "theme_prefs"

    fun loadColorSettings(context: Context) {
        loadThemeMode(context)
        loadThemeColors(context)
        loadDynamicColorState(context)
        if (!ThemeConfig.backgroundImageLoaded &&
            (!ThemeConfig.preventBackgroundRefresh || ThemeConfig.customBackground == null)
        ) {
            BackgroundManager.loadCustomBackground(context)
        }
    }

    fun saveThemeMode(context: Context, forceDark: Boolean?) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putString("theme_mode", when (forceDark) {
                true -> "dark"
                false -> "light"
                null -> "system"
            })
        }
        ThemeConfig.forceDarkMode = forceDark
    }

    fun loadThemeMode(context: Context) {
        val mode = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString("theme_mode", "system")

        ThemeConfig.forceDarkMode = when (mode) {
            "dark" -> true
            "light" -> false
            else -> null
        }
    }

    fun saveThemeColors(context: Context, themeName: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putString("theme_colors", themeName)
        }
        ThemeConfig.currentTheme = ThemeColors.fromName(themeName)
    }

    fun loadThemeColors(context: Context) {
        val themeName = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString("theme_colors", "default") ?: "default"
        ThemeConfig.currentTheme = ThemeColors.fromName(themeName)
    }

    fun saveDynamicColorState(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putBoolean("use_dynamic_color", enabled)
        }
        ThemeConfig.useDynamicColor = enabled
    }


    fun loadDynamicColorState(context: Context) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        ThemeConfig.useDynamicColor = prefs.getBoolean("use_dynamic_color", true)
        ThemeConfig.useWallpaperColors = prefs.getBoolean(USE_WALLPAPER_COLORS, false)
    }

    fun saveMotionPreset(context: Context, preset: MotionPreset) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putString("motion_preset", preset.persistedValue)
        }
        ThemeConfig.motionPreset = preset
    }

    fun loadMotionPreset(context: Context) {
        val savedValue = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString("motion_preset", null)
        ThemeConfig.motionPreset = MotionPreset.fromPersistedValue(savedValue)
    }

    fun savePageTransitionStyle(context: Context, style: PageTransitionStyle) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putString("page_transition_style", style.persistedValue)
        }
        ThemeConfig.pageTransitionStyle = style
    }

    fun loadPageTransitionStyle(context: Context) {
        val savedValue = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString("page_transition_style", null)
        ThemeConfig.pageTransitionStyle = PageTransitionStyle.fromPersistedValue(savedValue)
    }

    fun unlockTransPride(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putBoolean("trans_pride_unlocked", true)
        }
        ThemeConfig.isTransPrideUnlocked = true
    }

    fun loadTransPrideState(context: Context) {
        val unlocked = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean("trans_pride_unlocked", false)
        ThemeConfig.isTransPrideUnlocked = unlocked
    }
}

object BackgroundManager {
    private const val TAG = "BackgroundManager"
    private val changes = Mutex()

    suspend fun saveAndApplyCustomBackground(context: Context, uri: Uri): Boolean = changes.withLock {
        val useWallpaperColors = ThemeConfig.useWallpaperColors
        var pendingFile: File? = null
        try {
            val background = withContext(Dispatchers.IO) {
                val file = File.createTempFile("custom_background_", ".jpg", context.filesDir)
                pendingFile = file
                context.contentResolver.openInputStream(uri)?.use { input ->
                    file.outputStream().use { output -> input.copyTo(output) }
                } ?: throw IOException("No image stream")
                val savedUri = file.toURI().toASCIIString().toUri()
                CustomBackground(savedUri, if (useWallpaperColors) extractWallpaperSeed(context, savedUri) else null)
            }
            applyBackground(context, background)
            pendingFile = null
            true
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Log.e(TAG, "Could not save the custom background", e)
            false
        } finally {
            pendingFile?.delete()
        }
    }

    suspend fun clearCustomBackground(context: Context) = changes.withLock {
        applyBackground(context, null)
    }

    suspend fun setWallpaperColorsEnabled(context: Context, enabled: Boolean) = changes.withLock {
        val background = ThemeConfig.customBackground
        val updated = if (enabled && background != null && background.seed == null) {
            val seed = withContext(Dispatchers.IO) { extractWallpaperSeed(context, background.uri) }
            if (seed != null && ThemeConfig.customBackground == background) background.copy(seed = seed) else null
        } else null

        context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE).edit {
            putBoolean(USE_WALLPAPER_COLORS, enabled)
            updated?.seed?.let { putInt(WALLPAPER_SEED, it) }
        }
        if (updated != null) ThemeConfig.customBackground = updated
        ThemeConfig.useWallpaperColors = enabled
    }

    private fun applyBackground(context: Context, background: CustomBackground?) {
        context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE).edit {
            putString("custom_background", background?.uri?.toString())
            if (background?.seed == null) remove(WALLPAPER_SEED) else putInt(WALLPAPER_SEED, background.seed)
            putBoolean("prevent_background_refresh", false)
        }
        ThemeConfig.customBackground = background
        ThemeConfig.backgroundImageLoaded = false
        ThemeConfig.preventBackgroundRefresh = false
    }

    fun loadCustomBackground(context: Context) {
        val prefs = context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE)
        val uri = prefs.getString("custom_background", null)?.toUri()
        val background = uri?.takeIf {
            it.scheme != ContentResolver.SCHEME_FILE || it.path?.let { path -> File(path).isFile } == true
        }?.let {
            CustomBackground(it, if (prefs.contains(WALLPAPER_SEED)) prefs.getInt(WALLPAPER_SEED, 0) else null)
        }
        val preventRefresh = prefs.getBoolean("prevent_background_refresh", false)
        ThemeConfig.preventBackgroundRefresh = preventRefresh
        if (!preventRefresh || ThemeConfig.customBackground != background) {
            ThemeConfig.customBackground = background
            ThemeConfig.backgroundImageLoaded = false
        }
    }
}

@Composable
fun KernelSUTheme(
    darkTheme: Boolean = when(ThemeConfig.forceDarkMode) {
        true -> true
        false -> false
        null -> isSystemInDarkTheme()
    },
    dynamicColor: Boolean = ThemeConfig.useDynamicColor,
    showBackground: Boolean = true,
    content: @Composable () -> Unit
) {
    val context = LocalContext.current
    val systemIsDark = isSystemInDarkTheme()

    ThemeInitializer(context = context, systemIsDark = systemIsDark)

    val colorScheme = createColorScheme(context, darkTheme, dynamicColor)

    // 系统栏样式
    SystemBarController(darkTheme)

    val themedContent: @Composable () -> Unit = {
        Box(modifier = Modifier.clickHapticFeedback()) {
            if (showBackground) {
                Box(modifier = Modifier.fillMaxSize()) {
                    // 背景层
                    BackgroundLayer(darkTheme)
                    // 内容层
                    Box(modifier = Modifier.fillMaxSize().zIndex(1f)) {
                        content()
                    }
                }
            } else {
                content()
            }
        }
    }

    val motion = rememberYukiMotion(ThemeConfig.motionPreset)
    CompositionLocalProvider(
        LocalYukiMotion provides motion,
    ) {
        MaterialTheme(
            colorScheme = colorScheme,
            typography = ExpressiveTypography,
            shapes = ExpressiveShapes,
            motionScheme = motion,
            content = themedContent
        )
    }
}

@Composable
private fun ThemeInitializer(context: Context, systemIsDark: Boolean) {
    val themeChanged = ThemeConfig.detectThemeChange(systemIsDark)

    LaunchedEffect(systemIsDark, themeChanged) {
        if (ThemeConfig.forceDarkMode == null && themeChanged) {
            Log.d("ThemeSystem", "系统主题变化: $systemIsDark")
            ThemeConfig.resetBackgroundState()

            if (!ThemeConfig.preventBackgroundRefresh) {
                BackgroundManager.loadCustomBackground(context)
            }

            CardConfig.apply {
                load(context)
                setThemeDefaults(systemIsDark)
                save(context)
            }
        }
    }

    LaunchedEffect(context) {
        CardConfig.load(context)
        ThemeManager.loadColorSettings(context)
        ThemeManager.loadMotionPreset(context)
        ThemeManager.loadPageTransitionStyle(context)
        ThemeManager.loadTransPrideState(context)
    }
}

@Composable
internal fun BackgroundLayer(darkTheme: Boolean = ThemeConfig.forceDarkMode ?: isSystemInDarkTheme()) {
    // 默认背景
    Box(
        modifier = Modifier
            .fillMaxSize()
            .zIndex(-2f)
            .background(
                if (CardConfig.isCustomBackgroundEnabled) {
                    MaterialTheme.colorScheme.surfaceContainerLow
                } else {
                    MaterialTheme.colorScheme.background
                }
            )
    )

    ThemeConfig.customBackgroundUri?.let { uri ->
        CustomBackgroundLayer(uri = uri, darkTheme = darkTheme)
    }
}

@Composable
private fun CustomBackgroundLayer(uri: Uri, darkTheme: Boolean) {
    val painter = rememberAsyncImagePainter(
        model = uri,
        onError = { error ->
            Log.e("ThemeSystem", "背景加载失败: ${error.result.throwable.message}")
            if (ThemeConfig.customBackgroundUri == uri) {
                ThemeConfig.customBackground = null
                ThemeConfig.backgroundImageLoaded = false
            }
        },
        onSuccess = {
            if (ThemeConfig.customBackgroundUri == uri) {
                ThemeConfig.backgroundImageLoaded = true
                ThemeConfig.isThemeChanging = false
            }
        }
    )

    val transition = updateTransition(
        targetState = ThemeConfig.backgroundImageLoaded,
        label = "backgroundTransition"
    )

    val motion = LocalYukiMotion.current
    val alpha by transition.animateFloat(
        label = "backgroundAlpha",
        transitionSpec = { motion.effects() }
    ) { loaded -> if (loaded) 1f else 0f }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .zIndex(-1f)
            .alpha(alpha)
    ) {
        // 背景图片
        Box(
            modifier = Modifier
                .fillMaxSize()
                .paint(painter = painter, contentScale = ContentScale.Crop)
                .graphicsLayer {
                    this.alpha = (painter.state as? AsyncImagePainter.State.Success)?.let { 1f } ?: 0f
                }
        )

        // 遮罩层
        BackgroundOverlay(darkTheme = darkTheme)
    }
}

@Composable
private fun BackgroundOverlay(darkTheme: Boolean) {
    val dimFactor = CardConfig.cardDim

    // 主要遮罩层
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                if (darkTheme) {
                    Color.Black.copy(alpha = 0.3f + dimFactor * 0.4f)
                } else {
                    Color.White.copy(alpha = 0.05f + dimFactor * 0.3f)
                }
            )
    )

    // 边缘渐变遮罩
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.radialGradient(
                    colors = listOf(
                        Color.Transparent,
                        if (darkTheme) {
                            Color.Black.copy(alpha = 0.2f + dimFactor * 0.2f)
                        } else {
                            Color.Black.copy(alpha = 0.05f + dimFactor * 0.1f)
                        }
                    ),
                    radius = 1000f
                )
            )
    )
}

@Composable
private fun createColorScheme(
    context: Context,
    darkTheme: Boolean,
    dynamicColor: Boolean
): ColorScheme {
    if (!dynamicColor) {
        return if (darkTheme) createDarkColorScheme() else createLightColorScheme()
    }
    val seed = ThemeConfig.wallpaperColorSeed
    val scheme = when {
        seed != null -> rememberWallpaperColorScheme(context, seed, darkTheme)
        darkTheme -> dynamicDarkColorScheme(context)
        else -> dynamicLightColorScheme(context)
    }
    return if (CardConfig.isCustomBackgroundEnabled) {
        scheme.copy(background = Color.Transparent, surface = Color.Transparent)
    } else scheme
}

@Composable
private fun SystemBarController(darkMode: Boolean) {
    val context = LocalContext.current
    val activity = context as ComponentActivity

    SideEffect {
        activity.enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.auto(
                Color.Transparent.toArgb(),
                Color.Transparent.toArgb(),
            ) { darkMode },
            navigationBarStyle = if (darkMode) {
                SystemBarStyle.dark(Color.Transparent.toArgb())
            } else {
                SystemBarStyle.light(
                    Color.Transparent.toArgb(),
                    Color.Transparent.toArgb()
                )
            }
        )
    }
}

@Composable
private fun createDarkColorScheme() = darkColorScheme(
    primary = ThemeConfig.currentTheme.primaryDark,
    onPrimary = ThemeConfig.currentTheme.onPrimaryDark,
    primaryContainer = ThemeConfig.currentTheme.primaryContainerDark,
    onPrimaryContainer = ThemeConfig.currentTheme.onPrimaryContainerDark,
    secondary = ThemeConfig.currentTheme.secondaryDark,
    onSecondary = ThemeConfig.currentTheme.onSecondaryDark,
    secondaryContainer = ThemeConfig.currentTheme.secondaryContainerDark,
    onSecondaryContainer = ThemeConfig.currentTheme.onSecondaryContainerDark,
    tertiary = ThemeConfig.currentTheme.tertiaryDark,
    onTertiary = ThemeConfig.currentTheme.onTertiaryDark,
    tertiaryContainer = ThemeConfig.currentTheme.tertiaryContainerDark,
    onTertiaryContainer = ThemeConfig.currentTheme.onTertiaryContainerDark,
    error = ThemeConfig.currentTheme.errorDark,
    onError = ThemeConfig.currentTheme.onErrorDark,
    errorContainer = ThemeConfig.currentTheme.errorContainerDark,
    onErrorContainer = ThemeConfig.currentTheme.onErrorContainerDark,
    background = if (CardConfig.isCustomBackgroundEnabled) Color.Transparent else ThemeConfig.currentTheme.backgroundDark,
    onBackground = ThemeConfig.currentTheme.onBackgroundDark,
    surface = if (CardConfig.isCustomBackgroundEnabled) Color.Transparent else ThemeConfig.currentTheme.surfaceDark,
    onSurface = ThemeConfig.currentTheme.onSurfaceDark,
    surfaceVariant = ThemeConfig.currentTheme.surfaceVariantDark,
    onSurfaceVariant = ThemeConfig.currentTheme.onSurfaceVariantDark,
    outline = ThemeConfig.currentTheme.outlineDark,
    outlineVariant = ThemeConfig.currentTheme.outlineVariantDark,
    scrim = ThemeConfig.currentTheme.scrimDark,
    inverseSurface = ThemeConfig.currentTheme.inverseSurfaceDark,
    inverseOnSurface = ThemeConfig.currentTheme.inverseOnSurfaceDark,
    inversePrimary = ThemeConfig.currentTheme.inversePrimaryDark,
    surfaceDim = ThemeConfig.currentTheme.surfaceDimDark,
    surfaceBright = ThemeConfig.currentTheme.surfaceBrightDark,
    surfaceContainerLowest = ThemeConfig.currentTheme.surfaceContainerLowestDark,
    surfaceContainerLow = ThemeConfig.currentTheme.surfaceContainerLowDark,
    surfaceContainer = ThemeConfig.currentTheme.surfaceContainerDark,
    surfaceContainerHigh = ThemeConfig.currentTheme.surfaceContainerHighDark,
    surfaceContainerHighest = ThemeConfig.currentTheme.surfaceContainerHighestDark,
)

@Composable
private fun createLightColorScheme() = lightColorScheme(
    primary = ThemeConfig.currentTheme.primaryLight,
    onPrimary = ThemeConfig.currentTheme.onPrimaryLight,
    primaryContainer = ThemeConfig.currentTheme.primaryContainerLight,
    onPrimaryContainer = ThemeConfig.currentTheme.onPrimaryContainerLight,
    secondary = ThemeConfig.currentTheme.secondaryLight,
    onSecondary = ThemeConfig.currentTheme.onSecondaryLight,
    secondaryContainer = ThemeConfig.currentTheme.secondaryContainerLight,
    onSecondaryContainer = ThemeConfig.currentTheme.onSecondaryContainerLight,
    tertiary = ThemeConfig.currentTheme.tertiaryLight,
    onTertiary = ThemeConfig.currentTheme.onTertiaryLight,
    tertiaryContainer = ThemeConfig.currentTheme.tertiaryContainerLight,
    onTertiaryContainer = ThemeConfig.currentTheme.onTertiaryContainerLight,
    error = ThemeConfig.currentTheme.errorLight,
    onError = ThemeConfig.currentTheme.onErrorLight,
    errorContainer = ThemeConfig.currentTheme.errorContainerLight,
    onErrorContainer = ThemeConfig.currentTheme.onErrorContainerLight,
    background = if (CardConfig.isCustomBackgroundEnabled) Color.Transparent else ThemeConfig.currentTheme.backgroundLight,
    onBackground = ThemeConfig.currentTheme.onBackgroundLight,
    surface = if (CardConfig.isCustomBackgroundEnabled) Color.Transparent else ThemeConfig.currentTheme.surfaceLight,
    onSurface = ThemeConfig.currentTheme.onSurfaceLight,
    surfaceVariant = ThemeConfig.currentTheme.surfaceVariantLight,
    onSurfaceVariant = ThemeConfig.currentTheme.onSurfaceVariantLight,
    outline = ThemeConfig.currentTheme.outlineLight,
    outlineVariant = ThemeConfig.currentTheme.outlineVariantLight,
    scrim = ThemeConfig.currentTheme.scrimLight,
    inverseSurface = ThemeConfig.currentTheme.inverseSurfaceLight,
    inverseOnSurface = ThemeConfig.currentTheme.inverseOnSurfaceLight,
    inversePrimary = ThemeConfig.currentTheme.inversePrimaryLight,
    surfaceDim = ThemeConfig.currentTheme.surfaceDimLight,
    surfaceBright = ThemeConfig.currentTheme.surfaceBrightLight,
    surfaceContainerLowest = ThemeConfig.currentTheme.surfaceContainerLowestLight,
    surfaceContainerLow = ThemeConfig.currentTheme.surfaceContainerLowLight,
    surfaceContainer = ThemeConfig.currentTheme.surfaceContainerLight,
    surfaceContainerHigh = ThemeConfig.currentTheme.surfaceContainerHighLight,
    surfaceContainerHighest = ThemeConfig.currentTheme.surfaceContainerHighestLight,
)

fun Context.saveThemeMode(forceDark: Boolean?) {
    ThemeManager.saveThemeMode(this, forceDark)
}


fun Context.saveThemeColors(themeName: String) {
    ThemeManager.saveThemeColors(this, themeName)
}


fun Context.saveDynamicColorState(enabled: Boolean) {
    ThemeManager.saveDynamicColorState(this, enabled)
}

fun Context.saveMotionPreset(preset: MotionPreset) {
    ThemeManager.saveMotionPreset(this, preset)
}

fun Context.savePageTransitionStyle(style: PageTransitionStyle) {
    ThemeManager.savePageTransitionStyle(this, style)
}
