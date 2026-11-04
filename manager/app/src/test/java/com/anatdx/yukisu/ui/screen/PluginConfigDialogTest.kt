package com.anatdx.yukisu.ui.screen

import android.app.Application
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.viewmodel.PluginConfigChanges
import com.anatdx.yukisu.ui.viewmodel.PluginViewModel
import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class PluginConfigDialogTest {
    @get:Rule val compose = createComposeRule()

    private val initial = mapOf("mode" to "manual")
    private var saved: PluginConfigChanges? = null
    private var dismissed = false
    private val revision = mutableIntStateOf(0)

    private fun showDialog() {
        val plugin = PluginViewModel().parsePlugins(
            """[{"id":"example","config":[{"key":"mode","label":"Mode",
                "type":"text","default":"auto"}]}]"""
        ).single()
        compose.setContent {
            MaterialTheme {
                PluginConfigDialog(
                    plugin = plugin, initial = initial, revision = revision.intValue,
                    locale = Locale.ENGLISH, isSaving = false,
                    onDismiss = { dismissed = true }, onSave = { saved = it },
                )
            }
        }
    }

    private fun text(id: Int) = RuntimeEnvironment.getApplication().getString(id)

    @Test
    fun resettingAndCancellingDoesNotSave() {
        showDialog()
        compose.onNodeWithText(text(R.string.plugin_config_restore_default)).performClick()
        compose.onNodeWithText("auto").assertExists()
        compose.onNodeWithText(text(R.string.cancel)).performClick()
        compose.runOnIdle {
            assertTrue(dismissed)
            assertNull(saved)
            assertEquals(mapOf("mode" to "manual"), initial)
        }
    }

    @Test
    fun resettingAndSavingDeletesTheSavedKey() {
        showDialog()
        compose.onNodeWithText(text(R.string.plugin_config_restore_default)).performClick()
        compose.onNodeWithText(text(R.string.plugin_save)).performClick()
        compose.runOnIdle { assertEquals(PluginConfigChanges(emptyMap(), setOf("mode")), saved) }
    }

    @Test
    fun editingAfterResetWritesTheNewValue() {
        showDialog()
        compose.onNodeWithText(text(R.string.plugin_config_restore_default)).performClick()
        compose.onNodeWithText("auto").performTextReplacement("changed")
        compose.onNodeWithText(text(R.string.plugin_save)).performClick()
        compose.runOnIdle {
            assertEquals(PluginConfigChanges(mapOf("mode" to "changed"), emptySet()), saved)
        }
    }

    @Test
    fun reloadingAfterFailedSaveClearsTheDraftEvenWhenValuesMatch() {
        showDialog()
        compose.onNodeWithText(text(R.string.plugin_config_restore_default)).performClick()
        compose.runOnIdle { revision.intValue++ }
        compose.onNodeWithText("manual").assertExists()
        compose.onNodeWithText(text(R.string.plugin_save)).performClick()
        compose.runOnIdle { assertEquals(PluginConfigChanges(emptyMap(), emptySet()), saved) }
    }
}
