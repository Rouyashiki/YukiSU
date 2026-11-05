package com.anatdx.yukisu.ui.screen

import android.app.Application
import android.content.Context
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.platform.LocalAccessibilityManager
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.core.content.edit
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.YukiMotion
import com.anatdx.yukisu.ui.util.SoftRebootStatus
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class SoftRebootStatusCardTest {
    @get:Rule
    val compose = createComposeRule()

    private val context get() = RuntimeEnvironment.getApplication()
    private val preferences get() = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
    private val failure = SoftRebootStatus(
        known = true,
        result = "failed",
        error = "coordinator exited during rebuild",
        rebootId = "test-boot:123",
    )

    @Before
    fun clearPreferences() {
        preferences.edit { clear() }
    }

    @Test
    fun failureAutomaticallyDismissesAfterTenSeconds() {
        setContent(mutableStateOf(failure))
        advanceTime(5_000L)
        compose.onNodeWithText(failureMessage()).assertExists()
        advanceTime(5_100L)
        compose.onNodeWithText(failureMessage()).assertDoesNotExist()
        assertEquals(failure.rebootId, preferences.getString("soft_reboot_failed_notice", null))
    }

    @Test
    fun manualDismissalSurvivesRecreationAndAllowsTheNextFailure() {
        val status = mutableStateOf(failure)
        val shown = mutableStateOf(true)
        setContent(status, shown)
        compose.onNodeWithContentDescription(context.getString(R.string.close)).performClick()
        advanceTime(100L)
        compose.onNodeWithText(failureMessage()).assertDoesNotExist()

        compose.runOnIdle { shown.value = false }
        advanceTime(100L)
        compose.runOnIdle { shown.value = true }
        advanceTime(100L)
        compose.onNodeWithText(failureMessage()).assertDoesNotExist()

        compose.runOnIdle { status.value = failure.copy(rebootId = "test-boot:456") }
        advanceTime(100L)
        assertEquals(failure.rebootId, preferences.getString("soft_reboot_failed_notice", null))
        compose.mainClock.advanceTimeByFrame()
        compose.runOnIdle {}
        compose.onNodeWithText(failureMessage()).assertExists()
    }

    @Test
    fun runningRebootStaysVisibleAfterAnEarlierDismissal() {
        preferences.edit { putString("soft_reboot_failed_notice", failure.rebootId) }
        setContent(mutableStateOf(failure.copy(running = true)))
        compose.onNodeWithText(context.getString(R.string.soft_reboot_accepted)).assertExists()
        compose.onNodeWithContentDescription(context.getString(R.string.close)).assertDoesNotExist()
    }

    @Test
    fun existingCompletedDismissalIsRespected() {
        preferences.edit { putString("soft_reboot_completed_notice", failure.rebootId) }
        setContent(mutableStateOf(failure.copy(result = "completed")))
        compose.onNodeWithText(context.getString(R.string.soft_reboot_completed)).assertDoesNotExist()
    }

    @Test
    fun completedResultAutomaticallyDismissesAfterFiveSeconds() {
        setContent(mutableStateOf(failure.copy(result = "completed")))
        compose.onNodeWithText(context.getString(R.string.soft_reboot_completed)).assertExists()
        advanceTime(5_100L)
        compose.onNodeWithText(context.getString(R.string.soft_reboot_completed)).assertDoesNotExist()
        assertEquals(failure.rebootId, preferences.getString("soft_reboot_completed_notice", null))
    }

    private fun setContent(status: State<SoftRebootStatus>, shown: State<Boolean> = mutableStateOf(true)) {
        compose.mainClock.autoAdvance = false
        compose.setContent {
            val motion = YukiMotion(MotionPreset.Off)
            CompositionLocalProvider(
                LocalYukiMotion provides motion,
                LocalAccessibilityManager provides null,
            ) {
                MaterialTheme(motionScheme = motion) {
                    if (shown.value) SoftRebootStatusCard(status.value)
                }
            }
        }
        advanceTime(100L)
    }

    private fun advanceTime(millis: Long) {
        compose.mainClock.advanceTimeBy(millis)
        compose.runOnIdle {}
    }

    private fun failureMessage(): String = context.getString(R.string.soft_reboot_failed, failure.error)
}
