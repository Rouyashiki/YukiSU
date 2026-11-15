package org.rouyashiki.yukisu.ui.util.module

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.*
import androidx.compose.runtime.*
import org.rouyashiki.yukisu.ui.util.SnackbarController
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.component.rememberLoadingDialog
import org.rouyashiki.yukisu.ui.viewmodel.SuperUserViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import kotlinx.coroutines.CancellationException
import java.text.SimpleDateFormat
import java.util.*

object ModuleModify {
    suspend fun backupAllowlist(
        context: Context,
        snackBarHost: SnackbarController,
        viewModel: SuperUserViewModel,
        uri: Uri
    ) {
        withContext(Dispatchers.IO) {
            try {
                val json = viewModel.exportAllowlist()
                context.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                    output.write(json.toByteArray(Charsets.UTF_8))
                } ?: throw IOException("Failed to open output uri")

                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_backup_success),
                        duration = SnackbarDuration.Long
                    )
                }

            } catch (e: Exception) {
                if (e is CancellationException) throw e
                Log.e("AllowlistBackup", context.getString(R.string.allowlist_backup_failed, ""), e)
                withContext(Dispatchers.Main) {
                    snackBarHost.showSnackbar(
                        context.getString(R.string.allowlist_backup_failed, e.message),
                        duration = SnackbarDuration.Long
                    )
                }
            }
        }
    }

    internal fun restoreError(context: Context, error: Exception): String = when (error) {
        is AllowlistRestore.Failure -> context.getString(
            if (error.rollbackFailed) R.string.allowlist_rollback_failed else R.string.allowlist_rollback_success
        )
        else -> error.message ?: error.javaClass.simpleName
    }

    @Composable
    fun rememberAllowlistBackupLauncher(
        context: Context,
        snackBarHost: SnackbarController,
        viewModel: SuperUserViewModel,
        scope: CoroutineScope = rememberCoroutineScope()
    ): ActivityResultLauncher<Intent> {
        val loadingDialog = rememberLoadingDialog()
        return rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                result.data?.data?.let { uri ->
                    scope.launch {
                        loadingDialog.withLoading { backupAllowlist(context, snackBarHost, viewModel, uri) }
                    }
                }
            }
        }
    }

    @Composable
    fun rememberAllowlistRestoreLauncher(
        context: Context,
        snackBarHost: SnackbarController,
        viewModel: SuperUserViewModel,
    ): ActivityResultLauncher<Intent> {
        var selectedUri by remember { mutableStateOf<Uri?>(null) }
        selectedUri?.let { uri ->
            AllowlistRestoreDialog(uri, viewModel, snackBarHost) { selectedUri = null }
        }
        return rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) selectedUri = result.data?.data
        }
    }

    fun createAllowlistBackupIntent(): Intent {
        return Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            putExtra(Intent.EXTRA_TITLE, "yukisu_allowlist_backup_$timestamp.json")
        }
    }

    fun createAllowlistRestoreIntent(): Intent {
        return Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
        }
    }
}
