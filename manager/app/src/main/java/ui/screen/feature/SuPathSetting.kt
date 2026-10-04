package ui.screen.feature

import android.util.Log
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.screen.SettingItem
import com.anatdx.yukisu.ui.util.getKsud
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

internal const val DEFAULT_SU_PATH = "/system/bin/su"

internal fun isValidSuPath(path: String): Boolean =
    path.startsWith('/') && path.toByteArray(Charsets.UTF_8).size < 4096 &&
        path.none { it.code < 0x20 } &&
        path.substring(1).split('/').all {
            it.isNotEmpty() && it != "." && it != ".." &&
                it.toByteArray(Charsets.UTF_8).size <= 255
        }

@StringRes
internal fun suPathErrorResource(code: String): Int = when (code) {
    "path_exists" -> R.string.su_compat_path_error_exists
    "parent_unavailable" -> R.string.su_compat_path_error_parent
    "invalid_path", "path_too_long" -> R.string.su_compat_path_invalid
    "permission_denied" -> R.string.su_compat_path_error_permission
    "unsupported" -> R.string.feature_status_unsupported_summary
    "persistence_failed" -> R.string.su_compat_path_error_persistence
    else -> R.string.setting_change_failed
}

@Composable
internal fun SuPathSetting(
    enabled: Boolean,
    onSavingChange: (Boolean) -> Unit,
) {
    val scope = rememberCoroutineScope()
    var path by remember { mutableStateOf<String?>(null) }
    var input by remember { mutableStateOf("") }
    var showDialog by remember { mutableStateOf(false) }
    var saving by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<Int?>(null) }
    val title = stringResource(R.string.su_compat_path_title)

    LaunchedEffect(Unit) {
        path = withContext(Dispatchers.IO) {
            runCatching { Natives.getSuPath()?.toString(Charsets.UTF_8) }.getOrNull()
        }
    }
    val current = path ?: return

    SettingItem(
        icon = Icons.Rounded.Folder,
        title = title,
        summary = current,
        enabled = enabled && !saving,
        onClick = {
            input = current
            error = null
            showDialog = true
        },
    )

    if (showDialog) {
        val valid = isValidSuPath(input)
        YukiAlertDialog(
            onDismissRequest = { if (!saving) showDialog = false },
            title = { Text(title) },
            text = {
                Column {
                    Text(stringResource(R.string.su_compat_path_help))
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = input,
                        onValueChange = { input = it; error = null },
                        modifier = Modifier.fillMaxWidth(),
                        enabled = !saving,
                        singleLine = true,
                        label = { Text(title) },
                        isError = !valid || error != null,
                        supportingText = {
                            if (!valid) Text(stringResource(R.string.su_compat_path_invalid))
                        },
                    )
                    error?.let { message ->
                        Text(
                            stringResource(message),
                            color = MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    TextButton(
                        enabled = !saving,
                        onClick = { input = DEFAULT_SU_PATH; error = null },
                    ) {
                        Text(stringResource(R.string.su_compat_path_reset))
                    }
                }
            },
            dismissButton = {
                TextButton(enabled = !saving, onClick = { showDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
            confirmButton = {
                TextButton(
                    enabled = enabled && valid && !saving,
                    onClick = {
                        val requested = input
                        saving = true
                        onSavingChange(true)
                        scope.launch(start = CoroutineStart.UNDISPATCHED) {
                            withContext(NonCancellable) {
                                try {
                                    val result = withContext(Dispatchers.IO) {
                                        runCatching {
                                            val response = Natives.saveSuPath(
                                                getKsud().toByteArray(Charsets.UTF_8),
                                                requested.toByteArray(Charsets.UTF_8),
                                            )
                                            val reply = JSONObject(response.toString(Charsets.UTF_8))
                                            check(reply.getInt("version") == 1)
                                            val code = reply.getString("error")
                                            if (code == "none") {
                                                check(Natives.getSuPath()?.toString(Charsets.UTF_8) == requested)
                                                null
                                            } else {
                                                Log.w("SuPathSetting", "su path error: $code (${reply.optInt("errno")})")
                                                suPathErrorResource(code)
                                            }
                                        }
                                    }
                                    path = withContext(Dispatchers.IO) {
                                        runCatching { Natives.getSuPath()?.toString(Charsets.UTF_8) }
                                            .getOrNull()
                                    } ?: path
                                    error = result.getOrElse {
                                        Log.w("SuPathSetting", "Failed to save su path", it)
                                        R.string.setting_change_failed
                                    }
                                    if (error == null) {
                                        showDialog = false
                                    }
                                } finally {
                                    saving = false
                                    onSavingChange(false)
                                }
                            }
                        }
                    },
                ) {
                    if (saving) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    else Text(stringResource(R.string.app_profile_template_save))
                }
            },
        )
    }
}
