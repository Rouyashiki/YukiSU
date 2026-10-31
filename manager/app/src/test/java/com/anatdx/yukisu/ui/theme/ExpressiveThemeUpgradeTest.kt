package com.anatdx.yukisu.ui.theme

import android.app.Application
import android.content.Context
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Update
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.core.content.edit
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.screen.SwitchItem
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class ExpressiveThemeUpgradeTest {
    @get:Rule
    val compose = createComposeRule()

    private val context: Context get() = RuntimeEnvironment.getApplication()

    @Before
    fun prepare() {
        ThemeConfig.reset()
        context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE).edit { clear() }
        context.getSharedPreferences("settings", Context.MODE_PRIVATE).edit { clear() }
    }

    @After
    fun reset() {
        ThemeConfig.reset()
    }

    @Test
    fun classicInstallationUsesExpressiveThemeAndRetainsAppearancePreferences() {
        checkInstallation(savedStyle = "classic", firstRun = false, dark = true)
    }

    @Test
    fun installationPredatingStyleSelectionUsesExpressiveTheme() {
        checkInstallation(savedStyle = null, firstRun = false, dark = false)
    }

    @Test
    fun freshInstallationUsesExpressiveTheme() {
        checkInstallation(savedStyle = null, firstRun = true, dark = false)
    }

    @Test
    fun expressiveInstallationRetainsItsTheme() {
        checkInstallation(savedStyle = "expressive", firstRun = false, dark = true)
    }

    private fun checkInstallation(savedStyle: String?, firstRun: Boolean, dark: Boolean) {
        context.getSharedPreferences("settings", Context.MODE_PRIVATE).edit {
            putBoolean("is_first_run", firstRun)
        }
        context.getSharedPreferences("theme_prefs", Context.MODE_PRIVATE).edit {
            putString("ui_style", savedStyle)
            putString("theme_mode", if (dark) "dark" else "light")
            putBoolean("use_dynamic_color", false)
            putString("motion_preset", MotionPreset.Off.persistedValue)
            putString("page_transition_style", PageTransitionStyle.Slide.persistedValue)
        }
        ThemeManager.loadThemeMode(context)
        ThemeManager.loadDynamicColorState(context)
        ThemeManager.loadMotionPreset(context)
        ThemeManager.loadPageTransitionStyle(context)

        val checked = mutableStateOf(false)
        var changes = 0
        var themeObserved = false
        compose.setContent {
            KernelSUTheme(showBackground = false) {
                val typography = MaterialTheme.typography
                val shapes = MaterialTheme.shapes
                val colorScheme = MaterialTheme.colorScheme
                val motion = LocalYukiMotion.current
                SideEffect {
                    assertEquals(ExpressiveTypography, typography)
                    assertEquals(ExpressiveShapes, shapes)
                    assertEquals(
                        if (dark) ThemeConfig.currentTheme.primaryDark else ThemeConfig.currentTheme.primaryLight,
                        colorScheme.primary,
                    )
                    assertFalse(motion.enabled)
                    themeObserved = true
                }
                SwitchItem(
                    icon = Icons.Filled.Update,
                    title = "Check updates",
                    checked = checked.value,
                    onCheckedChange = {
                        checked.value = it
                        changes++
                    },
                )
            }
        }
        compose.onNodeWithText("Check updates").assertIsOff().performClick().assertIsOn()
        compose.onNodeWithText("Check updates").performClick().assertIsOff()
        compose.runOnIdle {
            assertTrue(themeObserved)
            assertEquals(2, changes)
            assertEquals(dark, ThemeConfig.forceDarkMode)
            assertFalse(ThemeConfig.useDynamicColor)
            assertEquals(PageTransitionStyle.Slide, ThemeConfig.pageTransitionStyle)
        }
    }
}
