package org.rouyashiki.yukisu.ui.util.module

import android.net.Uri
import android.os.SystemClock
import android.util.Log
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.component.YukiAlertDialog
import org.rouyashiki.yukisu.ui.util.SnackbarController
import org.rouyashiki.yukisu.ui.viewmodel.SuperUserViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun AllowlistRestoreDialog(
    uri: Uri,
    viewModel: SuperUserViewModel,
    snackbar: SnackbarController,
    onDismiss: () -> Unit
) {
    val androidContext = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    var prepared by remember(uri) { mutableStateOf<AllowlistPlan.Context?>(null) }
    var options by remember(uri) { mutableStateOf(AllowlistPlan.Options()) }
    var plan by remember(uri) { mutableStateOf<AllowlistPlan.Plan?>(null) }
    var loading by remember(uri) { mutableStateOf(true) }
    var applying by remember(uri) { mutableStateOf(false) }
    var error by remember(uri) { mutableStateOf<String?>(null) }
    var notice by remember(uri) { mutableStateOf<String?>(null) }
    var candidateSource by remember(uri) { mutableStateOf<AllowlistBackup.Entry?>(null) }
    var candidates by remember(uri) { mutableStateOf<List<BackupPackage>?>(null) }
    val ready = !loading && !applying && plan?.context === prepared && plan?.options == options

    LaunchedEffect(uri) {
        try {
            val document = withContext(Dispatchers.IO) {
                androidContext.contentResolver.openInputStream(uri)?.use(AllowlistBackup::read)
                    ?: error("Cannot open backup")
            }
            prepared = viewModel.prepareAllowlist(document)
        } catch (failure: Exception) {
            if (failure is CancellationException) throw failure
            Log.e("AllowlistRestore", "Cannot prepare backup", failure)
            error = ModuleModify.restoreError(androidContext, failure)
        } finally {
            loading = false
        }
    }
    LaunchedEffect(prepared, options) {
        val data = prepared ?: return@LaunchedEffect
        try {
            val started = SystemClock.elapsedRealtime()
            plan = withContext(Dispatchers.Default) { AllowlistPlan.create(data, options) }
            Log.i("AllowlistRestore", "Plan: ${data.document.apps.size} entries, ${SystemClock.elapsedRealtime() - started} ms")
        } catch (failure: Exception) {
            if (failure is CancellationException) throw failure
            error = ModuleModify.restoreError(androidContext, failure)
            plan = null
        }
    }
    LaunchedEffect(candidateSource, prepared) {
        candidates = null
        val source = candidateSource ?: return@LaunchedEffect
        val data = prepared ?: return@LaunchedEffect
        candidates = withContext(Dispatchers.Default) { data.installed.candidates(source, data.protected) }
    }

    YukiAlertDialog(
        onDismissRequest = { if (!applying) onDismiss() },
        properties = DialogProperties(dismissOnBackPress = !applying, dismissOnClickOutside = !applying),
        title = { Text(stringResource(R.string.allowlist_restore_confirm_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (loading || applying || prepared != null && !ready) LinearProgressIndicator(Modifier.fillMaxWidth())
                if (applying) Text(stringResource(R.string.allowlist_applying))
                error?.let { Text(stringResource(R.string.allowlist_restore_failed, it), color = MaterialTheme.colorScheme.error) }
                notice?.let { Text(it) }
                val current = plan
                if (current != null) {
                    Text(stringResource(
                        R.string.allowlist_preview_counts, current.selectedCount, current.migratedCount,
                        current.pendingCount, current.rows.count { !it.selected } - current.pendingCount, current.resetUids.size
                    ))
                    if (current.context.document.version == 1) Text(stringResource(R.string.allowlist_legacy_identity))
                    Row {
                        Checkbox(
                            checked = options.replace,
                            enabled = ready && (options.replace || current.replaceAvailable),
                            onCheckedChange = { options = options.copy(replace = it, restoreDefault = it) }
                        )
                        Text(stringResource(R.string.allowlist_replace_all), modifier = Modifier.weight(1f))
                    }
                    if (!current.replaceAvailable) Text(stringResource(R.string.allowlist_replace_unavailable))
                    Row {
                        Checkbox(
                            checked = options.restoreDefault, enabled = ready,
                            onCheckedChange = { options = options.copy(restoreDefault = it) }
                        )
                        val old = current.context.before[AllowlistRestore.DEFAULT_UID]?.umountModules ?: false
                        Text(stringResource(
                            R.string.allowlist_restore_default,
                            stringResource(if (old) R.string.allowlist_value_on else R.string.allowlist_value_off),
                            stringResource(if (current.context.document.defaultUmountModules) R.string.allowlist_value_on else R.string.allowlist_value_off)
                        ), modifier = Modifier.weight(1f))
                    }
                    LazyColumn(Modifier.heightIn(max = 320.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        items(current.rows, key = { "source:${it.source.profile.currentUid}" }) { row ->
                            Column {
                                Row {
                                    Checkbox(
                                        checked = row.requested && row.target != null, enabled = ready && row.target != null,
                                        onCheckedChange = { checked ->
                                            val uid = row.source.profile.currentUid
                                            options = options.copy(excluded = if (checked) options.excluded - uid else options.excluded + uid)
                                        }
                                    )
                                    Column(Modifier.weight(1f)) {
                                        Text(row.source.packages.joinToString())
                                        Text(stringResource(statusText(row.status)), style = MaterialTheme.typography.bodySmall)
                                        Text(stringResource(
                                            R.string.allowlist_uid_mapping, row.source.profile.currentUid,
                                            row.target?.currentUid?.toString() ?: "—"
                                        ), style = MaterialTheme.typography.bodySmall)
                                        if (row.status == AllowlistPlan.Status.MAPPED) Text(row.target!!.name)
                                    }
                                }
                                if (current.context.document.version == 2 && row.source.packages.size == 1 &&
                                    row.status in setOf(AllowlistPlan.Status.MISSING, AllowlistPlan.Status.MAPPED, AllowlistPlan.Status.DUPLICATE)) {
                                    TextButton(enabled = ready, onClick = {
                                        scope.launch {
                                            val data = prepared ?: return@launch
                                            val user = row.source.profile.currentUid / 100000
                                            loading = true
                                            try {
                                                if (user !in data.candidateUsers) {
                                                    prepared = viewModel.prepareAllowlist(data.document, data.candidateUsers + user)
                                                    options = options.copy(mappings = emptyMap(), replace = false)
                                                }
                                                candidateSource = row.source
                                            } catch (failure: Exception) {
                                                if (failure is CancellationException) throw failure
                                                error = ModuleModify.restoreError(androidContext, failure)
                                            } finally {
                                                loading = false
                                            }
                                        }
                                    }) { Text(stringResource(R.string.allowlist_choose_candidate)) }
                                }
                            }
                        }
                        items(current.resetUids.sorted(), key = { "reset:$it" }) { uid ->
                            Text(stringResource(R.string.allowlist_reset_item, current.context.before.getValue(uid).name, uid))
                        }
                        if (current.rules.isNotEmpty()) item { Text(stringResource(R.string.allowlist_global_rules)) }
                        items(current.rules.keys.sorted(), key = { "rules:$it" }) { key ->
                            Text(stringResource(
                                if (current.rules.getValue(key).isEmpty()) R.string.allowlist_clear_rules else R.string.allowlist_write_rules, key
                            ))
                        }
                    }
                    if (ready && !current.canApply && (!options.replace || current.replaceAvailable)) Text(stringResource(R.string.allowlist_no_changes))
                }
            }
        },
        confirmButton = {
            TextButton(enabled = ready && plan?.canApply == true, onClick = {
                val confirmed = plan ?: return@TextButton
                scope.launch {
                    applying = true
                    error = null
                    notice = null
                    try {
                        val fresh = viewModel.restoreAllowlist(confirmed)
                        if (fresh != null) {
                            prepared = fresh
                            options = options.copy(mappings = emptyMap(), replace = false)
                            notice = resources.getString(R.string.allowlist_preview_changed)
                        } else {
                            applying = false
                            onDismiss()
                            snackbar.showSnackbar(resources.getString(R.string.allowlist_restore_success) + "\n" + resources.getString(
                                R.string.allowlist_restore_result, confirmed.selectedCount,
                                confirmed.rows.count { !it.selected }, confirmed.resetUids.size
                            ), duration = SnackbarDuration.Long)
                        }
                    } catch (failure: Exception) {
                        if (failure is CancellationException) throw failure
                        Log.e("AllowlistRestore", "Restore failed", failure)
                        error = ModuleModify.restoreError(androidContext, failure)
                        plan = null
                        prepared = null
                    } finally {
                        applying = false
                    }
                }
            }) { Text(stringResource(R.string.confirm)) }
        },
        dismissButton = { TextButton(enabled = !applying, onClick = onDismiss) { Text(stringResource(R.string.cancel)) } }
    )

    candidateSource?.let { source ->
        YukiAlertDialog(
            onDismissRequest = { candidateSource = null },
            title = { Text(stringResource(R.string.allowlist_choose_candidate)) },
            text = {
                Column {
                    Text(source.profile.name)
                    Text(stringResource(R.string.allowlist_candidate_warning))
                    val choices = candidates
                    if (choices == null) LinearProgressIndicator(Modifier.fillMaxWidth())
                    else if (choices.isEmpty()) Text(stringResource(R.string.allowlist_no_candidates))
                    else LazyColumn(Modifier.heightIn(max = 320.dp)) {
                        items(choices, key = { it.uid }) { app ->
                            TextButton(onClick = {
                                options = options.copy(
                                    mappings = options.mappings + (source.profile.currentUid to app.uid),
                                    excluded = options.excluded - source.profile.currentUid
                                )
                                candidateSource = null
                            }) {
                                Column {
                                    Text(app.label)
                                    Text(app.packageName)
                                    Text(stringResource(R.string.allowlist_candidate_identity, app.userId, app.uid))
                                }
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { candidateSource = null }) { Text(stringResource(R.string.cancel)) } }
        )
    }
}

private fun statusText(status: AllowlistPlan.Status): Int = when (status) {
    AllowlistPlan.Status.MATCHED -> R.string.allowlist_match_ready
    AllowlistPlan.Status.MAPPED -> R.string.allowlist_match_mapped
    AllowlistPlan.Status.MISSING -> R.string.allowlist_match_missing
    AllowlistPlan.Status.SHARED_UID -> R.string.allowlist_match_shared
    AllowlistPlan.Status.SIGNATURE -> R.string.allowlist_match_signature
    AllowlistPlan.Status.UNREADABLE -> R.string.allowlist_match_unreadable
    AllowlistPlan.Status.PROTECTED -> R.string.allowlist_match_protected
    AllowlistPlan.Status.DUPLICATE -> R.string.allowlist_match_duplicate
    AllowlistPlan.Status.RULE_CONFLICT -> R.string.allowlist_match_rules
}
