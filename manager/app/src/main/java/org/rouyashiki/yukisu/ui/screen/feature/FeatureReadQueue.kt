package org.rouyashiki.yukisu.ui.screen.feature

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.joinAll
import org.rouyashiki.yukisu.Natives

internal class FeatureQuery<T>(
    val blocksWrites: Boolean = true,
    val onQueued: ((Job) -> Unit)? = null,
    val prepare: suspend () -> Unit = {},
    val load: suspend () -> T,
) {
    internal var generation = -1L
    internal var pending: Deferred<T>? = null
}

internal class FeatureReadQueue(
    private val scope: CoroutineScope,
    private val checkManager: () -> Boolean = { Natives.isManager },
) {
    private var managerRead: Deferred<Boolean>? = null
    private var generation = 0L
    private var lastWrite: Job? = null
    private val writeBarriers = mutableSetOf<Job>()

    suspend fun isManager(): Boolean = synchronized(this) {
        managerRead?.takeUnless { it.isCompleted } ?: scope.async { checkManager() }.also { task ->
            managerRead = task
            task.invokeOnCompletion {
                synchronized(this) { if (managerRead === task) managerRead = null }
            }
        }
    }.await()

    private fun <T> enqueue(
        after: List<Job>,
        blocksWrites: Boolean,
        onQueued: ((Job) -> Unit)? = null,
        block: suspend () -> T,
    ): Deferred<T> {
        val task = scope.async(start = CoroutineStart.LAZY) {
            after.joinAll()
            block()
        }
        if (blocksWrites) {
            writeBarriers.add(task)
            task.invokeOnCompletion { synchronized(this) { writeBarriers.remove(task) } }
        }
        onQueued?.invoke(task)
        task.start()
        return task
    }

    suspend fun <T> read(query: FeatureQuery<T>): T {
        var load: suspend () -> T = {
            query.prepare()
            query.load()
        }
        while (true) {
            val (revision, task) = synchronized(this) {
                val existing = query.pending
                val current = if (existing != null && !existing.isCompleted) {
                    existing
                } else {
                    enqueue(listOfNotNull(lastWrite), query.blocksWrites, query.onQueued, load).also { task ->
                        query.generation = generation
                        query.pending = task
                        task.invokeOnCompletion {
                            synchronized(this) { if (query.pending === task) query.pending = null }
                        }
                    }
                }
                query.generation to current
            }
            val value = task.await()
            if (synchronized(this) { revision == generation }) return value
            load = query.load
        }
    }

    suspend fun <T> change(block: suspend () -> T): T {
        val task = synchronized(this) {
            generation++
            enqueue(writeBarriers.toList(), true, block = block).also { task ->
                lastWrite = task
                task.invokeOnCompletion {
                    synchronized(this) { if (lastWrite === task) lastWrite = null }
                }
            }
        }
        return task.await()
    }
}

internal val featureReads = FeatureReadQueue(CoroutineScope(SupervisorJob() + Dispatchers.IO))
