package org.rouyashiki.yukisu.ui.util

import com.topjohnwu.superuser.Shell
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rouyashiki.yukisu.ui.screen.feature.FeatureQuery
import org.rouyashiki.yukisu.ui.screen.feature.FeatureReadQueue

class FeatureStatusReaderTest {
    private class Session(private val root: Boolean = true) : Shell() {
        @Volatile var closed = false
        override fun close() { closed = true }
        override fun isAlive() = !closed
        override fun getStatus() = if (root) ROOT_SHELL else NON_ROOT_SHELL
        override fun waitAndClose(timeout: Long, unit: TimeUnit): Boolean {
            close()
            return true
        }
        override fun execTask(task: Task) = error("unexpected task")
        override fun submitTask(task: Task) = error("unexpected task")
        override fun newJob(): Job = error("unexpected job")
    }

    @Test
    fun thirteenProbesReuseShellsAndResetRetiresBusyAndIdleSessions() = runBlocking {
        val sessions = CopyOnWriteArrayList<Session>()
        val started = CompletableDeferred<Session>()
        val finish = CountDownLatch(1)
        val reader = FeatureStatusReader(open = { Session().also { sessions.add(it) } }) { shell, id ->
            if (id == 0) {
                started.complete(shell as Session)
                check(finish.await(10, TimeUnit.SECONDS))
            }
            "supported"
        }
        val page = launch(start = CoroutineStart.UNDISPATCHED) { reader.withSession { awaitCancellation() } }
        try {
            val slow = async { reader.read(0) }
            val busy = withTimeout(5_000) { started.await() }
            val others = withTimeout(5_000) { (1..12).map { async { reader.read(it) } }.awaitAll() }
            assertEquals(List(12) { "supported" }, others)
            assertTrue(sessions.size in 2..4)
            assertFalse(slow.isCompleted)
            val beforeRefresh = sessions.toList()
            reader.reset()
            assertFalse(busy.closed)
            assertTrue(beforeRefresh.filterNot { it === busy }.all { it.closed })
            assertEquals("supported", reader.read(13))
            finish.countDown()
            assertEquals("supported", slow.await())
            assertTrue(busy.closed)
            assertEquals("supported", reader.read(14))
            assertEquals(beforeRefresh.size + 1, sessions.size)
        } finally {
            finish.countDown()
            page.cancelAndJoin()
        }
        assertTrue(sessions.all { it.closed })
        assertEquals("supported", reader.read(15))
        assertTrue(sessions.last().closed)
    }

    @Test
    fun leavingWithThirteenPendingProbesDrainsAndClosesTheSameFourShells() = runBlocking {
        val sessions = CopyOnWriteArrayList<Session>()
        val started = CountDownLatch(4)
        val release = CountDownLatch(1)
        val reader = FeatureStatusReader(open = { Session().also { sessions.add(it) } }) { _, _ ->
            started.countDown()
            check(release.await(10, TimeUnit.SECONDS))
            "supported"
        }
        val page = launch(start = CoroutineStart.UNDISPATCHED) { reader.withSession { awaitCancellation() } }
        val probes = (0 until 13).map { async(start = CoroutineStart.UNDISPATCHED) { reader.read(it) } }
        try {
            assertTrue(started.await(5, TimeUnit.SECONDS))
            page.cancelAndJoin()
        } finally {
            release.countDown()
            page.cancel()
        }
        assertEquals(List(13) { "supported" }, withTimeout(5_000) { probes.awaitAll() })
        assertEquals(4, sessions.size)
        assertTrue(sessions.all { it.closed })
    }

    @Test
    fun reentrySharesTheProbeQueuedBeforeTheFirstPageClosed() = runBlocking {
        val sessions = CopyOnWriteArrayList<Session>()
        val checked = CopyOnWriteArrayList<Int>()
        val reader = FeatureStatusReader(open = { Session().also { sessions.add(it) } }) { _, id ->
            checked.add(id)
            "supported"
        }
        val queue = FeatureReadQueue(this)
        val query = FeatureQuery(blocksWrites = false, onQueued = reader::keepSessionFor) { reader.read(1) }
        val releaseWrite = CompletableDeferred<Unit>()
        val write = launch(start = CoroutineStart.UNDISPATCHED) { queue.change { releaseWrite.await() } }
        try {
            reader.withSession {
                assertEquals("supported", reader.read(0))
                val leaving = launch(start = CoroutineStart.UNDISPATCHED) { queue.read(query) }
                leaving.cancelAndJoin()
            }
            assertFalse(sessions.single().closed)
            val reentered = async(start = CoroutineStart.UNDISPATCHED) {
                reader.withSession { queue.read(query) }
            }
            releaseWrite.complete(Unit)
            assertEquals("supported", withTimeout(5_000) { reentered.await() })
            assertEquals(listOf(0, 1), checked.toList())
            assertTrue(sessions.single().closed)
        } finally {
            releaseWrite.complete(Unit)
            write.join()
        }
    }

    @Test
    fun failedAndDeadShellsAreReplacedAndKernelStatusesReuseTheShell() = runBlocking {
        val sessions = mutableListOf<Session>()
        val reader = FeatureStatusReader(open = { Session(root = sessions.isNotEmpty()).also { sessions.add(it) } }) { _, id ->
            when (id) {
                1 -> ""
                3 -> "unknown"
                else -> "unsupported"
            }
        }
        reader.withSession {
            assertEquals("unknown", reader.read(0))
            assertTrue(sessions.single().closed)
            assertEquals("unknown", reader.read(1))
            assertTrue(sessions.last().closed)
            assertEquals("unsupported", reader.read(2))
            assertEquals("unknown", reader.read(3))
            assertEquals("unsupported", reader.read(2))
            assertEquals(3, sessions.size)
            sessions.last().close()
            assertEquals("unsupported", reader.read(4))
            assertEquals(4, sessions.size)
        }
        assertTrue(sessions.all { it.closed })
    }
}
