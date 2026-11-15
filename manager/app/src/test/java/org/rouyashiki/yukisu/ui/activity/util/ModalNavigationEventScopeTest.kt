package org.rouyashiki.yukisu.ui.activity.util

import androidx.compose.runtime.AbstractApplier
import androidx.compose.runtime.Composition
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Recomposer
import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventDispatcherOwner
import androidx.navigationevent.NavigationEventHandler
import androidx.navigationevent.NavigationEventInfo
import androidx.navigationevent.NavigationEventInput
import androidx.navigationevent.compose.LocalNavigationEventDispatcherOwner
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals

class ModalNavigationEventScopeTest {
    @Test
    fun buttonBackDuringSheetEntryDoesNotPopThePage() = withFixture { fixture ->
        fixture.sheet.isBackEnabled = false
        fixture.input.complete()
        assertEquals(emptyList(), fixture.page.events)
        assertEquals(emptyList(), fixture.sheet.events)
    }

    @Test
    fun gestureStartedDuringEntryDoesNotPopThePageAfterTheSheetBecomesVisible() = withFixture { fixture ->
        fixture.sheet.isBackEnabled = false
        fixture.input.start()
        fixture.sheet.isBackEnabled = true
        fixture.input.progress()
        fixture.input.complete()
        assertEquals(emptyList(), fixture.page.events)
    }

    @Test
    fun backDuringSheetExitDoesNotPopThePage() = withFixture { fixture ->
        fixture.input.start()
        fixture.sheet.isBackEnabled = false
        fixture.input.complete()
        fixture.input.complete()
        assertEquals(emptyList(), fixture.page.events)
    }

    @Test
    fun removingTheSheetDuringAGestureDoesNotRedirectCompletionToThePage() = withFixture { fixture ->
        fixture.input.start()
        fixture.sheet.remove()
        fixture.input.complete()
        assertEquals(listOf("start", "cancel"), fixture.sheet.events)
        assertEquals(emptyList(), fixture.page.events)
    }

    @Test
    fun visibleSheetStillReceivesThePredictiveGesture() = withFixture { fixture ->
        fixture.input.start()
        fixture.input.progress()
        fixture.input.complete()
        assertEquals(listOf("start", "progress", "complete"), fixture.sheet.events)
        assertEquals(emptyList(), fixture.page.events)
    }

    @Test
    fun disposingTheModalLeavesPageNavigationWorking() = withFixture { fixture ->
        fixture.composition.dispose()
        fixture.pageInput.complete()
        assertEquals(listOf("complete"), fixture.page.events)
        assertEquals(emptyList(), fixture.sheet.events)
    }

    private fun withFixture(block: (Fixture) -> Unit) = runTest {
        val fixture = Fixture()
        val recomposer = Recomposer(coroutineContext)
        val composition = Composition(NoOpApplier(), recomposer)
        fixture.composition = composition
        try {
            composition.setContent {
                CompositionLocalProvider(LocalNavigationEventDispatcherOwner provides fixture.pageOwner) {
                    ModalNavigationEventScope {
                        fixture.modal = checkNotNull(LocalNavigationEventDispatcherOwner.current).navigationEventDispatcher
                    }
                }
            }
            fixture.modal.addHandler(fixture.sheet)
            val disconnect = bridgeWindowBackEvents(fixture.window, fixture.modal)
            try {
                block(fixture)
            } finally {
                disconnect()
            }
        } finally {
            composition.dispose()
            recomposer.cancel()
            fixture.pageOwner.navigationEventDispatcher.dispose()
            fixture.window.dispose()
        }
    }

    private class Fixture {
        val pageOwner = object : NavigationEventDispatcherOwner {
            override val navigationEventDispatcher = NavigationEventDispatcher()
        }
        val window = NavigationEventDispatcher()
        val input = TestInput()
        val pageInput = TestInput()
        val page = RecordingHandler()
        val sheet = RecordingHandler()
        lateinit var modal: NavigationEventDispatcher
        lateinit var composition: Composition

        init {
            window.addInput(input)
            pageOwner.navigationEventDispatcher.addInput(pageInput)
            pageOwner.navigationEventDispatcher.addHandler(page)
        }
    }

    private class RecordingHandler : NavigationEventHandler<NavigationEventInfo>(
        NavigationEventInfo.None,
        isBackEnabled = true,
    ) {
        val events = mutableListOf<String>()
        override fun onBackStarted(event: NavigationEvent) { events += "start" }
        override fun onBackProgressed(event: NavigationEvent) { events += "progress" }
        override fun onBackCancelled() { events += "cancel" }
        override fun onBackCompleted() { events += "complete" }
    }

    private class TestInput : NavigationEventInput() {
        fun start() = dispatchOnBackStarted(NavigationEvent(swipeEdge = NavigationEvent.EDGE_LEFT))
        fun progress() = dispatchOnBackProgressed(NavigationEvent(progress = 0.6f))
        fun complete() = dispatchOnBackCompleted()
    }

    private class NoOpApplier : AbstractApplier<Unit>(Unit) {
        override fun insertTopDown(index: Int, instance: Unit) = Unit
        override fun insertBottomUp(index: Int, instance: Unit) = Unit
        override fun move(from: Int, to: Int, count: Int) = Unit
        override fun remove(index: Int, count: Int) = Unit
        override fun onClear() = Unit
    }
}
