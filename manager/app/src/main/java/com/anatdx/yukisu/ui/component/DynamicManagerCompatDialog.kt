package com.anatdx.yukisu.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.KsuCli

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun DynamicManagerCompatDialog(
    initial: KsuCli.DynamicManagerCompat,
    onDismiss: () -> Unit,
    onSave: (KsuCli.DynamicManagerCompat) -> Unit,
) {
    var version by rememberSaveable { mutableStateOf(initial.version.takeIf { it != 0L }?.toString().orEmpty()) }
    var uapi by rememberSaveable { mutableStateOf(initial.uapiVersion.takeIf { it != 0L }?.toString().orEmpty()) }
    var fullVersion by rememberSaveable { mutableStateOf(initial.versionFull) }
    fun parseVersion(text: String): Long? =
        if (text.isBlank()) 0 else text.toLongOrNull()?.takeIf { it in 0..0xffffffffL }
    val versionValue = parseVersion(version)
    val uapiValue = parseVersion(uapi)
    val validName = fullVersion.toByteArray(Charsets.UTF_8).size < 255 && '\u0000' !in fullVersion
    val fieldShape = MaterialTheme.shapes.medium
    val buttonShapes = ButtonDefaults.shapes()

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.dynamic_manager_compat)) },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    text = stringResource(R.string.dynamic_manager_compat_summary),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = version,
                    onValueChange = { version = it },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    label = { Text(stringResource(R.string.dynamic_manager_compat_version)) },
                    isError = versionValue == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = uapi,
                    onValueChange = { uapi = it },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    label = { Text(stringResource(R.string.dynamic_manager_compat_uapi)) },
                    isError = uapiValue == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = fullVersion,
                    onValueChange = { fullVersion = it },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    label = { Text(stringResource(R.string.dynamic_manager_compat_name)) },
                    isError = !validName,
                    singleLine = true,
                )
                FilledTonalButton(
                    onClick = {
                        version = Natives.version.toUInt().toString()
                        uapi = Natives.getUapiVersion().toUInt().toString()
                        fullVersion = Natives.getFullVersion()
                    },
                    modifier = Modifier.fillMaxWidth(),
                    shapes = buttonShapes,
                ) {
                    Text(stringResource(R.string.dynamic_manager_compat_reset))
                }
            }
        },
        confirmButton = {
            Button(
                shapes = buttonShapes,
                enabled = versionValue != null && uapiValue != null && validName,
                onClick = {
                    if (versionValue != null && uapiValue != null && validName)
                        onSave(KsuCli.DynamicManagerCompat(versionValue, uapiValue, fullVersion))
                },
            ) { Text(stringResource(android.R.string.ok)) }
        },
        dismissButton = {
            OutlinedButton(onClick = onDismiss, shapes = buttonShapes) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}
