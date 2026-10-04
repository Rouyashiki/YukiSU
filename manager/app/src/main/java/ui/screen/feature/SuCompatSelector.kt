package ui.screen.feature

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.RemoveModerator
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.screen.SettingItem

@Composable
internal fun SuCompatSelector(
    selected: SuCompatMode,
    enabled: Boolean,
    ksmSupported: Boolean,
    onSelect: (SuCompatMode) -> Unit,
) {
    var showDialog by remember { mutableStateOf(false) }
    val labels = listOf(
        stringResource(R.string.su_compat_off),
        stringResource(R.string.su_compat_traditional),
        stringResource(R.string.su_compat_ksm),
    )
    val title = stringResource(R.string.su_compat_title)

    SettingItem(
        icon = Icons.Rounded.RemoveModerator,
        title = title,
        summary = labels[selected.ordinal],
        enabled = enabled,
        onClick = { showDialog = true },
    )

    if (showDialog && enabled) {
        YukiAlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(title) },
            text = {
                Column(Modifier.selectableGroup()) {
                    SuCompatMode.entries.filter { it != SuCompatMode.KSM || ksmSupported }.forEach { mode ->
                        Row(
                            modifier = Modifier.fillMaxWidth()
                                .selectable(
                                    selected = selected == mode,
                                    role = Role.RadioButton,
                                    onClick = {
                                        showDialog = false
                                        onSelect(mode)
                                    },
                                )
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(
                                selected = selected == mode,
                                onClick = null,
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(labels[mode.ordinal])
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }
}
