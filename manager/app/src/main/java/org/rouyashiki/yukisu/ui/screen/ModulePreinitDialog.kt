package org.rouyashiki.yukisu.ui.screen

import android.util.Log
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.component.YukiAlertDialog
import org.rouyashiki.yukisu.ui.util.ModulePreinitStatus
import org.rouyashiki.yukisu.ui.util.readModulePreinitStatus
import org.rouyashiki.yukisu.ui.util.resetModulePreinitBootCount
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun ModulePreinitDialog(onDismiss: () -> Unit, onReset: () -> Unit) {
    val scope = rememberCoroutineScope()
    val operationFailed = stringResource(R.string.operation_failed)
    var status by remember { mutableStateOf<ModulePreinitStatus?>(null) }
    var busy by remember { mutableStateOf(true) }
    var errorMessage by remember { mutableStateOf<String?>(null) }
    var resetDone by remember { mutableStateOf(false) }
    var confirmReset by remember { mutableStateOf(false) }
    suspend fun refresh() {
        busy = true
        errorMessage = null
        try {
            status = readModulePreinitStatus()
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            Log.e("ModulePreinit", "Failed to read preinit status", error)
            status = null
            errorMessage = error.message?.takeIf { it.isNotBlank() } ?: operationFailed
        } finally {
            busy = false
        }
    }
    LaunchedEffect(Unit) { refresh() }
    YukiAlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        title = { Text(stringResource(R.string.module_preinit_title)) },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                errorMessage?.let { message ->
                    SelectionContainer {
                        Text(message, color = MaterialTheme.colorScheme.error)
                    }
                }
                if (resetDone) Text(stringResource(R.string.module_preinit_reset_done))
                status?.let { current ->
                    Text(stringResource(
                        when {
                            !current.supported -> R.string.module_preinit_unsupported
                            current.bootloop -> R.string.module_preinit_bootloop
                            else -> R.string.module_preinit_ready
                        },
                    ))
                    Text(stringResource(R.string.module_preinit_boot_count, current.bootCount.coerceAtMost(3)))
                    if (current.modules.isEmpty()) Text(stringResource(R.string.module_preinit_empty))
                    current.modules.forEach { (id, state) ->
                        Column {
                            Text(id, style = MaterialTheme.typography.titleSmall)
                            Text(stringResource(when (state) {
                                "ok" -> R.string.module_preinit_staged
                                "bootloop" -> R.string.module_preinit_bootloop
                                "no_space" -> R.string.module_preinit_no_space
                                "unsupported" -> R.string.module_preinit_unsupported
                                else -> R.string.module_preinit_error
                            }), style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    Text(stringResource(R.string.module_preinit_manage_hint), style = MaterialTheme.typography.bodySmall)
                    OutlinedButton(
                        onClick = { confirmReset = true },
                        enabled = !busy && current.bootCount > 0,
                    ) { Text(stringResource(R.string.module_preinit_reset)) }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss, enabled = !busy) { Text(stringResource(android.R.string.ok)) }
        },
        dismissButton = {
            TextButton(onClick = { scope.launch { refresh() } }, enabled = !busy) {
                Text(stringResource(R.string.refresh))
            }
        },
    )
    if (confirmReset) {
        YukiAlertDialog(
            onDismissRequest = { confirmReset = false },
            title = { Text(stringResource(R.string.module_preinit_reset)) },
            text = { Text(stringResource(R.string.module_preinit_reset_confirm)) },
            confirmButton = {
                TextButton(onClick = {
                    confirmReset = false
                    busy = true
                    errorMessage = null
                    resetDone = false
                    scope.launch {
                        try {
                            resetModulePreinitBootCount()
                            resetDone = true
                            onReset()
                            refresh()
                        } catch (error: Exception) {
                            if (error is CancellationException) throw error
                            Log.e("ModulePreinit", "Failed to reset preinit boot counter", error)
                            errorMessage = error.message?.takeIf { it.isNotBlank() } ?: operationFailed
                        } finally {
                            busy = false
                        }
                    }
                }) { Text(stringResource(android.R.string.ok)) }
            },
            dismissButton = {
                TextButton(onClick = { confirmReset = false }) { Text(stringResource(android.R.string.cancel)) }
            },
        )
    }
}
