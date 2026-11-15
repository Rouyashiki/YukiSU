package org.rouyashiki.yukisu.ui.screen.partition

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.component.YukiAlertDialog
import org.rouyashiki.yukisu.ui.component.YukiSwitch
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun PartitionAvbDialog(partition: PartitionInfo, onDismiss: () -> Unit) {
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    var status by remember(partition.blockDevice) { mutableStateOf<PartitionAvbStatus?>(null) }
    var pendingChange by remember(partition.blockDevice) { mutableStateOf<AvbChange?>(null) }
    var busy by remember(partition.blockDevice) { mutableStateOf(true) }
    var error by remember(partition.blockDevice) { mutableStateOf<String?>(null) }
    var saved by remember(partition.blockDevice) { mutableStateOf(false) }
    var refresh by remember { mutableIntStateOf(0) }

    LaunchedEffect(partition.blockDevice, refresh) {
        busy = true
        status = null
        error = null
        saved = false
        try {
            status = PartitionManagerHelper.getAvbStatus(partition.blockDevice)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = failure.message ?: resources.getString(R.string.partition_unknown)
        } finally {
            busy = false
        }
    }

    val change = pendingChange
    val current = status
    val dismiss = {
        if (!busy) {
            if (pendingChange != null) pendingChange = null else onDismiss()
        }
    }
    YukiAlertDialog(
        onDismissRequest = dismiss,
        properties = DialogProperties(dismissOnBackPress = !busy, dismissOnClickOutside = !busy),
        title = { Text(stringResource(R.string.partition_avb_title)) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(partition.name, style = MaterialTheme.typography.titleMedium)
                Text(partition.blockDevice, style = MaterialTheme.typography.bodySmall)
                Text(
                    stringResource(R.string.partition_info_slot) + ": " +
                        (partitionSlotSuffix(partition.blockDevice) ?: "/")
                )
                if (busy) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                if (change != null) {
                    Text(
                        stringResource(
                            R.string.partition_avb_confirm,
                            stringResource(change.feature.labelResource()),
                            stringResource(
                                if (change.enabled) R.string.partition_avb_enabled else R.string.partition_avb_disabled
                            ),
                        )
                    )
                    Text(
                        stringResource(R.string.partition_avb_warning),
                        color = MaterialTheme.colorScheme.error,
                    )
                } else {
                    Text(stringResource(R.string.partition_avb_stored_state))
                    current?.let { state ->
                        val canEdit = !busy && state.bootloader != AvbBootloaderState.Locked
                        AvbFlagRow(
                            label = stringResource(R.string.partition_avb_verification),
                            checked = state.verificationEnabled,
                            enabled = canEdit,
                            onCheckedChange = { pendingChange = AvbChange(AvbFeature.Verification, it) },
                        )
                        AvbFlagRow(
                            label = stringResource(R.string.partition_avb_verity),
                            checked = state.verityEnabled,
                            enabled = canEdit,
                            onCheckedChange = { pendingChange = AvbChange(AvbFeature.Verity, it) },
                        )
                        if (state.bootloader == AvbBootloaderState.Locked) {
                            Text(stringResource(R.string.partition_avb_locked), color = MaterialTheme.colorScheme.error)
                        }
                        if (!state.verificationEnabled) {
                            Text(stringResource(R.string.partition_avb_verity_note), style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    if (saved) Text(stringResource(R.string.partition_avb_saved))
                    error?.let {
                        Text(
                            stringResource(R.string.partition_avb_failed, it),
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                    TextButton(
                        onClick = {
                            if (!busy) {
                                busy = true
                                refresh++
                            }
                        },
                        enabled = !busy,
                    ) {
                        Text(stringResource(R.string.refresh))
                    }
                }
            }
        },
        confirmButton = {
            if (change != null && current != null) {
                TextButton(
                    enabled = !busy,
                    onClick = apply@{
                        if (busy) return@apply
                        busy = true
                        pendingChange = null
                        saved = false
                        error = null
                        scope.launch {
                            try {
                                status = PartitionManagerHelper.setAvbState(partition.blockDevice, current, change)
                                saved = true
                            } catch (cancelled: CancellationException) {
                                throw cancelled
                            } catch (failure: Exception) {
                                status = null
                                error = failure.message ?: resources.getString(R.string.partition_unknown)
                            } finally {
                                busy = false
                            }
                        }
                    },
                ) {
                    Text(stringResource(R.string.partition_avb_apply))
                }
            } else {
                TextButton(onClick = dismiss, enabled = !busy) {
                    Text(stringResource(android.R.string.ok))
                }
            }
        },
        dismissButton = if (change != null) {
            { TextButton(onClick = dismiss, enabled = !busy) { Text(stringResource(android.R.string.cancel)) } }
        } else null,
    )
}

private fun AvbFeature.labelResource(): Int = when (this) {
    AvbFeature.Verification -> R.string.partition_avb_verification
    AvbFeature.Verity -> R.string.partition_avb_verity
}

@Composable
private fun AvbFlagRow(label: String, checked: Boolean, enabled: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().toggleable(
            value = checked,
            enabled = enabled,
            role = Role.Switch,
            onValueChange = onCheckedChange,
        ),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(label)
            Text(
                stringResource(if (checked) R.string.partition_avb_enabled else R.string.partition_avb_disabled),
                style = MaterialTheme.typography.bodySmall,
            )
        }
        YukiSwitch(checked = checked, enabled = enabled, onCheckedChange = null)
    }
}
