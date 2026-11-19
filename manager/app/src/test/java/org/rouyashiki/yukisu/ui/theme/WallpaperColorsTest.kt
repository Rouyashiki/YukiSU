package org.rouyashiki.yukisu.ui.theme

import android.app.Application
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.core.content.edit
import kotlinx.coroutines.runBlocking
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.screen.moreSettings.AppearanceSettings
import org.rouyashiki.yukisu.ui.screen.moreSettings.MoreSettingsHandlers
import org.rouyashiki.yukisu.ui.screen.moreSettings.state.MoreSettingsState
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLog
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [31, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class WallpaperColorsTest {
    @get:Rule val compose = createComposeRule()
    private val context: Context get() = RuntimeEnvironment.getApplication()
    private val prefs get() = context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE)

    @Before
    fun prepare() {
        reset()
        prefs.edit { clear() }
    }

    @After
    fun reset() {
        ThemeConfig.reset()
        CardConfig.reset()
    }

    @Test
    fun wallpaperColoursSwitchExpandsAndCollapsesWithTheCustomBackground() {
        ThemeManager.saveDynamicColorState(context, true)
        val settings = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val state = MoreSettingsState(context, settings, systemIsDark = false)
        val uri = image(Color.Red)
        compose.setContent {
            val scope = rememberCoroutineScope()
            val handlers = remember { MoreSettingsHandlers(context, settings, state, scope) }
            val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) {}
            Column(Modifier.verticalScroll(rememberScrollState())) {
                AppearanceSettings(state, handlers, picker, scope)
            }
        }

        val option = compose.onNodeWithText(context.getString(R.string.wallpaper_colors_title))
        val dynamic = compose.onNodeWithText(context.getString(R.string.dynamic_color_title))
        option.assertDoesNotExist()
        compose.runOnIdle {
            runBlocking { assertTrue(BackgroundManager.saveAndApplyCustomBackground(context, uri)) }
        }
        option.performScrollTo().assertIsDisplayed().assertIsEnabled()
        dynamic.performScrollTo().performClick()
        option.assertIsNotEnabled()
        dynamic.performScrollTo().performClick()
        option.assertIsEnabled()
        compose.onNodeWithText(context.getString(R.string.settings_custom_background)).performScrollTo().performClick()
        option.assertDoesNotExist()
    }

    @Test
    fun existingWallpaperIsSampledOnOptInAndRestoredOnFirstComposition() {
        val red = image(Color.Red)
        prefs.edit {
            putString("custom_background", red.toString())
            putBoolean("prevent_background_refresh", true)
        }
        ThemeManager.loadColorSettings(context)
        assertFalse(ThemeConfig.useWallpaperColors)
        assertNull(ThemeConfig.wallpaperColorSeed)
        runBlocking { BackgroundManager.setWallpaperColorsEnabled(context, true) }
        val seed = requireSeed()
        assertTrue(Color(seed).red > 0.95f && Color(seed).blue < 0.05f)

        reset()
        ThemeManager.loadColorSettings(context)
        assertEquals(red, ThemeConfig.customBackgroundUri)
        assertEquals(seed, ThemeConfig.wallpaperColorSeed)
        val dark = mutableStateOf(false)
        val observed = mutableListOf<ColorScheme>()
        compose.setContent {
            KernelSUTheme(darkTheme = dark.value, showBackground = false) {
                val scheme = MaterialTheme.colorScheme
                SideEffect { observed += scheme }
            }
        }
        compose.runOnIdle {
            assertEquals(wallpaperColorScheme(seed, false, 0.0).primary, observed.first().primary)
            assertEquals(Color.Transparent, observed.first().surface)
            dark.value = true
        }
        compose.runOnIdle {
            assertEquals(wallpaperColorScheme(seed, true, 0.0).primary, observed.last().primary)
            assertEquals(Color.Transparent, observed.last().surface)
            assertNotEquals(observed.first().primary, observed.last().primary)
            ThemeManager.saveDynamicColorState(context, false)
        }
        compose.runOnIdle {
            assertEquals(ThemeConfig.currentTheme.primaryDark, observed.last().primary)
            ThemeManager.saveDynamicColorState(context, true)
            runBlocking { BackgroundManager.setWallpaperColorsEnabled(context, false) }
            dark.value = false
        }
        compose.runOnIdle {
            assertEquals(dynamicLightColorScheme(context).primary, observed.last().primary)
        }
    }

    @Test
    fun replacingAndRemovingTheWallpaperUpdatesThePersistedSeed() = runBlocking {
        BackgroundManager.setWallpaperColorsEnabled(context, true)
        assertNull(ThemeConfig.wallpaperColorSeed)
        assertTrue(BackgroundManager.saveAndApplyCustomBackground(context, image(Color.Red)))
        val red = requireSeed()
        assertTrue(BackgroundManager.saveAndApplyCustomBackground(context, image(Color.Blue)))
        val blue = requireSeed()
        assertTrue(Color(blue).blue > 0.95f && Color(blue).red < 0.05f)
        assertNotEquals(red, blue)

        val missing = Uri.fromFile(File(context.cacheDir, "missing"))
        assertFalse(BackgroundManager.saveAndApplyCustomBackground(context, missing))
        reset()
        ThemeManager.loadColorSettings(context)
        assertEquals(blue, requireSeed())
        BackgroundManager.clearCustomBackground(context)
        reset()
        ThemeManager.loadColorSettings(context)
        assertTrue(ThemeConfig.useWallpaperColors)
        assertNull(ThemeConfig.customBackgroundUri)
        assertNull(ThemeConfig.wallpaperColorSeed)
    }

    @Test
    fun missingOrUndecodableWallpaperCannotReuseTheLastSeed() = runBlocking {
        BackgroundManager.setWallpaperColorsEnabled(context, true)
        assertTrue(BackgroundManager.saveAndApplyCustomBackground(context, image(Color.Blue)))
        requireSeed()
        val installed = File(assertNotNull(ThemeConfig.customBackgroundUri?.path))
        assertTrue(installed.delete())
        reset()
        ThemeManager.loadColorSettings(context)
        assertNull(ThemeConfig.wallpaperColorSeed)
        assertFalse(CardConfig.isCustomBackgroundEnabled)

        val invalid = File(context.cacheDir, "invalid.png").apply { writeText("invalid") }
        assertTrue(BackgroundManager.saveAndApplyCustomBackground(context, Uri.fromFile(invalid)))
        assertNull(ThemeConfig.wallpaperColorSeed)
        reset()
        ThemeManager.loadColorSettings(context)
        assertNull(ThemeConfig.wallpaperColorSeed)
    }

    private fun requireSeed(): Int = assertNotNull(
        ThemeConfig.wallpaperColorSeed,
        ShadowLog.getLogsForTag("WallpaperColors").lastOrNull()?.throwable?.stackTraceToString()
            ?: "No seed for ${prefs.getString("custom_background", null)} (loaded ${ThemeConfig.customBackgroundUri})"
    )

    private fun image(color: Color): Uri {
        val file = File.createTempFile("wallpaper", ".png", context.cacheDir)
        val bitmap = Bitmap.createBitmap(480, 320, Bitmap.Config.ARGB_8888)
        try {
            bitmap.eraseColor(color.toArgb())
            file.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
        } finally {
            bitmap.recycle()
        }
        return Uri.parse(file.toURI().toASCIIString()).buildUpon().authority("").build()
    }
}
