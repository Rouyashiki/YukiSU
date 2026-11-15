package org.rouyashiki.yukisu.ui.component

import androidx.compose.runtime.Recomposer
import androidx.compose.runtime.mutableStateOf
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.yield
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class LoadingDialogHandleTest {
    @Test
    fun rejectedRestoreKeepsRecomposerAliveAndAllowsRetry() = runTest {
        Dispatchers.setMain(StandardTestDispatcher(testScheduler))
        val recomposer = Recomposer(coroutineContext + Job())
        val scope = CoroutineScope(recomposer.effectCoroutineContext + Job(recomposer.effectCoroutineContext[Job]))
        val visible = mutableStateOf(false)
        val handle = LoadingDialogHandleImpl(visible, scope)
        val invalidRules = IllegalStateException("Invalid SELinux rules")
        try {
            val failure = runCatching {
                handle.withLoading {
                    assertTrue(visible.value)
                    yield()
                    throw invalidRules
                }
            }.exceptionOrNull()
            assertTrue(recomposer.effectCoroutineContext[Job]!!.isActive, "Validation must not cancel the UI recomposer")
            assertIs<IllegalStateException>(failure)
            assertEquals(invalidRules.message, failure.message)
            assertFalse(visible.value)
            assertEquals(47, handle.withLoading { 47 })
            assertFalse(visible.value)
        } finally {
            scope.cancel()
            recomposer.cancel()
            Dispatchers.resetMain()
        }
    }

    @Test
    fun callerCancellationStopsWorkAndDismissesLoading() = runTest {
        Dispatchers.setMain(StandardTestDispatcher(testScheduler))
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())
        val visible = mutableStateOf(false)
        val handle = LoadingDialogHandleImpl(visible, scope)
        val started = CompletableDeferred<Unit>()
        var stopped = false
        try {
            val caller = launch {
                handle.withLoading {
                    started.complete(Unit)
                    try {
                        awaitCancellation()
                    } finally {
                        stopped = true
                    }
                }
            }
            started.await()
            assertTrue(visible.value)
            caller.cancel()
            caller.join()
            runCurrent()
            assertTrue(stopped, "Loading work must share the caller's cancellation")
            assertFalse(visible.value)
        } finally {
            scope.cancel()
            runCurrent()
            Dispatchers.resetMain()
        }
    }
}
