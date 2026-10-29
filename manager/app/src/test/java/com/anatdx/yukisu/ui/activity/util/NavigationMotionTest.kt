package com.anatdx.yukisu.ui.activity.util

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.SeekableTransitionState
import androidx.compose.animation.core.TargetBasedAnimation
import androidx.compose.animation.core.VectorConverter
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.rememberTransition
import androidx.compose.runtime.AbstractApplier
import androidx.compose.runtime.BroadcastFrameClock
import androidx.compose.runtime.Composition
import androidx.compose.runtime.Recomposer
import androidx.compose.runtime.State
import androidx.compose.runtime.snapshots.Snapshot
import androidx.navigationevent.NavigationEvent
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.motion.YukiMotion
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.math.max
import kotlin.math.min
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class NavigationMotionTest {
    @Test
    fun cancellingAHandoffPreservesTheReleaseGeometryAndReturnsToIdentity() {
        val entry = ForwardPageMotion(PageMotion(YukiMotion()), NavigationOrigin(0.1f, 0.9f), bottomBar = false)
        val original = entry.transform()
        assertEquals(-0.112f, original.offset, 0.00001f)
        assertEquals(0.112f, original.offsetY, 0.00001f)
        val gestures = BackGestureState({ "detail" }, { "home" })
        gestures.registerEntry("detail", entry)
        gestures.onStarted(NavigationEvent(swipeEdge = NavigationEvent.EDGE_LEFT))
        assertEquals(original, gestures.session!!.entryTransform)
        assertEquals(ForwardPageTransform(), entry.transform())
        gestures.onProgressed(NavigationEvent(progress = 0.6f, swipeEdge = NavigationEvent.EDGE_LEFT))
        val released = predictivePageTransform(gestures.session!!, outgoing = true, progress = 0.6f)
        gestures.onCancelled()
        assertEquals(0.6f, gestures.session!!.releaseProgress)
        assertEquals(released, predictivePageTransform(gestures.session!!, outgoing = true, progress = 0.6f))
        val middle = predictivePageTransform(gestures.session!!, outgoing = true, progress = 0.3f)
        assertEquals((1f + released.scale) / 2f, middle.scale, 0.00001f)
        assertEquals(released.offset / 2f, middle.offset, 0.00001f)
        assertEquals(released.offsetY / 2f, middle.offsetY, 0.00001f)
        assertEquals(PredictivePageTransform(), predictivePageTransform(gestures.session!!, outgoing = true, progress = 0f))
    }

    @Test
    fun springSlidesKeepTheirLeftEdgeInBoundsWhileTheScaleRebounds() {
        for (preset in listOf(MotionPreset.Soft, MotionPreset.QBouncy)) {
            val motion = PageMotion(YukiMotion(preset), PageTransitionStyle.Slide)
            assertTrue(motion.usesForwardSlide)
            assertMonotonic(motion.slideOffsetSpec(), 1f, 0f)

            val scale = animation(motion.slideScaleSpec(), motion.slideOpeningScale, 1f)
            val values = (0L..scale.durationNanos step 1_000_000L).map(scale::getValueFromNanos)
            assertTrue(values.all { it.isFinite() && it >= motion.slideOpeningScale && it < 1.05f })
            if (preset == MotionPreset.QBouncy) {
                assertTrue(values.max() > 1.01f, "The classic slide lost its visible spring rebound")
            }
            assertEquals(1f, scale.getValueFromNanos(scale.durationNanos))
        }
    }

    @Test
    fun offDisablesBothSlideChannelsAndTheSlideEntry() {
        val motion = PageMotion(YukiMotion(MotionPreset.Off), PageTransitionStyle.Slide)
        assertTrue(!motion.usesForwardSlide)
        assertTrue(!motion.enabled)
        assertEquals(EnterTransition.None, motion.popEnter())
        assertEquals(ExitTransition.None, motion.popExit())
        assertEquals(0L, animation(motion.slideOffsetSpec(), 1f, 0f).durationNanos)
        assertEquals(0L, animation(motion.slideScaleSpec(), motion.slideOpeningScale, 1f).durationNanos)
    }

    @Test
    fun allPageChannelsStayMonotonicAndInsideTheirEndpoints() {
        MotionPreset.entries.forEach { preset ->
            val motion = PageMotion(YukiMotion(preset))
            assertMonotonic(motion.enterSpec(), motion.openingScale, 1f)
            assertMonotonic(motion.enterSpec(), 0f, 1f)
            assertMonotonic(motion.returnSpec(), 1f, motion.closingScale)
            assertMonotonic(motion.returnFadeSpec(), 1f, 0f)
        }
    }

    @Test
    fun offRemovesPageTransitionsAndAllAnimationTime() {
        val motion = PageMotion(YukiMotion(MotionPreset.Off))
        assertTrue(!motion.enabled)
        assertEquals(ExitTransition.None, motion.popExit())
        listOf(motion.enterSpec<Float>(), motion.returnSpec(), motion.returnFadeSpec()).forEach { spec ->
            val animation = animation(spec, 0f, 1f)
            assertEquals(0L, animation.durationNanos)
            assertEquals(1f, animation.getValueFromNanos(0L))
        }
    }

    @Test
    fun predictiveReturnKeepsTheOutgoingPageOpaqueAtTheStartOfTheGesture() {
        listOf(MotionPreset.Soft, MotionPreset.QBouncy).forEach { preset ->
            val motion = PageMotion(YukiMotion(preset))
            val fade = animation(motion.returnFadeSpec(), 1f, 0f)
            val scale = animation(motion.returnSpec(), 1f, motion.closingScale)
            assertEquals(1f, fade.getValueFromNanos(40_000_000L))
            assertTrue(scale.getValueFromNanos(40_000_000L) < 1f)
            assertEquals(scale.durationNanos, fade.durationNanos)
        }
    }

    @Test
    fun repeatedPredictiveCancellationAndButtonCompletionRestoreAVisiblePage() = runTest {
        listOf(MotionPreset.Soft, MotionPreset.QBouncy).forEach { preset ->
            val motion = PageMotion(YukiMotion(preset))
            val frames = PageTransitionFrames(this, motion)
            try {
                repeat(3) {
                    for (fraction in listOf(0.1f, 0.4f, 0.8f, 0.55f, 0.2f, 0f)) {
                        frames.runOperation { state.seekTo(fraction, Page.Hidden) }
                        frames.assertInBounds()
                    }
                    frames.runOperation { state.snapTo(Page.Visible) }
                    frames.assertVisible()

                    frames.runOperation { state.animateTo(Page.Hidden) }
                    assertEquals(0f, frames.alpha)
                    assertEquals(motion.closingScale, frames.scale)
                    frames.runOperation { state.snapTo(Page.Visible) }
                    frames.assertVisible()

                    frames.runOperation { state.seekTo(0.65f, Page.Hidden) }
                    frames.runOperation { state.animateTo(Page.Hidden) }
                    assertEquals(0f, frames.alpha)
                    frames.runOperation { state.snapTo(Page.Visible) }
                    frames.assertVisible()
                }
            } finally {
                frames.close()
            }
        }
    }

    @Test
    fun tapCoordinatesAreNormalizedAndClampedToTheViewport() {
        val tracker = NavigationOriginTracker()
        tracker.recordTap(54f, 384f, 1080, 1920, 100L)
        val origin = tracker.originFor("home", "settings", 110L)
        assertEquals(0.05f, origin.x)
        assertEquals(0.2f, origin.y)
        assertEquals(origin.x, origin.transformOrigin().pivotFractionX)
        assertEquals(origin.y, origin.transformOrigin().pivotFractionY)

        tracker.recordTap(-10f, 2000f, 1080, 1920, 120L)
        assertEquals(NavigationOrigin(0f, 1f), tracker.originFor("settings", "details", 121L))
    }

    @Test
    fun aTapIsConsumedOnceWhileTheSameTransitionKeepsItsOriginalPivot() {
        val tracker = NavigationOriginTracker()
        tracker.recordTap(20f, 40f, 100, 200, 100L)
        val origin = tracker.originFor("home", "settings", 110L)
        assertEquals(origin, tracker.originFor("home", "settings", 120L))
        assertEquals(NavigationOrigin(), tracker.originFor("settings", "details", 130L))

        tracker.recordTap(20f, 40f, 100, 200, 200L)
        assertEquals(origin, tracker.originFor("details", "editor", 210L))
        tracker.recordTap(80f, 160f, 100, 200, 220L)
        assertEquals(origin, tracker.originFor("details", "editor", 230L))
        assertEquals(NavigationOrigin(0.8f, 0.8f), tracker.originFor("editor", "help", 240L))
    }

    @Test
    fun missingExpiredFutureAndInvalidTapsUseTheCenter() {
        assertEquals(NavigationOrigin(), NavigationOriginTracker().originFor("home", "settings", 100L))
        listOf(99L, 851L).forEach { navigationTime ->
            val tracker = NavigationOriginTracker()
            tracker.recordTap(10f, 90f, 100, 100, 100L)
            assertEquals(NavigationOrigin(), tracker.originFor("home", "settings", navigationTime))
        }
        val inclusiveBoundary = NavigationOriginTracker()
        inclusiveBoundary.recordTap(10f, 90f, 100, 100, 100L)
        assertEquals(NavigationOrigin(0.1f, 0.9f), inclusiveBoundary.originFor("home", "settings", 850L))
        assertEquals(NavigationOrigin(0.1f, 0.9f), inclusiveBoundary.originFor("home", "settings", 9000L))

        listOf(
            floatArrayOf(Float.NaN, 20f, 100f, 100f),
            floatArrayOf(20f, Float.POSITIVE_INFINITY, 100f, 100f),
            floatArrayOf(20f, 20f, 0f, 100f),
            floatArrayOf(20f, 20f, 100f, 0f),
        ).forEach { (x, y, width, height) ->
            val tracker = NavigationOriginTracker()
            tracker.recordTap(10f, 10f, 100, 100, 100L)
            tracker.recordTap(x, y, width.toInt(), height.toInt(), 110L)
            assertEquals(NavigationOrigin(), tracker.originFor("home", "settings", 120L))
        }
    }

    private fun assertMonotonic(spec: FiniteAnimationSpec<Float>, start: Float, end: Float) {
        val animation = animation(spec, start, end)
        val times = (0L..animation.durationNanos step 1_000_000L).toList() + animation.durationNanos
        val values = times.distinct().map(animation::getValueFromNanos)
        assertTrue(values.all { it.isFinite() && it in min(start, end)..max(start, end) })
        assertTrue(values.zipWithNext().all { (before, after) -> if (end >= start) after >= before else after <= before })
        if (animation.durationNanos > 0L) {
            assertEquals(start, values.first())
            assertTrue(values.any { it > min(start, end) && it < max(start, end) })
        }
        assertEquals(end, values.last())
    }

    private fun animation(spec: FiniteAnimationSpec<Float>, start: Float, end: Float) =
        TargetBasedAnimation(spec, Float.VectorConverter, start, end)

    private enum class Page { Visible, Hidden }

    // Exercises Compose's seekable transition clock with production specs, without mounting a NavHost.
    private class PageTransitionFrames(private val testScope: TestScope, private val motion: PageMotion) {
        val state = SeekableTransitionState(Page.Visible)
        private val clock = BroadcastFrameClock()
        private val scope = CoroutineScope(testScope.backgroundScope.coroutineContext + clock)
        private val recomposer = Recomposer(scope.coroutineContext)
        private val composition = Composition(NoOpApplier(), recomposer)
        private val runner = scope.launch { recomposer.runRecomposeAndApplyChanges() }
        private lateinit var scaleState: State<Float>
        private lateinit var alphaState: State<Float>
        private var timeNanos = 0L
        val scale get() = scaleState.value
        val alpha get() = alphaState.value

        init {
            composition.setContent {
                val transition = rememberTransition(state, "page test")
                scaleState = transition.animateFloat(transitionSpec = { motion.returnSpec() }, label = "scale") {
                    if (it == Page.Visible) 1f else motion.closingScale
                }
                alphaState = transition.animateFloat(transitionSpec = { motion.returnFadeSpec() }, label = "opacity") {
                    if (it == Page.Visible) 1f else 0f
                }
            }
            drain()
        }

        suspend fun runOperation(operation: suspend PageTransitionFrames.() -> Unit) {
            val job = scope.async { operation() }
            var frames = 0
            do {
                drain()
                timeNanos += 16_000_000L
                clock.sendFrame(timeNanos)
                drain()
                assertInBounds()
                frames++
            } while (!job.isCompleted && frames < 120)
            assertTrue(job.isCompleted, "Page transition did not complete within 1.92 seconds of frames")
            job.await()
        }

        fun assertInBounds() {
            assertTrue(scale.isFinite() && scale in motion.closingScale..1f, "Page scale escaped its endpoints: $scale")
            assertTrue(alpha.isFinite() && alpha in 0f..1f, "Page opacity escaped its endpoints: $alpha")
        }

        fun assertVisible() {
            assertEquals(Page.Visible, state.currentState)
            assertEquals(Page.Visible, state.targetState)
            assertEquals(1f, scale)
            assertEquals(1f, alpha)
        }

        private fun drain() {
            Snapshot.sendApplyNotifications()
            testScope.runCurrent()
            Snapshot.sendApplyNotifications()
            testScope.runCurrent()
        }

        fun close() {
            composition.dispose()
            recomposer.cancel()
            runner.cancel()
        }
    }

    private class NoOpApplier : AbstractApplier<Unit>(Unit) {
        override fun onClear() = Unit
        override fun insertTopDown(index: Int, instance: Unit) = error("Unexpected UI node")
        override fun insertBottomUp(index: Int, instance: Unit) = error("Unexpected UI node")
        override fun move(from: Int, to: Int, count: Int) = error("Unexpected UI node")
        override fun remove(index: Int, count: Int) = error("Unexpected UI node")
    }
}
