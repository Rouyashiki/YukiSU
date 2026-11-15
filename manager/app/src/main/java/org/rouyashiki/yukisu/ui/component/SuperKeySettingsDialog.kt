package org.rouyashiki.yukisu.ui.component

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.superkey.SuperKeyHelper

@Composable
fun SuperKeySettingsDialog(onDismiss: () -> Unit, onKeyCleared: () -> Unit) {
    val context = LocalContext.current
    var skipStoreSuperKey by remember {
        mutableStateOf(SuperKeyHelper.shouldSkipStorage(context))
    }
    var autoAuthenticateSuperKey by remember {
        mutableStateOf(SuperKeyHelper.isAutoAuthenticationEnabled(context))
    }
    var hasSavedSuperKey by remember {
        mutableStateOf(SuperKeyHelper.hasSavedSuperKey(context))
    }
    var confirmingClear by rememberSaveable { mutableStateOf(false) }
    val clearKeyDialogTitle = stringResource(R.string.clear_super_key)
    val clearKeyDialogContent = stringResource(R.string.settings_clear_super_key_dialog)

    YukiAlertDialog(
        onDismissRequest = {
            if (confirmingClear) confirmingClear = false else onDismiss()
        },
        title = {
            Text(
                if (confirmingClear) clearKeyDialogTitle
                else stringResource(R.string.settings_superkey_management)
            )
        },
        text = {
            if (confirmingClear) {
                Text(clearKeyDialogContent)
            } else {
                Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                    SwitchItem(
                        title = stringResource(R.string.settings_donot_store_superkey),
                        summary = stringResource(R.string.settings_donot_store_superkey_summary),
                        checked = skipStoreSuperKey,
                        onCheckedChange = {
                            skipStoreSuperKey = it
                            SuperKeyHelper.setSkipStorage(context, it)
                            hasSavedSuperKey = SuperKeyHelper.hasSavedSuperKey(context)
                            autoAuthenticateSuperKey =
                                SuperKeyHelper.isAutoAuthenticationEnabled(context)
                        }
                    )
                    SwitchItem(
                        title = stringResource(R.string.settings_auto_authenticate_superkey),
                        summary = stringResource(R.string.settings_auto_authenticate_superkey_summary),
                        checked = autoAuthenticateSuperKey,
                        enabled = hasSavedSuperKey && !skipStoreSuperKey,
                        onCheckedChange = {
                            autoAuthenticateSuperKey = it
                            SuperKeyHelper.setAutoAuthenticationEnabled(context, it)
                        }
                    )
                    OutlinedButton(
                        modifier = Modifier.fillMaxWidth(),
                        onClick = { confirmingClear = true }
                    ) {
                        Text(clearKeyDialogTitle)
                    }
                }
            }
        },
        confirmButton = {
            if (confirmingClear) {
                TextButton(onClick = {
                    SuperKeyHelper.clearSavedSuperKey(context)
                    hasSavedSuperKey = false
                    autoAuthenticateSuperKey = false
                    confirmingClear = false
                    onKeyCleared()
                }) {
                    Text(stringResource(android.R.string.ok))
                }
            } else {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.close))
                }
            }
        },
        dismissButton = if (confirmingClear) {
            {
                TextButton(onClick = { confirmingClear = false }) {
                    Text(stringResource(android.R.string.cancel))
                }
            }
        } else null
    )
}
