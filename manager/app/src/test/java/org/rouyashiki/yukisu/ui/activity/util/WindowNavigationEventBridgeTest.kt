package org.rouyashiki.yukisu.ui.activity.util

import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventHandler
import androidx.navigationevent.NavigationEventInfo
import androidx.navigationevent.NavigationEventInput
import kotlin.test.Test
import kotlin.test.assertEquals

class WindowNavigationEventBridgeTest {
    @Test
    fun predictiveGestureReachesTheSheetWithoutPoppingThePage() {
        for (edge in listOf(NavigationEvent.EDGE_LEFT, NavigationEvent.EDGE_RIGHT)) {
            val fixture = Fixture()
            val disconnect = bridgeWindowBackEvents(fixture.window, fixture.inherited)
            fixture.input.start(edge)
            fixture.input.progress(0.6f)
            fixture.input.complete()
            assertEquals(listOf("start:$edge", "progress:0.6", "complete"), fixture.sheet.events)
            assertEquals(emptyList(), fixture.page.events)
            disconnect()
        }
    }

    @Test
    fun cancellationReachesTheSheetWithoutCompletingIt() {
        val fixture = Fixture()
        val disconnect = bridgeWindowBackEvents(fixture.window, fixture.inherited)
        fixture.input.start(NavigationEvent.EDGE_LEFT)
        fixture.input.progress(0.4f)
        fixture.input.cancel()
        assertEquals(listOf("start:0", "progress:0.4", "cancel"), fixture.sheet.events)
        assertEquals(emptyList(), fixture.page.events)
        disconnect()
    }

    @Test
    fun buttonBackReachesTheSheet() {
        val fixture = Fixture()
        val disconnect = bridgeWindowBackEvents(fixture.window, fixture.inherited)
        fixture.input.complete()
        assertEquals(listOf("complete"), fixture.sheet.events)
        assertEquals(emptyList(), fixture.page.events)
        disconnect()
    }

    @Test
    fun disconnectCancelsTheGestureAndStopsForwarding() {
        val fixture = Fixture()
        val disconnect = bridgeWindowBackEvents(fixture.window, fixture.inherited)
        fixture.input.start(NavigationEvent.EDGE_RIGHT)
        disconnect()
        fixture.input.complete()
        assertEquals(listOf("start:1", "cancel"), fixture.sheet.events)
        assertEquals(emptyList(), fixture.page.events)
    }

    @Test
    fun cleanupToleratesEitherDispatcherBeingDisposedFirst() {
        for (disposeWindow in listOf(false, true)) {
            val fixture = Fixture()
            val disconnect = bridgeWindowBackEvents(fixture.window, fixture.inherited)
            fixture.input.start(NavigationEvent.EDGE_LEFT)
            if (disposeWindow) fixture.window.dispose() else fixture.inherited.dispose()
            disconnect()
        }
    }

    @Test
    fun sharedDispatcherDoesNotCreateAForwardingLoop() {
        val dispatcher = NavigationEventDispatcher()
        val input = TestInput()
        val handler = RecordingHandler()
        dispatcher.addInput(input)
        dispatcher.addHandler(handler)
        val disconnect = bridgeWindowBackEvents(dispatcher, dispatcher)
        input.complete()
        disconnect()
        input.complete()
        assertEquals(listOf("complete", "complete"), handler.events)
    }

    private class Fixture {
        val window = NavigationEventDispatcher()
        val inherited = NavigationEventDispatcher()
        val input = TestInput()
        val page = RecordingHandler()
        val sheet = RecordingHandler()

        init {
            window.addInput(input)
            inherited.addHandler(page)
            inherited.addHandler(sheet)
        }
    }

    private class RecordingHandler : NavigationEventHandler<NavigationEventInfo>(
        NavigationEventInfo.None,
        isBackEnabled = true,
    ) {
        val events = mutableListOf<String>()
        override fun onBackStarted(event: NavigationEvent) { events += "start:${event.swipeEdge}" }
        override fun onBackProgressed(event: NavigationEvent) { events += "progress:${event.progress}" }
        override fun onBackCancelled() { events += "cancel" }
        override fun onBackCompleted() { events += "complete" }
    }

    private class TestInput : NavigationEventInput() {
        fun start(edge: Int) = dispatchOnBackStarted(NavigationEvent(swipeEdge = edge))
        fun progress(value: Float) = dispatchOnBackProgressed(NavigationEvent(progress = value))
        fun cancel() = dispatchOnBackCancelled()
        fun complete() = dispatchOnBackCompleted()
    }
}
