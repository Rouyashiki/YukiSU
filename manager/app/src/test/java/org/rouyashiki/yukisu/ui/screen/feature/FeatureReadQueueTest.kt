package org.rouyashiki.yukisu.ui.screen.feature

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Test
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.ui.screen.changeDefaultUmount
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class FeatureReadQueueTest {
    @Test
    fun completedOperationsReleaseTheirReferencesEvenWhenTheReaderLeaves() = runBlocking {
        val background = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
        val queue = FeatureReadQueue(background) { true }
        try {
            val finish = CompletableDeferred<Unit>()
            val query = FeatureQuery { finish.await() }
            val waiter = async(start = CoroutineStart.UNDISPATCHED) { queue.read(query) }
            val task = requireNotNull(query.pending)
            waiter.cancelAndJoin()
            finish.complete(Unit)
            task.join()
            assertNull(query.pending)

            val immediate = FeatureQuery { true }
            assertEquals(true, queue.read(immediate))
            assertNull(immediate.pending)
            assertEquals(true, queue.isManager())
            queue.change { Unit }
            val failed = runCatching { queue.change { error("write failed") } }
            assertEquals("write failed", failed.exceptionOrNull()?.message)
            val retained = FeatureReadQueue::class.java.declaredFields.filter {
                it.isAccessible = true
                Job::class.java.isAssignableFrom(it.type) && it.get(queue) != null
            }
            assertEquals(emptyList<String>(), retained.map { it.name })
        } finally {
            background.cancel()
        }
    }

    @Test
    fun completingAnEarlierWriteKeepsTheNewerWriteAsAReadBarrier() = runTest {
        val queue = FeatureReadQueue(this)
        val releaseFirst = CompletableDeferred<Unit>()
        val releaseSecond = CompletableDeferred<Unit>()
        val first = async(start = CoroutineStart.UNDISPATCHED) { queue.change { releaseFirst.await() } }
        val second = async(start = CoroutineStart.UNDISPATCHED) { queue.change { releaseSecond.await() } }
        releaseFirst.complete(Unit)
        first.await()
        val read = async(start = CoroutineStart.UNDISPATCHED) { queue.read(FeatureQuery { true }) }
        testScheduler.runCurrent()
        assertFalse(read.isCompleted)
        releaseSecond.complete(Unit)
        second.await()
        assertEquals(true, read.await())
    }

    @Test
    fun pendingIdentityCheckIsSharedAndTheNextAccessRechecksAuthorization() = runBlocking {
        val background = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val entered = CompletableDeferred<Unit>()
        val finish = CountDownLatch(1)
        val checks = AtomicInteger()
        val queue = FeatureReadQueue(background) {
            val first = checks.incrementAndGet() == 1
            entered.complete(Unit)
            check(finish.await(10, TimeUnit.SECONDS))
            first
        }
        try {
            withTimeout(5_000) {
                val settings = async(start = CoroutineStart.UNDISPATCHED) { queue.isManager() }
                entered.await()
                val features = async(start = CoroutineStart.UNDISPATCHED) { queue.isManager() }
                queue.change { Unit }
                finish.countDown()
                assertEquals(true, settings.await())
                assertEquals(true, features.await())
                assertEquals(1, checks.get())
                assertFalse(queue.isManager())
            }
        } finally {
            finish.countDown()
            background.cancel()
        }
    }

    @Test
    fun blockedMigrationOnlyDelaysTheNoNewPrivsReading() = runTest {
        val queue = FeatureReadQueue(this)
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        var migrations = 0
        var statusReads = 0
        val defaults = featureQuery(
            Natives.FEATURE_DEFAULT_NO_NEW_PRIVS,
            migrateDefaults = {
                migrations++
                entered.complete(Unit)
                release.await()
                false
            },
            readStatus = {
                check(release.isCompleted)
                statusReads++
                "forced"
            },
            readValue = { false },
        )
        val adbRoot = featureQuery(
            Natives.FEATURE_ADB_ROOT,
            migrateDefaults = { error("unrelated migration") },
            readStatus = { "supported" },
            readValue = { true },
        )

        val first = async { queue.read(defaults) }
        entered.await()
        val otherRow = async { queue.read(adbRoot) }
        val umount = async { queue.change { changeDefaultUmount(true, write = { true }) } }
        val reentry = async { queue.read(defaults) }
        testScheduler.runCurrent()
        assertFalse(first.isCompleted)
        assertFalse(reentry.isCompleted)
        assertEquals(true, otherRow.isCompleted)
        assertEquals(true, umount.isCompleted)
        assertEquals(FeatureReading(true, "supported"), otherRow.await())
        assertEquals(true to true, umount.await())
        release.complete(Unit)
        assertEquals(FeatureReading(false, "forced"), first.await())
        assertEquals(FeatureReading(false, "forced"), reentry.await())
        assertEquals(1, migrations)
        assertEquals(2, statusReads)
        assertEquals(FeatureReading(false, "forced"), queue.read(defaults))
        assertEquals(2, migrations)
    }

    @Test
    fun reentryReadsTheCurrentValueAfterTheSharedStatusProbe() = runTest {
        val queue = FeatureReadQueue(this)
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        var enabled = false
        var probes = 0
        val query = featureQuery(
            Natives.FEATURE_SULOG,
            readStatus = {
                probes++
                entered.complete(Unit)
                release.await()
                "supported"
            },
            readValue = { enabled },
        )
        val first = async { queue.read(query) }
        entered.await()
        first.cancelAndJoin()
        enabled = true
        val reentry = async(start = CoroutineStart.UNDISPATCHED) { queue.read(query) }
        assertFalse(reentry.isCompleted)
        release.complete(Unit)
        assertEquals(FeatureReading(true, "supported"), reentry.await())
        assertEquals(1, probes)
    }

    @Test
    fun slowFeatureProbeDoesNotDelayAWriteAndDiscardsItsStaleValue() = runTest {
        val queue = FeatureReadQueue(this)
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        var current = false
        var calls = 0
        val query = FeatureQuery(blocksWrites = false) {
            calls++
            val value = current
            entered.complete(Unit)
            release.await()
            value
        }
        val probe = async { queue.read(query) }
        entered.await()
        queue.change { current = true }
        val reentered = async { queue.read(query) }
        testScheduler.runCurrent()
        assertFalse(probe.isCompleted)
        assertFalse(reentered.isCompleted)
        assertEquals(1, calls)
        release.complete(Unit)
        assertEquals(true, probe.await())
        assertEquals(true, reentered.await())
        assertEquals(2, calls)
    }

    @Test
    fun missingValueOnlyMasksSupportedFeatures() {
        assertEquals("unknown", FeatureToggleState(null, "supported").displayStatus)
        assertEquals("supported", FeatureToggleState(false, "supported").displayStatus)
        assertEquals("unsupported", FeatureToggleState(null, "unsupported").displayStatus)
    }

    @Test
    fun navigatingBackWhileBlockedReusesProbeOnReentry() = runBlocking {
        withTimeout(5000) {
            val queue = FeatureReadQueue(this)
            val entered = CompletableDeferred<Unit>()
            val release = CompletableDeferred<Unit>()
            val calls = AtomicInteger()
            val probe = FeatureQuery {
                calls.incrementAndGet()
                entered.complete(Unit)
                release.await()
                true
            }
            val first = async { queue.read(probe) }
            entered.await()
            first.cancelAndJoin()
            val second = async(start = CoroutineStart.UNDISPATCHED) { queue.read(probe) }
            assertFalse(second.isCompleted)
            release.complete(Unit)
            assertEquals(true, second.await())
            assertEquals(1, calls.get())
        }
    }

    @Test
    fun writeWaitsForEveryPriorReadAndBlocksLaterReads() = runTest {
        val queue = FeatureReadQueue(this)
        val first = CompletableDeferred<Unit>()
        val second = CompletableDeferred<Unit>()
        val writing = CompletableDeferred<Unit>()
        val finishWrite = CompletableDeferred<Unit>()
        val events = mutableListOf<String>()
        val a = async {
            queue.read(FeatureQuery {
                events += "a"
                first.await()
            })
        }
        val b = async {
            queue.read(FeatureQuery {
                events += "b"
                second.await()
            })
        }
        testScheduler.runCurrent()
        assertEquals(setOf("a", "b"), events.toSet())

        val write = async(start = CoroutineStart.UNDISPATCHED) {
            queue.change {
                writing.complete(Unit)
                finishWrite.await()
            }
        }
        val after = async(start = CoroutineStart.UNDISPATCHED) {
            queue.read(FeatureQuery { events += "after" })
        }
        first.complete(Unit)
        testScheduler.runCurrent()
        assertFalse(writing.isCompleted)
        second.complete(Unit)
        writing.await()
        assertFalse(after.isCompleted)
        finishWrite.complete(Unit)
        write.await()
        after.await()
        a.await()
        b.await()
        assertEquals(1, events.count { it == "after" })
    }

    @Test
    fun pendingReadRechecksAfterWriteAndFreshEntryProbesAgain() = runBlocking {
        withTimeout(5000) {
            val queue = FeatureReadQueue(this)
            val entered = CompletableDeferred<Unit>()
            val release = CompletableDeferred<Unit>()
            var value = 0
            var calls = 0
            val probe = FeatureQuery {
                calls++
                val previous = value
                entered.complete(Unit)
                release.await()
                previous
            }
            val before = async { queue.read(probe) }
            entered.await()
            val write = async(start = CoroutineStart.UNDISPATCHED) { queue.change { value = 1 } }
            val after = async(start = CoroutineStart.UNDISPATCHED) { queue.read(probe) }
            release.complete(Unit)
            write.await()
            assertEquals(1, before.await())
            assertEquals(1, after.await())
            val completedCalls = calls
            value = 2
            assertEquals(2, queue.read(probe))
            assertEquals(completedCalls + 1, calls)
        }
    }
}
