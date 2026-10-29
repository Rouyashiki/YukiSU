package com.anatdx.yukisu.ui.motion

import androidx.compose.runtime.BroadcastFrameClock
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.runtime.snapshots.SnapshotStateObserver
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class HeightAnimationObservationTest {
    @Test
    fun openingInvalidatesTheObservedLayoutBeforeTheAnimationFinishes() = runTest {
        val layout = ObservedHeight(this, initialHeight = 0f)
        try {
            layout.request(600)
            layout.remeasure()
            assertEquals(0, layout.height)

            layout.advance(6)

            assertTrue(layout.height in 1..599, "Opening did not deliver its first visible frames: ${layout.height}")
            assertTrue(layout.measurements > 3, "Animation frames must invalidate layout without unrelated UI changes")
        } finally {
            layout.close()
        }
    }

    @Test
    fun aPreviouslyVisibleRowAnimatesEveryCloseAndReopenUsingOnlySnapshotInvalidations() = runTest {
        val layout = ObservedHeight(this)
        try {
            layout.request(200, animate = false)
            layout.advance(2)
            assertEquals(200, layout.height)

            repeat(3) {
                layout.request(0)
                layout.remeasure()
                layout.advance(5)
                assertTrue(layout.height in 1..199, "Close $it skipped all intermediate heights: ${layout.height}")
                layout.advance(120)
                assertEquals(0, layout.height)

                layout.request(200)
                layout.remeasure()
                layout.advance(5)
                assertTrue(layout.height in 1..199, "Open $it skipped all intermediate heights: ${layout.height}")
                layout.advance(120)
                assertEquals(200, layout.height)
            }
        } finally {
            layout.close()
        }
    }

    @Test
    fun contentSizeChangesSubscribeToFramesAfterStartingFromAnAlreadyRenderedHeight() = runTest {
        val layout = ObservedHeight(this)
        try {
            layout.request(240, animate = false)
            layout.advance(2)
            layout.request(900)
            layout.remeasure()
            assertEquals(240, layout.height)

            layout.advance(6)

            assertTrue(layout.height in 241..899, "Content growth stalled at its previous rendered height: ${layout.height}")
            layout.advance(120)
            assertEquals(900, layout.height)
        } finally {
            layout.close()
        }
    }

    private class ObservedHeight(testScope: TestScope, initialHeight: Float? = null) {
        private val scheduler = testScope
        private val clock = BroadcastFrameClock()
        private val scope = CoroutineScope(testScope.backgroundScope.coroutineContext + clock)
        private val animation = HeightAnimation(initialHeight)
        private val motion = YukiMotion(MotionPreset.QBouncy)
        private val observer = SnapshotStateObserver { it() }
        private val observation = Any()
        private val onChanged: (Any) -> Unit = { invalidated = true }
        private var invalidated = false
        private var target = 0
        private var animate = true
        private var timeNanos = 0L
        var height = 0
            private set
        var measurements = 0
            private set

        init {
            observer.start()
        }

        fun request(height: Int, animate: Boolean = true) {
            target = height
            this.animate = animate
            remeasure()
        }

        fun remeasure() {
            invalidated = false
            observer.observeReads(observation, onChanged) {
                val raw = animation.measure(target, animate, motion, MotionRole.Container, scope)
                height = animation.boundedHeight(raw, target, motion.overshootDp)
                measurements++
            }
            Snapshot.notifyObjectsInitialized()
        }

        fun advance(frames: Int) {
            repeat(frames) {
                scheduler.runCurrent()
                dispatchInvalidations()
                timeNanos += 16_000_000L
                clock.sendFrame(timeNanos)
                scheduler.runCurrent()
                dispatchInvalidations()
            }
        }

        private fun dispatchInvalidations() {
            Snapshot.sendApplyNotifications()
            var passes = 0
            while (invalidated) {
                remeasure()
                Snapshot.sendApplyNotifications()
                check(++passes < 10) { "Layout failed to settle between animation frames" }
            }
        }

        fun close() {
            observer.stop()
            observer.clear()
        }
    }
}
