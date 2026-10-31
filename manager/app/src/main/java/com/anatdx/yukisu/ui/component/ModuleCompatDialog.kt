package com.anatdx.yukisu.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.ModuleCompatConfig
import com.anatdx.yukisu.ui.util.getModuleCompatSettings
import com.anatdx.yukisu.ui.util.setModuleCompatSettings
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ModuleCompatDialog(onDismiss: () -> Unit) {
    var enabled by rememberSaveable { mutableStateOf(false) }
    var ksudCode by rememberSaveable { mutableStateOf("") }
    var kernelCode by rememberSaveable { mutableStateOf("") }
    var uapi by rememberSaveable { mutableStateOf("") }
    var ksudVersion by rememberSaveable { mutableStateOf("") }
    var initialized by rememberSaveable { mutableStateOf(false) }
    var actual by remember { mutableStateOf<ModuleCompatConfig?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        busy = true
        try {
            val settings = getModuleCompatSettings()
            actual = settings.actual
            if (!initialized) {
                enabled = settings.config.enabled
                ksudCode = settings.config.ksudVersionCode.takeIf { it != 0L }?.toString().orEmpty()
                kernelCode = settings.config.kernelVersionCode.takeIf { it != 0L }?.toString().orEmpty()
                uapi = settings.config.uapiVersion.takeIf { it != 0L }?.toString().orEmpty()
                ksudVersion = settings.config.ksudVersion
                initialized = true
            }
        } catch (e: Exception) {
            if (e is CancellationException) throw e
            error = e.message ?: e.javaClass.simpleName
        } finally {
            busy = false
        }
    }

    fun parseCode(value: String): Long? =
        if (value.isBlank()) 0 else value.toLongOrNull()?.takeIf { it in 0..0xffffffffL }
    val ksudValue = parseCode(ksudCode)
    val kernelValue = parseCode(kernelCode)
    val uapiValue = parseCode(uapi)
    val nameValid = ksudVersion.toByteArray(Charsets.UTF_8).size < 255 &&
        ksudVersion.none { it.code < 0x20 || it.code == 0x7f }
    val fieldShape = MaterialTheme.shapes.medium
    val buttonShapes = ButtonDefaults.shapes()

    YukiAlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        title = { Text(stringResource(R.string.module_compat_title)) },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    text = stringResource(R.string.module_compat_summary),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (busy) CircularProgressIndicator()
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                SwitchItem(
                    title = stringResource(R.string.module_compat_enable),
                    checked = enabled,
                    enabled = initialized && !busy,
                    onCheckedChange = { enabled = it },
                )
                OutlinedTextField(
                    value = ksudCode,
                    onValueChange = { ksudCode = it },
                    label = { Text(stringResource(R.string.module_compat_ksud_code)) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    enabled = initialized && !busy,
                    singleLine = true,
                    isError = ksudValue == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = kernelCode,
                    onValueChange = { kernelCode = it },
                    label = { Text(stringResource(R.string.module_compat_kernel_code)) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    enabled = initialized && !busy,
                    singleLine = true,
                    isError = kernelValue == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = uapi,
                    onValueChange = { uapi = it },
                    label = { Text(stringResource(R.string.module_compat_uapi)) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    enabled = initialized && !busy,
                    singleLine = true,
                    isError = uapiValue == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = ksudVersion,
                    onValueChange = { ksudVersion = it },
                    label = { Text(stringResource(R.string.module_compat_ksud_version)) },
                    modifier = Modifier.fillMaxWidth(),
                    shape = fieldShape,
                    enabled = initialized && !busy,
                    singleLine = true,
                    isError = !nameValid,
                )
                FilledTonalButton(
                    onClick = {
                        actual?.let {
                            ksudCode = it.ksudVersionCode.toString()
                            kernelCode = it.kernelVersionCode.toString()
                            uapi = it.uapiVersion.toString()
                            ksudVersion = it.ksudVersion
                        }
                    },
                    enabled = actual != null && !busy,
                    modifier = Modifier.fillMaxWidth(),
                    shapes = buttonShapes,
                ) { Text(stringResource(R.string.module_compat_actual)) }
            }
        },
        confirmButton = {
            Button(
                enabled = initialized && !busy && ksudValue != null && kernelValue != null && uapiValue != null && nameValid,
                shapes = buttonShapes,
                onClick = {
                    if (ksudValue != null && kernelValue != null && uapiValue != null) {
                        busy = true
                        error = null
                        scope.launch {
                            try {
                                setModuleCompatSettings(ModuleCompatConfig(enabled, ksudValue, kernelValue, uapiValue, ksudVersion))
                                onDismiss()
                            } catch (e: Exception) {
                                if (e is CancellationException) throw e
                                error = e.message ?: e.javaClass.simpleName
                            } finally {
                                busy = false
                            }
                        }
                    }
                },
            ) { Text(stringResource(android.R.string.ok)) }
        },
        dismissButton = {
            OutlinedButton(onClick = onDismiss, enabled = !busy, shapes = buttonShapes) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}
