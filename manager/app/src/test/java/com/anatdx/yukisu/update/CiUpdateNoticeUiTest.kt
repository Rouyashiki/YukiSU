package com.anatdx.yukisu.update

import android.app.Application
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.YukiMotion
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class CiUpdateNoticeUiTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun availableResultExpandsAndRoutesTheUpdateAction() {
        val state = mutableStateOf<CiUpdateCheckState>(CiUpdateCheckState.Hidden)
        var updates = 0
        var retries = 0
        setNoticeContent(state, onRetry = { retries++ }, onUpdate = { updates++ })

        compose.onNodeWithTag(NOTICE).assertDoesNotExist()
        listOf(CiUpdateCheckState.Checking, CiUpdateCheckState.Current).forEach {
            compose.runOnIdle { state.value = it }
            frame()
            compose.onNodeWithTag(NOTICE).assertDoesNotExist()
        }

        compose.runOnIdle { state.value = CiUpdateCheckState.Available(run) }
        assertExpandsFromHidden()
        compose.onNodeWithText(availableMessage()).performClick()
        compose.runOnIdle {
            assertEquals(1, updates)
            assertEquals(0, retries)
        }

        val expandedHeight = height()
        compose.runOnIdle { state.value = CiUpdateCheckState.Current }
        assertCollapses(expandedHeight)
        compose.onNodeWithText(availableMessage()).assertDoesNotExist()
    }

    @Test
    fun failedResultExpandsAndRetryCollapsesBeforeTheNextResult() {
        val state = mutableStateOf<CiUpdateCheckState>(CiUpdateCheckState.Checking)
        var retries = 0
        var updates = 0
        setNoticeContent(
            state,
            onRetry = {
                retries++
                state.value = CiUpdateCheckState.Checking
            },
            onUpdate = { updates++ },
        )

        compose.runOnIdle { state.value = CiUpdateCheckState.Failed }
        assertExpandsFromHidden()
        val expandedHeight = height()
        val failureMessage = RuntimeEnvironment.getApplication()
            .getString(R.string.ci_update_check_failed_card)
        compose.onNodeWithText(failureMessage).performClick()
        compose.runOnIdle {
            assertEquals(1, retries)
            assertEquals(0, updates)
            assertEquals(CiUpdateCheckState.Checking, state.value)
        }
        assertCollapses(expandedHeight)
        compose.onNodeWithText(failureMessage).assertDoesNotExist()

        compose.runOnIdle { state.value = CiUpdateCheckState.Available(run) }
        assertExpandsFromHidden()
        compose.onNodeWithText(availableMessage()).performClick()
        compose.runOnIdle {
            assertEquals(1, retries)
            assertEquals(1, updates)
        }
    }

    @Test
    fun disabledMotionShowsAndRemovesBothNoticeTypesImmediately() {
        val state = mutableStateOf<CiUpdateCheckState>(CiUpdateCheckState.Checking)
        setNoticeContent(state, preset = MotionPreset.Off)

        listOf(CiUpdateCheckState.Failed, CiUpdateCheckState.Available(run)).forEach { notice ->
            compose.runOnIdle { state.value = notice }
            repeat(2) { frame() }
            val shownHeight = height()
            assertTrue(shownHeight > 0)
            frame()
            assertEquals(shownHeight, height())

            compose.runOnIdle { state.value = CiUpdateCheckState.Hidden }
            repeat(2) { frame() }
            assertEquals(0, sampledHeight())
            compose.onNodeWithText(availableMessage()).assertDoesNotExist()
            compose.onNodeWithText(
                RuntimeEnvironment.getApplication().getString(R.string.ci_update_check_failed_card),
            ).assertDoesNotExist()
            frame()
            compose.onNodeWithTag(NOTICE).assertDoesNotExist()
        }
    }

    private fun setNoticeContent(
        state: State<CiUpdateCheckState>,
        preset: MotionPreset = MotionPreset.QBouncy,
        onRetry: () -> Unit = {},
        onUpdate: () -> Unit = {},
    ) {
        compose.mainClock.autoAdvance = false
        compose.setContent {
            val motion = YukiMotion(preset)
            CompositionLocalProvider(LocalYukiMotion provides motion) {
                MaterialTheme(motionScheme = motion) {
                    Box(Modifier.width(360.dp)) {
                        CiUpdateNotice(state.value, onRetry, onUpdate, Modifier.testTag(NOTICE))
                    }
                }
            }
        }
    }

    private fun assertExpandsFromHidden() {
        val opening = sampleHeights()
        settle()
        val expandedHeight = height()
        assertTrue(expandedHeight > 0)
        assertTrue(opening.any { it in 1 until expandedHeight }, "Notice skipped expansion: $opening")
    }

    private fun assertCollapses(expandedHeight: Int) {
        val closing = sampleHeights()
        assertTrue(closing.any { it in 1 until expandedHeight }, "Notice skipped collapse: $closing")
        settle()
        compose.onNodeWithTag(NOTICE).assertDoesNotExist()
    }

    private fun sampleHeights(): List<Int> = List(5) {
        frame()
        sampledHeight()
    }

    private fun sampledHeight(): Int =
        compose.onAllNodesWithTag(NOTICE, useUnmergedTree = true).fetchSemanticsNodes()
            .singleOrNull()?.size?.height ?: 0

    private fun height(): Int = compose.onNodeWithTag(NOTICE, useUnmergedTree = true)
        .fetchSemanticsNode().size.height

    private fun frame() {
        compose.mainClock.advanceTimeByFrame()
        compose.runOnIdle {}
    }

    private fun settle() {
        compose.mainClock.advanceTimeBy(2_000L)
        compose.runOnIdle {}
    }

    private fun availableMessage(): String = RuntimeEnvironment.getApplication()
        .getString(R.string.ci_update_available_card, run.versionCode)

    private companion object {
        const val NOTICE = "ci-update-notice"
        val run = CiRun(123L, 12345, "01234567", "CI update")
    }
}
