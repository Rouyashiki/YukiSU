package com.anatdx.yukisu.ui.viewmodel

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withContext
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@OptIn(ExperimentalCoroutinesApi::class)
class ModuleUpdateStateTest {
    @Test
    fun checksTheWholeInventoryWithBoundedConcurrencyAndPublishesEachResult() = runTest {
        val modules = (1..9).map { module("module-$it") }
        val gates = modules.associate { it.dirId to CompletableDeferred<Unit>() }
        val started = mutableListOf<String>()
        val state = ModuleUpdateState(this, StandardTestDispatcher(testScheduler)) {
            started += it.dirId
            gates.getValue(it.dirId).await()
            update(it.dirId)
        }

        state.refresh(modules)
        runCurrent()
        assertEquals(4, started.size)
        assertEquals(0, state.availableCount)

        gates.getValue("module-2").complete(Unit)
        runCurrent()
        assertEquals(5, started.size)
        assertEquals(1, state.availableCount)
        assertEquals(update("module-2"), state.updateInfo["module-2"])

        gates.values.forEach { it.complete(Unit) }
        advanceUntilIdle()
        assertEquals(modules.map { it.dirId }.toSet(), started.toSet())
        assertEquals(9, state.availableCount)
    }

    @Test
    fun reusesUnchangedResultsAndInvalidatesChangedOrIneligibleModules() = runTest {
        val checked = mutableListOf<String>()
        val first = module("first")
        val second = module("second").copy(id = first.id)
        val state = ModuleUpdateState(this, StandardTestDispatcher(testScheduler)) {
            checked += it.dirId
            update(it.dirId)
        }

        state.refresh(listOf(first, second))
        advanceUntilIdle()
        state.refresh(listOf(second, first))
        advanceUntilIdle()
        assertEquals(2, checked.size)
        assertEquals(2, state.availableCount)

        state.refresh(listOf(first.copy(versionCode = 2), second.copy(enabled = false)))
        assertTrue(state.updateInfo.isEmpty())
        advanceUntilIdle()
        assertEquals(listOf("first", "second", "first"), checked)
        assertEquals(setOf("first"), state.updateInfo.keys)

        state.refresh(listOf(first.copy(update = true), second.copy(remove = true), module("no-url").copy(updateJson = "")))
        advanceUntilIdle()
        assertEquals(3, checked.size)
        assertEquals(0, state.availableCount)

        state.refresh(listOf(first))
        advanceUntilIdle()
        state.refresh(listOf(first.copy(updateJson = "https://example.com/new.json")))
        assertTrue(state.updateInfo.isEmpty())
        advanceUntilIdle()
        assertEquals(5, checked.size)
        state.refresh(emptyList())
        assertEquals(0, state.availableCount)
    }

    @Test
    fun aFailedCheckDoesNotStopOtherModulesAndManualRefreshRetriesIt() = runTest {
        var attempts = 0
        val state = ModuleUpdateState(this, StandardTestDispatcher(testScheduler)) {
            if (it.dirId == "retry" && ++attempts == 1) error("Offline")
            update(it.dirId)
        }
        val modules = listOf(module("retry"), module("working"))

        state.refresh(modules)
        advanceUntilIdle()
        assertEquals(1, state.availableCount)
        state.refresh(modules, force = true)
        advanceUntilIdle()
        assertEquals(2, attempts)
        assertEquals(2, state.availableCount)
    }

    @Test
    fun lateResponsesCannotRestoreRemovedModulesOrOverwriteNewVersions() = runTest {
        val oldResponse = CompletableDeferred<Unit>()
        val state = ModuleUpdateState(this, StandardTestDispatcher(testScheduler)) {
            if (it.versionCode == 1) {
                withContext(NonCancellable) { oldResponse.await() }
            }
            update("${it.dirId}-${it.versionCode}")
        }
        val kept = module("kept")
        state.refresh(listOf(kept, module("removed")))
        runCurrent()

        state.refresh(listOf(kept.copy(versionCode = 2)))
        runCurrent()
        assertEquals(update("kept-2"), state.updateInfo["kept"])
        oldResponse.complete(Unit)
        advanceUntilIdle()
        assertFalse("removed" in state.updateInfo)
        assertEquals(update("kept-2"), state.updateInfo["kept"])
        assertEquals(1, state.availableCount)
    }

    private fun update(name: String) = ModuleUpdateInfo(zipUrl = "https://example.com/$name.zip")

    private fun module(dirId: String) = ModuleViewModel.ModuleInfo(
        id = dirId, name = dirId, author = "", version = "1", versionCode = 1,
        description = "", enabled = true, update = false, remove = false,
        updateJson = "https://example.com/$dirId.json", hasWebUi = false,
        hasActionScript = false, metamodule = false, dirId = dirId,
    )
}
