package org.rouyashiki.yukisu.ui.screen.ramdisk

import kotlin.test.Test
import kotlin.test.assertEquals

class RamdiskBackPolicyTest {
    @Test
    fun busyWorkPrecedesDocumentAndArchiveConfirmation() {
        val state = RamdiskBackContext(busy = true, textOpen = true, textDirty = true, archiveChanged = true)
        assertEquals(RamdiskBackAction.Blocked, state.action())
        assertEquals(RamdiskBackAction.Blocked, state.action(leavingScreen = true))
    }

    @Test
    fun dirtyTextClosesBeforeArchiveConfirmationOnScreenExit() {
        val text = RamdiskBackContext(textOpen = true, textDirty = true, archiveChanged = true)
        assertEquals(RamdiskBackAction.ConfirmText, text.action(leavingScreen = true))
        assertEquals(RamdiskBackAction.ConfirmArchive, text.copy(textOpen = false).action(leavingScreen = true))
        assertEquals(RamdiskBackAction.Exit, RamdiskBackContext().action(leavingScreen = true))
    }

    @Test
    fun viewersReturnToTheBrowserWithoutDiscardingTheArchive() {
        assertEquals(RamdiskBackAction.CloseText, RamdiskBackContext(textOpen = true, archiveChanged = true).action())
        assertEquals(RamdiskBackAction.CloseElf, RamdiskBackContext(elfOpen = true, archiveChanged = true).action())
    }

    @Test
    fun browserModesAndParentPrecedeFragmentSelector() {
        val state = RamdiskBackContext(selecting = true, searching = true, hasParent = true, multipleFragments = true)
        assertEquals(RamdiskBackAction.ClearSelection, state.action())
        assertEquals(RamdiskBackAction.StopSearch, state.copy(selecting = false).action())
        assertEquals(RamdiskBackAction.ParentDirectory, state.copy(selecting = false, searching = false).action())
        assertEquals(RamdiskBackAction.FragmentSelector, RamdiskBackContext(multipleFragments = true, archiveChanged = true).action())
    }

    @Test
    fun cleanRootRequiresTwoReturnsButChangedArchiveAlwaysConfirms() {
        assertEquals(RamdiskBackAction.PrimeExit, RamdiskBackContext().action())
        assertEquals(RamdiskBackAction.Exit, RamdiskBackContext(exitPrimed = true).action())
        assertEquals(RamdiskBackAction.ConfirmArchive, RamdiskBackContext(exitPrimed = true, archiveChanged = true).action())
    }

    @Test
    fun hexChangesConfirmBeforeArchiveExitAndBusySaveBlocksNavigation() {
        val hex = RamdiskBackContext(hexOpen = true, hexDirty = true, archiveChanged = true)
        assertEquals(RamdiskBackAction.ConfirmHex, hex.action())
        assertEquals(RamdiskBackAction.ConfirmHex, hex.action(leavingScreen = true))
        assertEquals(RamdiskBackAction.Blocked, hex.copy(busy = true).action(leavingScreen = true))
        assertEquals(RamdiskBackAction.CloseHex, hex.copy(hexDirty = false).action())
        assertEquals(RamdiskBackAction.ConfirmArchive, hex.copy(hexOpen = false).action(leavingScreen = true))
    }
}
