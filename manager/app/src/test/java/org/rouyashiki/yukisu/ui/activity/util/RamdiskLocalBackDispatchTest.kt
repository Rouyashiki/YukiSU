package org.rouyashiki.yukisu.ui.activity.util

import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventHandler
import androidx.navigationevent.NavigationEventInfo
import androidx.navigationevent.NavigationEventInput
import org.rouyashiki.yukisu.ui.util.NavigationLeaveGuard
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertSame

/** Verifies the dispatcher contract used by the embedded YukiFB back handler. */
class RamdiskLocalBackDispatchTest {
    @Test
    fun localGestureIsForwardedWithoutStartingARouteLeaveTransaction() {
        for (predictiveEnabled in listOf(false, true)) {
            val dispatcher = NavigationEventDispatcher()
            val parent = NavigationEventDispatcher()
            val systemInput = TestInput()
            val input = ForwardedBackInput()
            val guard = NavigationLeaveGuard()
            var routeLeaves = 0
            guard.register(this, "ramdisk") { _, intercepted -> routeLeaves++; intercepted() }
            val bridge = GuardedBackHandler(input, guard, { predictiveEnabled }) { BackTarget("editor", "ramdisk") }
            var completed = 0
            var cancelled = 0
            var progress = 0f
            val local = object : NavigationEventHandler<NavigationEventInfo>(NavigationEventInfo.None, isBackEnabled = true) {
                override fun onBackProgressed(event: NavigationEvent) { progress = event.progress }
                override fun onBackCompleted() { completed++ }
                override fun onBackCancelled() { cancelled++ }
            }
            dispatcher.addInput(input)
            dispatcher.addHandler(local)
            parent.addInput(systemInput)
            parent.addHandler(bridge)
            bridge.setHostStarted(true)
            // The self-drawn NavHost only starts a page gesture for NavBackStackEntryInfo.
            val history = dispatcher.history.value
            assertSame(NavigationEventInfo.None, history.mergedHistory[history.currentIndex])
            systemInput.start(NavigationEvent.EDGE_LEFT)
            systemInput.progress(0.5f)
            assertEquals(0.5f, progress)
            systemInput.cancel()
            assertEquals(1, cancelled)
            assertEquals(0, completed)
            systemInput.start(NavigationEvent.EDGE_RIGHT)
            systemInput.complete()
            assertEquals(1, completed)
            assertEquals(0, routeLeaves)
            local.remove()
            bridge.remove()
            dispatcher.removeInput(input)
            parent.removeInput(systemInput)
        }
    }

    private class TestInput : NavigationEventInput() {
        fun start(edge: Int) = dispatchOnBackStarted(NavigationEvent(swipeEdge = edge))
        fun progress(value: Float) = dispatchOnBackProgressed(NavigationEvent(progress = value))
        fun cancel() = dispatchOnBackCancelled()
        fun complete() = dispatchOnBackCompleted()
    }
}
