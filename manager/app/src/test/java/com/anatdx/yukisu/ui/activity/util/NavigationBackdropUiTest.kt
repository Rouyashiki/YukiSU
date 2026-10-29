package com.anatdx.yukisu.ui.activity.util

import android.app.Application
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.GraphicsContext
import androidx.compose.ui.graphics.layer.GraphicsLayer
import androidx.compose.ui.graphics.toPixelMap
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalGraphicsContext
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.navigation.NavHostController
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventDispatcherOwner
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
import kotlin.test.assertTrue
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class NavigationBackdropUiTest {
    @get:Rule val compose = createComposeRule()
    private lateinit var controller: NavHostController
    private val leaveGuard = NavigationLeaveGuard()
    private lateinit var graphics: TrackingGraphicsContext
    private var preset by mutableStateOf(MotionPreset.QBouncy)
    private var style by mutableStateOf(PageTransitionStyle.Reveal)
    private var viewportFraction by mutableStateOf(1f)
    private var mounted by mutableStateOf(true)
    private val owner = object : NavigationEventDispatcherOwner {
        override val navigationEventDispatcher = NavigationEventDispatcher()
    }

    @Test
    fun bottomBarCrossfadesKeepThePreviousTabBehindEveryEntryFrame() {
        mount()
        repeat(3) {
            for (route in listOf("settings", "home")) {
                compose.runOnIdle {
                    controller.navigate(route) {
                        popUpTo("home") { saveState = true }
                        launchSingleTop = true
                        restoreState = true
                    }
                }
                repeat(25) { frame(); assertNoWindowBackground() }
                assertColor(if (route == "home") Color.Blue else Color.Red)
            }
        }
    }

    @Test
    fun aRevealKeepsThePreviousPageAtTheUncoveredEdges() {
        mount()
        compose.runOnIdle { controller.navigate("detail") }
        repeat(4) { frame(); assertNoWindowBackground() }
        val pixels = compose.onNodeWithTag("viewport").captureToImage().toPixelMap()
        assertPixel(Color.Blue, pixels[pixels.width - 2, pixels.height - 2])
        repeat(28) { frame() }
        assertColor(Color.Red)
    }

    @Test
    fun rapidForwardEntriesRetainThePresentedFrameAndReleaseItAfterSettling() {
        mount()
        compose.runOnIdle { controller.navigate("detail") }
        repeat(4) { frame(); assertNoWindowBackground() }
        compose.runOnIdle { controller.navigate("third") }
        repeat(25) { frame(); assertNoWindowBackground() }
        assertColor(Color.Green)
        compose.runOnIdle { controller.popBackStack() }
        repeat(30) { frame() }
        assertColor(Color.Red)
        compose.runOnIdle { controller.popBackStack() }
        repeat(30) { frame() }
        assertColor(Color.Blue)
    }

    @Test
    fun interruptingEntryWithAnOrdinaryReturnReleasesTheRetainedFrames() {
        mount()
        val baseline = compose.runOnIdle { graphics.active.size }
        for (mode in PageTransitionStyle.entries) {
            compose.runOnIdle { style = mode; controller.navigate("detail") }
            repeat(4) { frame() }
            assertNoWindowBackground()
            assertTrue(compose.runOnIdle { graphics.active.size } > baseline)
            compose.runOnIdle { controller.popBackStack() }
            repeat(30) { frame() }
            assertColor(Color.Blue)
            assertEquals(baseline, compose.runOnIdle { graphics.active.size })
        }
    }

    @Test
    fun disablingMotionOrResizingFinishesEntryAndReleasesItsBackdrop() {
        mount()
        val baseline = compose.runOnIdle { graphics.active.size }
        compose.runOnIdle { controller.navigate("detail") }
        repeat(4) { frame() }
        compose.runOnIdle { preset = MotionPreset.Off }
        repeat(3) { frame() }
        assertColor(Color.Red)
        assertEquals(baseline, compose.runOnIdle { graphics.active.size })
        compose.runOnIdle { controller.popBackStack(); preset = MotionPreset.QBouncy }
        repeat(30) { frame() }
        assertColor(Color.Blue)
        compose.runOnIdle { controller.navigate("detail") }
        repeat(4) { frame() }
        compose.runOnIdle { viewportFraction = 0.8f }
        repeat(3) { frame() }
        assertColor(Color.Red)
        assertEquals(baseline, compose.runOnIdle { graphics.active.size })
    }

    @Test
    fun disposingTheHostDuringEntryReleasesEveryOwnedLayer() {
        mount()
        compose.runOnIdle { controller.navigate("detail") }
        repeat(4) { frame() }
        assertNoWindowBackground()
        compose.runOnIdle { mounted = false }
        repeat(3) { frame() }
        assertEquals(0, compose.runOnIdle { graphics.active.size })
    }

    private fun mount() {
        compose.mainClock.autoAdvance = false
        compose.setContent {
            val context = LocalGraphicsContext.current
            graphics = remember(context) { TrackingGraphicsContext(context) }
            CompositionLocalProvider(
                LocalNavigationEventDispatcherOwner provides owner,
                LocalYukiMotion provides YukiMotion(preset),
                LocalNavigationLeaveGuard provides leaveGuard,
                LocalGraphicsContext provides graphics,
            ) {
                MaterialTheme {
                    controller = rememberNavController()
                    if (mounted) {
                        YukiNavigationHost(
                            navController = controller,
                            startDestination = "home",
                            predictiveBackEnabled = true,
                            pageTransitionStyle = style,
                            bottomBarRoutes = setOf("home", "settings"),
                            modifier = Modifier.fillMaxSize(viewportFraction).background(Color.Magenta).testTag("viewport"),
                        ) {
                            for ((route, color) in listOf(
                                "home" to Color.Blue, "settings" to Color.Red,
                                "detail" to Color.Red, "third" to Color.Green,
                            )) {
                                composable(route) { entry ->
                                    YukiNavigationPage(entry.id) {
                                        Box(Modifier.fillMaxSize().background(color))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        repeat(3) { frame() }
        assertColor(Color.Blue)
    }

    private fun frame() {
        compose.mainClock.advanceTimeByFrame()
        compose.runOnIdle {}
    }

    private fun assertNoWindowBackground() {
        val pixels = compose.onNodeWithTag("viewport").captureToImage().toPixelMap()
        for (x in listOf(2, pixels.width / 2, pixels.width - 2)) {
            for (y in listOf(2, pixels.height / 2, pixels.height - 2)) {
                val pixel = pixels[x, y]
                // White antialiasing preserves this bound; the magenta window background exceeds it.
                assertTrue(pixel.red + pixel.blue - pixel.green <= 1.05f,
                    "The window background leaked at ($x, $y): $pixel")
            }
        }
    }

    private fun assertColor(expected: Color) {
        val pixels = compose.onNodeWithTag("viewport").captureToImage().toPixelMap()
        assertPixel(expected, pixels[pixels.width / 2, pixels.height / 2])
    }

    private fun assertPixel(expected: Color, actual: Color) {
        assertTrue(abs(expected.red - actual.red) < 0.05f &&
            abs(expected.green - actual.green) < 0.05f && abs(expected.blue - actual.blue) < 0.05f,
            "Rendered $actual, expected $expected")
    }

    private class TrackingGraphicsContext(private val delegate: GraphicsContext) : GraphicsContext by delegate {
        val active = mutableSetOf<GraphicsLayer>()

        override fun createGraphicsLayer(): GraphicsLayer = delegate.createGraphicsLayer().also { active += it }

        override fun releaseGraphicsLayer(layer: GraphicsLayer) {
            assertTrue(active.remove(layer), "A layer was released twice")
            delegate.releaseGraphicsLayer(layer)
        }
    }
}
