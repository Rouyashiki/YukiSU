package org.rouyashiki.yukisu.ui.screen.ramdisk

/** A single decision shared by toolbar, local back and host leave protection. */
internal enum class RamdiskBackAction {
    Blocked, ConfirmText, ConfirmHex, CloseText, CloseHex, CloseElf, ClearSelection, StopSearch,
    ParentDirectory, FragmentSelector, ConfirmArchive, PrimeExit, Exit,
}

internal data class RamdiskBackContext(
    val busy: Boolean = false,
    val textOpen: Boolean = false,
    val textDirty: Boolean = false,
    val elfOpen: Boolean = false,
    val hexOpen: Boolean = false,
    val hexDirty: Boolean = false,
    val selecting: Boolean = false,
    val searching: Boolean = false,
    val hasParent: Boolean = false,
    val multipleFragments: Boolean = false,
    val archiveChanged: Boolean = false,
    val exitPrimed: Boolean = false,
) {
    fun action(leavingScreen: Boolean = false): RamdiskBackAction = when {
        busy -> RamdiskBackAction.Blocked
        textOpen && textDirty -> RamdiskBackAction.ConfirmText
        hexOpen && hexDirty -> RamdiskBackAction.ConfirmHex
        leavingScreen && archiveChanged -> RamdiskBackAction.ConfirmArchive
        leavingScreen -> RamdiskBackAction.Exit
        textOpen -> RamdiskBackAction.CloseText
        hexOpen -> RamdiskBackAction.CloseHex
        elfOpen -> RamdiskBackAction.CloseElf
        selecting -> RamdiskBackAction.ClearSelection
        searching -> RamdiskBackAction.StopSearch
        hasParent -> RamdiskBackAction.ParentDirectory
        multipleFragments -> RamdiskBackAction.FragmentSelector
        archiveChanged -> RamdiskBackAction.ConfirmArchive
        exitPrimed -> RamdiskBackAction.Exit
        else -> RamdiskBackAction.PrimeExit
    }
}
