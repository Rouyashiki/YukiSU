package org.rouyashiki.yukisu.ui.util

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class KasumiConflictControllerTest {
    private val first = KasumiConflict(1, KasumiConflict.LOADED_MODULE, "susfs_guard_lkm")
    private val second = KasumiConflict(2, KasumiConflict.BUILTIN_CONFIG, "CONFIG_KSU_SUSFS=y")

    @Test
    fun unsupportedKernelDoesNotProduceAWarning() = runTest {
        val controller = KasumiConflictController(read = { null }, acknowledge = { error("Unexpected ack") })
        controller.refresh()
        controller.confirm()
        assertTrue(controller.state.value.events.isEmpty())
    }

    @Test
    fun warningRemainsUntilExplicitConfirmation() = runTest {
        var reads = 0
        var acknowledgements = 0
        val controller = KasumiConflictController(
            read = { reads++; listOf(first) },
            acknowledge = { acknowledgements++; true },
        )
        controller.refresh()
        repeat(3) { controller.refresh() }
        assertEquals(listOf(first), controller.state.value.events)
        assertEquals(1, reads)
        assertEquals(0, acknowledgements)
        controller.confirm()
        assertTrue(controller.state.value.events.isEmpty())
        assertEquals(1, acknowledgements)
    }

    @Test
    fun failedAcknowledgementPreservesTheWarningForRetry() = runTest {
        var succeeds = false
        val controller = KasumiConflictController(read = { listOf(first) }, acknowledge = { succeeds })
        controller.refresh()
        controller.confirm()
        assertEquals(listOf(first), controller.state.value.events)
        assertTrue(controller.state.value.confirmationFailed)
        assertFalse(controller.state.value.confirming)
        succeeds = true
        controller.confirm()
        assertTrue(controller.state.value.events.isEmpty())
        assertFalse(controller.state.value.confirmationFailed)
    }

    @Test
    fun confirmationUsesOnlyTheDisplayedSnapshot() = runTest {
        var kernelEvents = listOf(first)
        val ackStarted = CompletableDeferred<Long>()
        val finishAck = CompletableDeferred<Unit>()
        val controller = KasumiConflictController(
            read = { kernelEvents },
            acknowledge = { sequence ->
                ackStarted.complete(sequence)
                finishAck.await()
                kernelEvents = kernelEvents.filter { it.sequence > sequence }
                true
            },
        )
        controller.refresh()
        val confirm = launch { controller.confirm() }
        assertEquals(1L, ackStarted.await())
        assertTrue(controller.state.value.confirming)
        kernelEvents = listOf(first, second)
        val refresh = launch { controller.refresh() }
        runCurrent()
        assertEquals(listOf(first), controller.state.value.events)
        finishAck.complete(Unit)
        confirm.join()
        refresh.join()
        assertEquals(listOf(second), controller.state.value.events)
    }

    @Test
    fun cancelledConfirmationDoesNotDismissTheWarning() = runTest {
        val ackStarted = CompletableDeferred<Unit>()
        val controller = KasumiConflictController(
            read = { listOf(first) },
            acknowledge = { ackStarted.complete(Unit); CompletableDeferred<Boolean>().await() },
        )
        controller.refresh()
        val confirm = launch { controller.confirm() }
        ackStarted.await()
        confirm.cancel()
        confirm.join()
        assertEquals(listOf(first), controller.state.value.events)
        assertFalse(controller.state.value.confirming)
    }

    @Test
    fun multipleConflictsAreConfirmedTogetherInSequenceOrder() = runTest {
        var sequence = 0L
        val controller = KasumiConflictController(
            read = { listOf(second, first) },
            acknowledge = { sequence = it; true },
        )
        controller.refresh()
        assertEquals(listOf(first, second), controller.state.value.events)
        controller.confirm()
        assertEquals(2L, sequence)
        assertTrue(controller.state.value.events.isEmpty())
    }
}
