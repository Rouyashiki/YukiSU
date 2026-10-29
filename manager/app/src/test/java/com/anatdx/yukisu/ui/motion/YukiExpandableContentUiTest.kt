package com.anatdx.yukisu.ui.motion

import android.app.Application
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeveloperMode
import androidx.compose.material.icons.filled.Update
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.LookaheadScope
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.ui.screen.SwitchItem
import com.anatdx.yukisu.ui.theme.LocalUiStyle
import com.anatdx.yukisu.ui.theme.UiStyle
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class YukiExpandableContentUiTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun firstOpeningRemeasuresWithoutAnUnrelatedAnimatedWidget() {
        val visible = mutableStateOf(false)
        compose.mainClock.autoAdvance = false
        compose.setContent {
            MotionTheme {
                YukiExpandableContent(visible.value, Modifier.testTag("expansion")) {
                    Box(Modifier.fillMaxWidth().height(96.dp))
                }
            }
        }
        compose.onNodeWithTag("expansion").assertDoesNotExist()
        compose.runOnIdle { visible.value = true }

        val opening = sampleHeights("expansion")
        settle()
        val expandedHeight = height("expansion")

        assertTrue(expandedHeight > 0)
        assertTrue(opening.any { it in 1 until expandedHeight }, "No visible opening frame within 80 ms: $opening")
        assertTrue(opening.distinct().size > 1, "The layout did not subscribe to animation frames: $opening")
    }

    @Test
    fun classicUpdateSwitchAnimatesItsCiRowOnEveryToggle() {
        checkUpdateSwitch(UiStyle.Classic)
    }

    @Test
    fun expressiveUpdateSwitchAnimatesItsCiRowOnEveryToggle() {
        checkUpdateSwitch(UiStyle.Expressive)
    }

    @Test
    fun nestedDisclosureFollowsItsChildAndSurvivesAnInterruptedParentClose() {
        val parentVisible = mutableStateOf(true)
        val childVisible = mutableStateOf(false)
        compose.mainClock.autoAdvance = false
        compose.setContent {
            MotionTheme {
                LookaheadScope {
                    YukiExpandableContent(parentVisible.value, Modifier.testTag("parent")) {
                        Box(Modifier.fillMaxWidth().height(24.dp))
                        YukiExpandableContent(childVisible.value, Modifier.testTag("child")) {
                            Box(Modifier.fillMaxWidth().height(96.dp))
                        }
                    }
                }
            }
        }
        val collapsedHeight = height("parent")
        compose.runOnIdle { childVisible.value = true }
        repeat(5) {
            frame()
            assertEquals(collapsedHeight + sampledHeight("child"), height("parent"))
        }
        compose.runOnIdle { parentVisible.value = false }
        repeat(3) { frame() }
        compose.runOnIdle {
            childVisible.value = false
            parentVisible.value = true
        }
        sampleHeights("parent").forEach { assertTrue(it >= 0) }
        settle()
        assertEquals(collapsedHeight, height("parent"))
        compose.onNodeWithTag("child").assertDoesNotExist()
    }

    @Test
    fun contentSizeAnimationDeliversIntermediateFramesInsideLookahead() {
        val expanded = mutableStateOf(false)
        compose.mainClock.autoAdvance = false
        compose.setContent {
            MotionTheme {
                LookaheadScope {
                    Box(
                        Modifier.testTag("sized")
                            .yukiAnimateContentSize()
                            .fillMaxWidth()
                            .height(if (expanded.value) 240.dp else 96.dp),
                    )
                }
            }
        }
        val initialHeight = height("sized")
        compose.runOnIdle { expanded.value = true }
        val growing = sampleHeights("sized")
        settle()
        val expandedHeight = height("sized")
        assertTrue(expandedHeight > initialHeight)
        assertTrue(growing.any { it > initialHeight && it < expandedHeight }, "Content growth skipped intermediate layout: $growing")

        compose.runOnIdle { expanded.value = false }
        val shrinking = sampleHeights("sized")
        assertTrue(shrinking.any { it > initialHeight && it < expandedHeight }, "Content shrink skipped intermediate layout: $shrinking")
        settle()
        assertEquals(initialHeight, height("sized"))
    }

    private fun checkUpdateSwitch(style: UiStyle) {
        val checkUpdate = mutableStateOf(true)
        compose.mainClock.autoAdvance = false
        compose.setContent {
            MotionTheme(style) {
                LookaheadScope {
                    Card {
                        Column {
                            SwitchItem(
                                icon = Icons.Filled.Update,
                                title = "Check updates",
                                checked = checkUpdate.value,
                                onCheckedChange = { checkUpdate.value = it },
                            )
                            YukiExpandableContent(checkUpdate.value, Modifier.testTag("ci-row")) {
                                SwitchItem(
                                    icon = Icons.Filled.DeveloperMode,
                                    title = "Check CI updates",
                                    checked = false,
                                    onCheckedChange = {},
                                )
                            }
                        }
                    }
                }
            }
        }
        val expandedHeight = height("ci-row")
        assertTrue(expandedHeight > 0)
        repeat(3) { attempt ->
            compose.onNodeWithText("Check updates").performClick()
            val closing = sampleHeights("ci-row")
            assertTrue(closing.any { it in 1 until expandedHeight }, "$style close $attempt jumped or stalled: $closing")
            settle()
            compose.onNodeWithTag("ci-row").assertDoesNotExist()

            compose.onNodeWithText("Check updates").performClick()
            val opening = sampleHeights("ci-row")
            assertTrue(opening.any { it in 1 until expandedHeight }, "$style open $attempt jumped or stalled: $opening")
            settle()
            assertEquals(expandedHeight, height("ci-row"))
        }
    }

    private fun height(tag: String): Int =
        compose.onNodeWithTag(tag, useUnmergedTree = true).fetchSemanticsNode().size.height

    private fun sampleHeights(tag: String): List<Int> = List(5) {
        frame()
        sampledHeight(tag)
    }

    private fun sampledHeight(tag: String): Int =
        compose.onAllNodesWithTag(tag, useUnmergedTree = true).fetchSemanticsNodes()
            .singleOrNull()?.size?.height ?: 0

    private fun frame() {
        compose.mainClock.advanceTimeByFrame()
        compose.runOnIdle {}
    }

    private fun settle() {
        compose.mainClock.advanceTimeBy(2_000L)
        compose.runOnIdle {}
    }

    @Composable
    private fun MotionTheme(style: UiStyle = UiStyle.Classic, content: @Composable () -> Unit) {
        val motion = YukiMotion(MotionPreset.QBouncy)
        CompositionLocalProvider(LocalYukiMotion provides motion, LocalUiStyle provides style) {
            MaterialTheme(motionScheme = motion, content = content)
        }
    }
}
