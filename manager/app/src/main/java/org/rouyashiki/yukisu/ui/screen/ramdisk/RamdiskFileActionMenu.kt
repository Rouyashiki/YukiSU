package org.rouyashiki.yukisu.ui.screen.ramdisk

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.EditNote
import androidx.compose.material.icons.filled.Fingerprint
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Share
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import com.anatdx.yukifb.backend.FileChecksumCapability
import com.anatdx.yukifb.backend.FileExternalOpenCapability
import com.anatdx.yukifb.backend.FileShareCapability
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import com.anatdx.yukifb.ui.FileActionItem
import com.anatdx.yukifb.ui.FileActionsDialog
import org.rouyashiki.yukisu.R

internal enum class RamdiskFileAction { TEXT, ELF, HEX, SHARE, EXTERNAL, CHECKSUMS }

internal fun ramdiskFileActions(entry: FileEntry, hexSupported: Boolean, checksums: FileChecksumCapability?, sharing: FileShareCapability?, external: FileExternalOpenCapability?): List<RamdiskFileAction> = buildList {
    if (entry.type != FileEntryType.REGULAR_FILE) return@buildList
    if ((entry.size ?: 0L) <= MAX_RAMDISK_TEXT_FILE_SIZE) add(RamdiskFileAction.TEXT)
    if (entry.mimeType == ELF_MIME_TYPE) add(RamdiskFileAction.ELF)
    if (hexSupported) add(RamdiskFileAction.HEX)
    if (sharing?.canShare(entry) == true) add(RamdiskFileAction.SHARE)
    if (external?.canOpenExternally(entry) == true) add(RamdiskFileAction.EXTERNAL)
    if (!checksums?.algorithms(entry).isNullOrEmpty()) add(RamdiskFileAction.CHECKSUMS)
}

@Composable
internal fun RamdiskFileActionsDialog(entry: FileEntry, actions: List<RamdiskFileAction>, onAction: (RamdiskFileAction) -> Unit, onDismiss: () -> Unit) {
    val items = actions.map { action ->
        val (label, icon) = when (action) {
            RamdiskFileAction.TEXT -> R.string.ramdisk_editor_open_text to Icons.Default.EditNote
            RamdiskFileAction.ELF -> R.string.ramdisk_editor_elf_summary_title to Icons.Default.Memory
            RamdiskFileAction.HEX -> com.anatdx.yukifb.hex.R.string.yukifb_hex_title to Icons.Default.Code
            RamdiskFileAction.SHARE -> com.anatdx.yukifb.R.string.yukifb_action_share to Icons.Default.Share
            RamdiskFileAction.EXTERNAL -> com.anatdx.yukifb.R.string.yukifb_action_open_externally to Icons.AutoMirrored.Filled.OpenInNew
            RamdiskFileAction.CHECKSUMS -> com.anatdx.yukifb.R.string.yukifb_action_checksums to Icons.Default.Fingerprint
        }
        FileActionItem(action.name, stringResource(label), icon)
    }
    FileActionsDialog(entry, items, { id -> onAction(RamdiskFileAction.valueOf(id)) }, onDismiss)
}
