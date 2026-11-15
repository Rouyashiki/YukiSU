package org.rouyashiki.yukisu.ui.activity.util

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class IntentDispatcherTest {
    @Test fun recreationDoesNotSubmitTheLaunchIntentAgain() = runTest {
        var saved: List<String>? = null
        val queue = IntentQueue<String>(save = { saved = it })
        queue.initialize("launch")
        queue.initialize("launch")
        assertEquals("launch", queue.next())
        queue.complete()
        assertEquals(emptyList(), saved)
        queue.close()
    }

    @Test fun processRestoreKeepsThePendingRequestAheadOfLaterRequests() = runTest {
        var saved: List<String>? = null
        val queue = IntentQueue<String>(save = { saved = it })
        queue.initialize("pending ZIP")
        assertEquals("pending ZIP", queue.next())
        queue.add("later link")
        queue.close()

        val restored = IntentQueue(saved, save = { saved = it })
        restored.initialize("launch intent")
        assertEquals("pending ZIP", restored.next())
        restored.complete()
        restored.complete()
        assertEquals(listOf("later link"), saved)
        assertEquals("later link", restored.next())
        restored.complete()
        assertEquals(emptyList(), saved)
        restored.close()
    }

    @Test fun processRestoreDoesNotReplayCompletedRequests() = runTest {
        var saved: List<String>? = null
        val queue = IntentQueue<String>(save = { saved = it })
        queue.initialize("completed action")
        queue.next()
        queue.complete()
        queue.close()

        val restored = IntentQueue(saved, save = { saved = it })
        restored.initialize("completed action")
        val next = async { restored.next() }
        runCurrent()
        assertFalse(next.isCompleted)
        restored.add("new action")
        assertEquals("new action", next.await())
        restored.complete()
        restored.close()
    }

    @Test fun cancellationDuringDeliveryStillRestoresTheRequest() = runTest {
        var saved: List<String>? = null
        val queue = IntentQueue<String>(save = { saved = it })
        queue.initialize(null)
        val consumer = launch { queue.next() }
        runCurrent()
        queue.add("incoming ZIP")
        consumer.cancelAndJoin()
        queue.close()

        val restored = IntentQueue(saved)
        assertEquals("incoming ZIP", restored.next())
        restored.complete()
        restored.close()
    }

    @Test fun authenticationKeepsTheRequestPendingUntilAccessIsGranted() = runTest {
        var manager = false
        var saved: List<String>? = null
        val gate = ManagerAccessGate { manager }
        val queue = IntentQueue<String>(save = { saved = it })
        queue.initialize("module action")
        val handled = mutableListOf<String>()
        val consumer = launch {
            val request = queue.next()
            gate.awaitReady()
            handled += request
            queue.complete()
        }
        runCurrent()
        gate.notifyChanged()
        runCurrent()
        assertFalse(consumer.isCompleted)
        assertEquals(emptyList(), handled)
        assertEquals(listOf("module action"), saved)

        manager = true
        gate.notifyChanged()
        consumer.join()
        assertEquals(listOf("module action"), handled)
        assertEquals(emptyList(), saved)
        queue.close()
    }

    @Test fun authenticationBeforeSubscriptionDoesNotLoseTheWakeup() = runTest {
        val gate = ManagerAccessGate { true }
        gate.notifyChanged()
        val ready = async { gate.awaitReady() }
        runCurrent()
        assertTrue(ready.isCompleted)
        ready.await()
    }

    @Test fun cancelledAuthenticationWaitCanResumeAfterRecreation() = runTest {
        var manager = false
        val gate = ManagerAccessGate { manager }
        val oldConsumer = launch { gate.awaitReady() }
        runCurrent()
        oldConsumer.cancelAndJoin()
        manager = true
        gate.notifyChanged()
        val newConsumer = async { gate.awaitReady() }
        runCurrent()
        assertTrue(newConsumer.isCompleted)
        newConsumer.await()
    }

    @Test fun anOldAuthenticationSignalDoesNotAuthorizeRevokedAccess() = runTest {
        var manager = true
        val gate = ManagerAccessGate { manager }
        gate.notifyChanged()
        manager = false
        val ready = async { gate.awaitReady() }
        runCurrent()
        assertFalse(ready.isCompleted)
        manager = true
        gate.notifyChanged()
        ready.await()
    }
}
