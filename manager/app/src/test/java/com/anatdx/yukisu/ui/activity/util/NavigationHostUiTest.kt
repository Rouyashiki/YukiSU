package com.anatdx.yukisu.ui.activity.util

import android.app.Application
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toPixelMap
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performSemanticsAction
import androidx.navigation.NavHostController
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventDispatcherOwner
import androidx.navigationevent.NavigationEventInput
import androidx.navigationevent.compose.LocalNavigationEventDispatcherOwner
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.motion.YukiMotion
import com.anatdx.yukisu.ui.util.LocalNavigationLeaveGuard
import com.anatdx.yukisu.ui.util.NavigationLeaveGuard
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.math.abs
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class NavigationHostUiTest {
    @get:Rule val compose = createComposeRule()
    private val owner = object : NavigationEventDispatcherOwner {
        override val navigationEventDispatcher = NavigationEventDispatcher()
    }
    private val input = GestureInput()
    private val leaveGuard = NavigationLeaveGuard()
    private lateinit var controller: NavHostController
    private var style by mutableStateOf(PageTransitionStyle.Reveal)
    private var preset by mutableStateOf(MotionPreset.QBouncy)

    @Test
    fun realPredictiveEventsShrinkThePageFromBothEdgesAndCancelWithoutFadingItAway() {
        mount()
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode }
            openDetail()
            val full = bounds("detail")
            for (edge in listOf(NavigationEvent.EDGE_LEFT, NavigationEvent.EDGE_RIGHT)) {
                compose.runOnIdle { input.start(edge) }
                frames(2)
                compose.runOnIdle { input.progress(0.5f, edge) }
                frames(4)
                val preview = bounds("detail")
                assertEquals(full.width * 0.9f, preview.width, 2f, "$mode did not shrink with the finger")
                assertEquals(full.height * 0.9f, preview.height, 2f)
                val direction = if (edge == NavigationEvent.EDGE_LEFT) 1 else -1
                assertTrue((preview.center.x - full.center.x) * direction > full.width * 0.02f)
                assertColor(Color.Red)
                compose.runOnIdle { input.cancel() }
                frames(30)
                assertFullPage("detail", full)
                assertColor(Color.Red)
            }
            compose.onNodeWithTag("back").performClick()
            frames(30)
            assertColor(Color.Blue)
        }
    }

    @Test
    fun completingGesturesAndPressingBackDuringCancellationNeverLeaveAnEmptyPage() {
        mount()
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode }
            repeat(2) {
                openDetail()
                compose.runOnIdle { input.start(NavigationEvent.EDGE_LEFT) }
                frames(2)
                compose.runOnIdle { input.progress(0.7f, NavigationEvent.EDGE_LEFT) }
                frames(3)
                compose.runOnIdle { input.complete() }
                frames(30)
                assertEquals("home", controller.currentDestination?.route)
                assertColor(Color.Blue)

                openDetail()
                compose.runOnIdle {
                    input.start(NavigationEvent.EDGE_RIGHT)
                    input.progress(0.6f, NavigationEvent.EDGE_RIGHT)
                }
                frames(3)
                compose.runOnIdle { input.cancel() }
                frames(1)
                compose.onNodeWithTag("back").performClick()
                frames(30)
                assertEquals("home", controller.currentDestination?.route)
                assertColor(Color.Blue)
            }
        }
    }

    @Test
    fun aLeaveGuardRestoresThePreviewBeforeTheNextSuccessfulReturn() {
        mount()
        openDetail()
        val full = bounds("detail")
        val guardOwner = Any()
        compose.runOnIdle {
            leaveGuard.register(guardOwner, "detail") { _, intercepted -> intercepted() }
            input.start(NavigationEvent.EDGE_LEFT)
        }
        frames(2)
        compose.runOnIdle { input.progress(0.65f, NavigationEvent.EDGE_LEFT) }
        frames(3)
        compose.runOnIdle { input.complete() }
        frames(30)
        assertEquals("detail", controller.currentDestination?.route)
        assertFullPage("detail", full)
        assertColor(Color.Red)
        compose.runOnIdle { leaveGuard.unregister(guardOwner) }
        compose.onNodeWithTag("back").performClick()
        frames(30)
        assertColor(Color.Blue)
    }

    @Test
    fun aGestureCanInterruptEitherForwardStyleWithoutLosingThePage() {
        mount()
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode }
            compose.onNodeWithTag("open").performClick()
            frames(4)
            val entering = bounds("detail")
            val viewport = bounds("viewport")
            when (mode) {
                PageTransitionStyle.Reveal -> assertTrue(
                    entering.width < viewport.width * 0.95f,
                    "Reveal did not visibly scale",
                )
                PageTransitionStyle.Slide -> assertTrue(
                    entering.left > viewport.left + viewport.width * 0.1f,
                    "Slide did not visibly translate",
                )
            }
            compose.runOnIdle { input.start(NavigationEvent.EDGE_LEFT) }
            frames(2)
            val handedOff = bounds("detail")
            assertEquals(entering.left, handedOff.left, 1f, "$mode jumped horizontally at gesture start")
            assertEquals(entering.top, handedOff.top, 1f, "$mode jumped vertically at gesture start")
            assertEquals(entering.width, handedOff.width, 1f)
            assertEquals(entering.height, handedOff.height, 1f)
            compose.runOnIdle { input.progress(0.5f, NavigationEvent.EDGE_LEFT) }
            frames(4)
            assertTrue(bounds("detail").width < viewport.width * 0.96f)
            assertColor(Color.Red, "detail")
            compose.runOnIdle { input.cancel() }
            awaitFullPage("detail", viewport)
            assertFullPage("detail", viewport)
            assertColor(Color.Red)
            frames(2)
            compose.runOnIdle { input.start(NavigationEvent.EDGE_RIGHT) }
            frames(2)
            compose.runOnIdle { input.progress(0.5f, NavigationEvent.EDGE_RIGHT) }
            frames(4)
            assertEquals(viewport.width * 0.9f, bounds("detail").width, 2f)
            assertColor(Color.Red)
            compose.runOnIdle { input.cancel() }
            awaitFullPage("detail", viewport)
            compose.onNodeWithTag("back").performClick()
            frames(30)
            assertColor(Color.Blue)

            compose.onNodeWithTag("open").performClick()
            frames(4)
            compose.runOnIdle { input.start(NavigationEvent.EDGE_RIGHT) }
            frames(2)
            compose.runOnIdle { input.progress(0.5f, NavigationEvent.EDGE_RIGHT) }
            frames(4)
            compose.runOnIdle { input.complete() }
            frames(60)
            assertEquals("home", controller.currentDestination?.route)
            assertFullPage("home", viewport)
            assertColor(Color.Blue)
        }
    }

    @Test
    fun changingTheForwardStyleDoesNotMoveAnAlreadyVisiblePage() {
        mount()
        openDetail()
        val viewport = bounds("viewport")
        for (mode in listOf(PageTransitionStyle.Slide, PageTransitionStyle.Reveal)) {
            compose.runOnIdle { style = mode }
            frames(4)
            assertFullPage("detail", viewport)
            assertColor(Color.Red)
        }
    }

    @Test
    fun disablingMotionImmediatelyFinishesAnEntryAndKeepsFutureEntriesStill() {
        mount()
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode; preset = MotionPreset.QBouncy }
            compose.onNodeWithTag("open").performClick()
            frames(4)
            compose.runOnIdle { preset = MotionPreset.Off }
            frames(2)
            val viewport = bounds("viewport")
            assertFullPage("detail", viewport)
            assertColor(Color.Red)
            compose.onNodeWithTag("back").performClick()
            frames(4)
            assertColor(Color.Blue)
            compose.onNodeWithTag("open").performClick()
            frames(2)
            assertFullPage("detail", viewport)
            assertColor(Color.Red)
            compose.onNodeWithTag("back").performClick()
            frames(4)
        }
    }

    @Test
    fun toolbarReturnFreezesAnUnfinishedEntryBeforeItsExitStarts() {
        mount()
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode }
            compose.onNodeWithTag("open").performClick()
            frames(4)
            val entering = bounds("detail")
            compose.onNodeWithTag("back").performSemanticsAction(SemanticsActions.OnClick) { it() }
            frames(1)
            val exiting = bounds("detail")
            assertEquals(entering.left, exiting.left, 1f)
            assertEquals(entering.top, exiting.top, 1f)
            assertEquals(entering.width, exiting.width, 1f)
            assertEquals(entering.height, exiting.height, 1f)
            frames(4)
            val leaving = bounds("detail")
            if (mode == PageTransitionStyle.Slide) {
                assertEquals(exiting.width, leaving.width, 1f)
                assertEquals(exiting.height, leaving.height, 1f)
            } else {
                assertTrue(leaving.width < exiting.width)
                assertTrue(leaving.height < exiting.height)
            }
            frames(30)
            assertFullPage("home", bounds("viewport"))
            assertColor(Color.Blue)
        }
    }

    @Test
    fun rapidPushesAndReturningToTheUnfinishedMiddlePageKeepPredictiveGeometry() {
        mount()
        compose.runOnIdle { style = PageTransitionStyle.Slide }
        compose.onNodeWithTag("open").performClick()
        frames(4)
        compose.onNodeWithTag("next").performClick()
        frames(4)
        compose.onNodeWithTag("third-back").performClick()
        frames(40)
        val viewport = bounds("viewport")
        assertFullPage("detail", viewport)
        assertColor(Color.Red)
        repeat(2) {
            compose.runOnIdle { input.start(NavigationEvent.EDGE_LEFT) }
            frames(2)
            compose.runOnIdle { input.progress(0.5f, NavigationEvent.EDGE_LEFT) }
            frames(4)
            assertEquals(viewport.width * 0.9f, bounds("detail").width, 2f)
            assertColor(Color.Red)
            compose.runOnIdle { input.cancel() }
            awaitFullPage("detail", viewport)
            frames(2)
            assertColor(Color.Red)
        }
    }

    @Test
    fun consecutiveCompletedGesturesTrackTheSecondFingerWithoutAFrameGap() = consecutiveCompletedGestures(0)

    @Test
    fun consecutiveCompletedGesturesTrackTheSecondFingerAfterTwoFrames() = consecutiveCompletedGestures(2)

    @Test
    fun consecutiveCompletedGesturesTrackTheSecondFingerAfterSettling() = consecutiveCompletedGestures(30)

    private fun consecutiveCompletedGestures(delayFrames: Int) {
        mount()
        val viewport = bounds("viewport")
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode }
            openDetail()
            compose.onNodeWithTag("next").performClick()
            frames(40)
            assertEquals("third", controller.currentDestination?.route)
            assertColor(Color.Green)
            compose.runOnIdle { input.start(NavigationEvent.EDGE_LEFT) }
            frames(2)
            compose.runOnIdle { input.progress(0.65f, NavigationEvent.EDGE_LEFT) }
            frames(3)
            assertEquals(viewport.width * 0.87f, bounds("third").width, 2f)
            assertColor(Color.Green)
            compose.runOnIdle { input.complete() }
            frames(delayFrames)
            assertEquals("detail", controller.currentDestination?.route)
            compose.runOnIdle { input.start(NavigationEvent.EDGE_RIGHT) }
            frames(2)
            for (fraction in listOf(0.2f, 0.5f, 0.8f, 0.35f)) {
                compose.runOnIdle { input.progress(fraction, NavigationEvent.EDGE_RIGHT) }
                frames(4)
                val preview = bounds("detail")
                assertEquals(viewport.width * (1f - 0.2f * fraction), preview.width, 2f,
                    "$mode second gesture after $delayFrames frames did not follow $fraction")
                assertEquals(viewport.height * (1f - 0.2f * fraction), preview.height, 2f)
                assertEquals(viewport.center.x - viewport.width * 0.075f * fraction, preview.center.x, 2f)
                assertColor(Color.Red)
            }
            compose.runOnIdle { input.complete() }
            frames(30)
            assertEquals("home", controller.currentDestination?.route)
            assertFullPage("home", viewport)
            assertColor(Color.Blue)
        }
    }

    private fun mount() {
        owner.navigationEventDispatcher.addInput(input)
        compose.mainClock.autoAdvance = false
        compose.setContent {
            CompositionLocalProvider(
                LocalNavigationEventDispatcherOwner provides owner,
                LocalNavigationLeaveGuard provides leaveGuard,
                LocalYukiMotion provides YukiMotion(preset),
            ) {
                MaterialTheme {
                    controller = rememberNavController()
                    YukiNavigationHost(
                        navController = controller,
                        startDestination = "home",
                        predictiveBackEnabled = true,
                        pageTransitionStyle = style,
                        modifier = Modifier.testTag("viewport"),
                    ) {
                        composable("home") { entry ->
                            YukiNavigationPage(entry.id) {
                                Box(Modifier.fillMaxSize().background(Color.Blue).testTag("home")) {
                                    Button(onClick = { controller.navigate("detail") }, modifier = Modifier.testTag("open")) {
                                        Text("Open")
                                    }
                                }
                            }
                        }
                        composable("detail") { entry ->
                            YukiNavigationPage(entry.id) {
                                Box(Modifier.fillMaxSize().background(Color.Red).testTag("detail")) {
                                    Button(onClick = { controller.popBackStack() }, modifier = Modifier.testTag("back")) {
                                        Text("Back")
                                    }
                                    Button(onClick = { controller.navigate("third") }, modifier = Modifier.align(Alignment.TopEnd).testTag("next")) {
                                        Text("Next")
                                    }
                                }
                            }
                        }
                        composable("third") { entry ->
                            YukiNavigationPage(entry.id) {
                                Box(Modifier.fillMaxSize().background(Color.Green).testTag("third")) {
                                    Button(onClick = { controller.popBackStack() }, modifier = Modifier.testTag("third-back")) {
                                        Text("Back")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        frames(3)
        assertColor(Color.Blue)
    }

    private fun openDetail() {
        compose.onNodeWithTag("open").performClick()
        frames(30)
        assertEquals("detail", controller.currentDestination?.route)
        assertColor(Color.Red)
    }

    private fun frames(count: Int) = repeat(count) {
        compose.mainClock.advanceTimeByFrame()
        compose.runOnIdle {}
    }

    private fun bounds(tag: String): Rect = compose.onNodeWithTag(tag).fetchSemanticsNode().boundsInRoot

    private fun awaitFullPage(tag: String, expected: Rect) {
        repeat(60) {
            frames(1)
            val actual = bounds(tag)
            if (abs(actual.width - expected.width) <= 1f &&
                abs(actual.height - expected.height) <= 1f &&
                abs(actual.left - expected.left) <= 1f
            ) return
        }
        assertFullPage(tag, expected)
    }

    private fun assertFullPage(tag: String, expected: Rect) {
        val actual = bounds(tag)
        assertEquals(expected.width, actual.width, 1f)
        assertEquals(expected.height, actual.height, 1f)
        assertEquals(expected.left, actual.left, 1f)
    }

    private fun assertColor(expected: Color, pageTag: String? = null) {
        val viewport = bounds("viewport")
        val visible = pageTag?.let { bounds(it).intersect(viewport) } ?: viewport
        assertTrue(visible.width > viewport.width * 0.05f && visible.height > viewport.height * 0.05f)
        val pixels = compose.onNodeWithTag("viewport").captureToImage().toPixelMap()
        val actual = pixels[(visible.center.x - viewport.left).toInt(), (visible.center.y - viewport.top).toInt()]
        assertTrue(abs(expected.red - actual.red) < 0.05f && abs(expected.green - actual.green) < 0.05f &&
            abs(expected.blue - actual.blue) < 0.05f,
            "The actual rendered page was $actual, expected $expected")
    }

    private class GestureInput : NavigationEventInput() {
        fun start(edge: Int) = dispatchOnBackStarted(NavigationEvent(swipeEdge = edge))
        fun progress(value: Float, edge: Int) = dispatchOnBackProgressed(NavigationEvent(progress = value, swipeEdge = edge))
        fun cancel() = dispatchOnBackCancelled()
        fun complete() = dispatchOnBackCompleted()
    }
}
