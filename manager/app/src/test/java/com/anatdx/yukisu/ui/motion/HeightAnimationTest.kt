package com.anatdx.yukisu.ui.motion

import androidx.compose.runtime.BroadcastFrameClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class HeightAnimationTest {
    @Test
    fun firstExpansionVisitsIntermediateHeightsBeforeSettling() = runTest {
        enabledPresets.forEach { preset ->
            val frames = FrameDriver(this)
            val animation = HeightAnimation(initialHeight = 0f)
            val motion = YukiMotion(preset)
            val samples = mutableListOf<Int>()

            assertEquals(0, frames.render(animation, 600, motion))
            assertTrue(animation.active, "$preset must start its first expansion")
            frames.settle(animation) {
                samples += frames.render(animation, 600, motion)
            }

            assertTrue(samples.any { it in 1..599 }, "$preset skipped intermediate heights")
            assertEquals(600, samples.last())
        }
    }

    @Test
    fun rapidCloseAndReopenKeepTheCurrentHeightAndEventuallySettle() = runTest {
        bouncyPresets.forEach { preset ->
            val frames = FrameDriver(this)
            val animation = HeightAnimation(initialHeight = 0f)
            val motion = YukiMotion(preset)
            frames.render(animation, 640, motion)
            frames.advance(4) { frames.render(animation, 640, motion) }
            val openingHeight = frames.render(animation, 640, motion)
            assertTrue(openingHeight in 1..639)

            assertEquals(openingHeight.toFloat(), frames.measure(animation, 0, motion), 0.001f)
            assertEquals(openingHeight, frames.render(animation, 0, motion))
            frames.runPending()
            assertEquals(openingHeight, frames.render(animation, 0, motion))
            frames.advance(3) { frames.render(animation, 0, motion) }
            val closingHeight = frames.render(animation, 0, motion)

            assertEquals(closingHeight.toFloat(), frames.measure(animation, 640, motion), 0.001f)
            assertEquals(closingHeight, frames.render(animation, 640, motion))
            frames.runPending()
            assertEquals(closingHeight, frames.render(animation, 640, motion))
            frames.settle(animation) { frames.render(animation, 640, motion) }
            assertEquals(640, frames.render(animation, 640, motion))
        }
    }

    @Test
    fun stableParentTracksEachChildFrameWithoutStartingAnotherAnimation() = runTest {
        val frames = FrameDriver(this)
        val child = HeightAnimation(initialHeight = 0f)
        val parent = HeightAnimation()
        val motion = YukiMotion(MotionPreset.QBouncy)
        val parentPadding = 80
        assertEquals(parentPadding, frames.render(parent, parentPadding, motion, animate = false))
        frames.runPending()
        frames.render(child, 640, motion)
        var intermediateFrames = 0

        frames.settle(child) {
            val childHeight = frames.render(child, 640, motion)
            if (childHeight in 1..639) intermediateFrames++
            val parentHeight = frames.render(parent, parentPadding + childHeight, motion, animate = false)
            assertEquals(parentPadding + childHeight, parentHeight)
            assertFalse(parent.active, "A stable parent must follow the child's measured height")
        }

        assertTrue(intermediateFrames > 0)
        assertEquals(720, frames.render(parent, 720, motion, animate = false))
    }

    @Test
    fun changingAnActiveTargetPreservesHeightBeforeFollowingTheNewDestination() = runTest {
        val frames = FrameDriver(this)
        val animation = HeightAnimation(initialHeight = 0f)
        val motion = YukiMotion(MotionPreset.QBouncy)
        frames.render(animation, 900, motion)
        frames.advance(4) { frames.render(animation, 900, motion) }
        val beforeGrowth = frames.render(animation, 900, motion)

        assertEquals(beforeGrowth.toFloat(), frames.measure(animation, 1400, motion), 0.001f)
        assertEquals(beforeGrowth, frames.render(animation, 1400, motion))
        frames.runPending()
        assertEquals(beforeGrowth, frames.render(animation, 1400, motion))
        frames.advance(4) { frames.render(animation, 1400, motion) }
        val beforeShrink = frames.render(animation, 1400, motion)
        assertTrue(beforeShrink > 250)

        assertEquals(beforeShrink.toFloat(), frames.measure(animation, 250, motion), 0.001f)
        assertEquals(beforeShrink, frames.render(animation, 250, motion))
        frames.runPending()
        assertEquals(beforeShrink, frames.render(animation, 250, motion))
        frames.settle(animation) {
            assertTrue(frames.render(animation, 250, motion) >= 250)
        }
        assertEquals(250, frames.render(animation, 250, motion))
    }

    @Test
    fun disablingMotionReturnsTheDestinationBeforeThePendingCoroutineRuns() = runTest {
        val frames = FrameDriver(this)
        val animation = HeightAnimation(initialHeight = 0f)
        val motion = YukiMotion(MotionPreset.QBouncy)
        val off = YukiMotion(MotionPreset.Off)
        frames.render(animation, 1000, motion)
        frames.advance(4) { frames.render(animation, 1000, motion) }
        val intermediate = frames.render(animation, 1000, motion)
        assertTrue(intermediate in 1..999)

        assertEquals(1000f, frames.measure(animation, 1000, off))
        assertEquals(1000, frames.render(animation, 1000, off))
        assertFalse(animation.active)
        frames.runPending()
        frames.advance(120) {
            assertEquals(1000, frames.render(animation, 1000, off))
            assertFalse(animation.active)
        }
        assertEquals(0f, frames.measure(animation, 0, off))
        assertEquals(0, frames.render(animation, 0, off))
        assertFalse(animation.active)
        frames.runPending()
    }

    @Test
    fun shrinkingLongContentNeverUndershootsOrRevealsMoreAfterReachingItsDestination() = runTest {
        bouncyPresets.forEach { preset ->
            val frames = FrameDriver(this)
            val animation = HeightAnimation()
            val motion = YukiMotion(preset)
            assertEquals(1600, frames.render(animation, 1600, motion, animate = false))
            frames.runPending()
            assertEquals(1600, frames.render(animation, 240, motion))
            var reachedDestination = false
            var rawSpringCrossedDestination = false
            var intermediateFrames = 0

            frames.settle(animation) {
                val rawHeight = frames.measure(animation, 240, motion)
                val visibleHeight = animation.boundedHeight(rawHeight, 240, motion.overshootDp)
                if (rawHeight < 240f) rawSpringCrossedDestination = true
                if (visibleHeight in 241..1599) intermediateFrames++
                assertTrue(visibleHeight >= 240, "$preset shrank below its nonzero destination")
                if (reachedDestination) {
                    assertEquals(240, visibleHeight, "$preset re-exposed space after closing")
                }
                if (visibleHeight == 240) reachedDestination = true
            }

            assertTrue(intermediateFrames > 0)
            assertTrue(rawSpringCrossedDestination, "$preset must exercise the contraction boundary")
            assertTrue(reachedDestination)
            assertEquals(240, frames.render(animation, 240, motion))
        }
    }

    @Test
    fun retargetingWhileTheRawSpringIsBelowTheClosedBoundaryStartsAtTheVisibleHeight() = runTest {
        bouncyPresets.forEach { preset ->
            listOf(200, 800).forEach { destination ->
                val frames = FrameDriver(this)
                val animation = HeightAnimation()
                val motion = YukiMotion(preset)
                frames.render(animation, 1600, motion, animate = false)
                frames.runPending()
                frames.render(animation, 240, motion)
                var rawHeight = 1600f
                var visibleHeight = 1600
                var count = 0

                while (rawHeight >= 240f && animation.active && count < 300) {
                    frames.advance()
                    rawHeight = frames.measure(animation, 240, motion)
                    visibleHeight = animation.boundedHeight(rawHeight, 240, motion.overshootDp)
                    count++
                }
                assertTrue(rawHeight < 240f, "$preset must still have momentum below the visible boundary")
                assertTrue(animation.active)
                assertEquals(240, visibleHeight)

                assertEquals(240f, frames.measure(animation, destination, motion))
                assertEquals(240, frames.render(animation, destination, motion))
                frames.runPending()
                assertEquals(240f, frames.measure(animation, destination, motion))
                assertEquals(240, frames.render(animation, destination, motion))
                frames.settle(animation) { frames.render(animation, destination, motion) }
                assertEquals(destination, frames.render(animation, destination, motion))
            }
        }
    }

    private class FrameDriver(private val testScope: TestScope) {
        private val clock = BroadcastFrameClock()
        private val scope = CoroutineScope(testScope.backgroundScope.coroutineContext + clock)
        private var timeNanos = 0L

        fun measure(
            animation: HeightAnimation,
            height: Int,
            motion: YukiMotion,
            animate: Boolean = true,
        ): Float = animation.measure(height, animate, motion, MotionRole.Container, scope)

        fun render(
            animation: HeightAnimation,
            height: Int,
            motion: YukiMotion,
            animate: Boolean = true,
        ): Int {
            val rawHeight = measure(animation, height, motion, animate)
            assertTrue(rawHeight.isFinite())
            return animation.boundedHeight(rawHeight, height, motion.overshootDp)
        }

        fun runPending() = testScope.runCurrent()

        fun advance(count: Int = 1, afterFrame: () -> Unit = {}) {
            repeat(count) {
                runPending()
                timeNanos += 16_000_000L
                clock.sendFrame(timeNanos)
                runPending()
                afterFrame()
            }
        }

        fun settle(animation: HeightAnimation, afterFrame: () -> Unit) {
            var count = 0
            do {
                advance(afterFrame = afterFrame)
                count++
            } while (animation.active && count < 300)
            assertFalse(animation.active, "Animation did not settle within 4.8 seconds of frames")
            runPending()
        }
    }

    private companion object {
        val enabledPresets = listOf(MotionPreset.Soft, MotionPreset.QBouncy)
        val bouncyPresets = listOf(MotionPreset.QBouncy)
    }
}
