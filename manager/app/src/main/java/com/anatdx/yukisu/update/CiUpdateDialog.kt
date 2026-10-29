package com.anatdx.yukisu.update

import android.content.Context
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiDownloadProgressIndicator
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.screen.WarningCard
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

private enum class CiUpdateStage {
    READY,
    DOWNLOADING,
    VERIFYING,
    INSTALLING,
    FAILED,
}

internal sealed interface CiUpdateCheckState {
    data object Hidden : CiUpdateCheckState
    data object Checking : CiUpdateCheckState
    data object Current : CiUpdateCheckState
    data object Failed : CiUpdateCheckState
    data class Available(val run: CiRun) : CiUpdateCheckState
}

@Composable
fun CiUpdateCard() {
    val context = LocalContext.current
    val resources = LocalResources.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    var checkRequest by remember { mutableIntStateOf(0) }
    var checkState by remember { mutableStateOf<CiUpdateCheckState>(CiUpdateCheckState.Hidden) }
    var showDialog by remember { mutableStateOf(false) }
    var stage by remember { mutableStateOf(CiUpdateStage.READY) }
    var progress by remember { mutableIntStateOf(0) }
    var error by remember { mutableStateOf("") }
    var pendingSystemInstallerApk by remember { mutableStateOf<File?>(null) }
    val installPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        val apk = pendingSystemInstallerApk ?: return@rememberLauncherForActivityResult
        pendingSystemInstallerApk = null
        if (!CiUpdateManager.canRequestPackageInstalls(context)) {
            error = resources.getString(R.string.ci_update_install_permission_denied)
            stage = CiUpdateStage.FAILED
            return@rememberLauncherForActivityResult
        }
        try {
            CiUpdateManager.startSystemInstaller(context, apk)
            checkState = CiUpdateCheckState.Current
            showDialog = false
        } catch (throwable: Exception) {
            error = throwable.message ?: throwable.javaClass.simpleName
            stage = CiUpdateStage.FAILED
        }
    }

    DisposableEffect(lifecycleOwner) {
        var started = lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> {
                    if (!started) {
                        started = true
                        if (
                            pendingSystemInstallerApk == null &&
                            checkState != CiUpdateCheckState.Checking
                        ) {
                            checkRequest++
                        }
                    }
                }
                Lifecycle.Event.ON_STOP -> started = false
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (started) checkRequest++
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    LaunchedEffect(checkRequest) {
        if (checkRequest == 0) return@LaunchedEffect
        val settings = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val updateChecksEnabled = settings.getBoolean("check_update", true)
        val ciUpdateChecksEnabled = settings.getBoolean("check_ci_update", false)
        if (!updateChecksEnabled || !ciUpdateChecksEnabled) {
            checkState = CiUpdateCheckState.Hidden
            showDialog = false
            return@LaunchedEffect
        }

        val forceCheck = checkState == CiUpdateCheckState.Failed
        checkState = CiUpdateCheckState.Checking
        val latestRun = try {
            CiUpdateManager.latestSuccessfulMainRun(force = forceCheck)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (throwable: Exception) {
            Log.w("CiUpdate", "Failed to check CI update", throwable)
            checkState = CiUpdateCheckState.Failed
            return@LaunchedEffect
        }
        checkState = if (latestRun != null && latestRun.runId > BuildConfig.CI_RUN_ID) {
            CiUpdateCheckState.Available(latestRun)
        } else {
            CiUpdateCheckState.Current
        }
    }

    val currentCheckState = checkState
    val availableRun = (currentCheckState as? CiUpdateCheckState.Available)?.run
    CiUpdateNotice(
        state = currentCheckState,
        onRetry = { checkRequest++ },
        onUpdate = {
            error = ""
            progress = 0
            stage = CiUpdateStage.READY
            showDialog = true
        },
    )

    if (!showDialog || availableRun == null) return

    val busy = stage == CiUpdateStage.DOWNLOADING ||
        stage == CiUpdateStage.VERIFYING ||
        stage == CiUpdateStage.INSTALLING

    fun beginUpdate() {
        scope.launch {
            try {
                error = ""
                progress = 0
                stage = CiUpdateStage.DOWNLOADING
                val prepared = CiUpdateManager.downloadAndExtract { downloaded ->
                    progress = downloaded
                }
                stage = CiUpdateStage.VERIFYING
                withContext(Dispatchers.IO) {
                    CiUpdateManager.verify(context, availableRun, prepared)
                }
                stage = CiUpdateStage.INSTALLING
                when (val result = CiUpdateManager.install(context, availableRun, prepared)) {
                    CiInstallResult.RootInstalled -> Toast.makeText(
                        context,
                        R.string.ci_update_root_installed,
                        Toast.LENGTH_LONG,
                    ).show()
                    CiInstallResult.SystemInstallerStarted -> Unit
                    is CiInstallResult.SystemInstallerPermissionRequired -> {
                        pendingSystemInstallerApk = result.apk
                        installPermissionLauncher.launch(
                            CiUpdateManager.unknownSourcesPermissionIntent(context)
                        )
                        return@launch
                    }
                }
                checkState = CiUpdateCheckState.Current
                showDialog = false
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (throwable: Exception) {
                error = throwable.message ?: throwable.javaClass.simpleName
                stage = CiUpdateStage.FAILED
            }
        }
    }

    YukiAlertDialog(
        onDismissRequest = { if (!busy) showDialog = false },
        title = { Text(stringResource(R.string.ci_update_title)) },
        text = {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(
                    stringResource(
                        R.string.ci_update_message,
                        availableRun.versionCode,
                        BuildConfig.VERSION_CODE,
                    )
                )
                if (availableRun.commitSha.isNotBlank()) {
                    Spacer(Modifier.height(16.dp))
                    Text(
                        text = stringResource(
                            R.string.ci_update_commit,
                            availableRun.commitSha.take(8),
                        ),
                        style = MaterialTheme.typography.titleSmall,
                    )
                    if (availableRun.commitMessage.isNotBlank()) {
                        Spacer(Modifier.height(4.dp))
                        Text(availableRun.commitMessage)
                    }
                }
                when (stage) {
                    CiUpdateStage.READY -> Unit
                    CiUpdateStage.DOWNLOADING -> {
                        Spacer(Modifier.height(16.dp))
                        YukiDownloadProgressIndicator(
                            progress = { progress / 100f },
                        )
                        Spacer(Modifier.height(8.dp))
                        Text(stringResource(R.string.ci_update_downloading, progress))
                    }
                    CiUpdateStage.VERIFYING -> {
                        Spacer(Modifier.height(16.dp))
                        YukiDownloadProgressIndicator()
                        Spacer(Modifier.height(8.dp))
                        Text(stringResource(R.string.ci_update_verifying))
                    }
                    CiUpdateStage.INSTALLING -> {
                        Spacer(Modifier.height(16.dp))
                        YukiDownloadProgressIndicator()
                        Spacer(Modifier.height(8.dp))
                        Text(stringResource(R.string.ci_update_installing))
                    }
                    CiUpdateStage.FAILED -> {
                        Spacer(Modifier.height(16.dp))
                        Text(stringResource(R.string.ci_update_failed, error))
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = ::beginUpdate, enabled = !busy) {
                Text(
                    stringResource(
                        if (stage == CiUpdateStage.FAILED) {
                            R.string.ci_update_retry
                        } else {
                            R.string.ci_update_download
                        }
                    )
                )
            }
        },
        dismissButton = {
            TextButton(onClick = { showDialog = false }, enabled = !busy) {
                Text(stringResource(android.R.string.cancel))
            }
        },
        properties = DialogProperties(
            dismissOnBackPress = !busy,
            dismissOnClickOutside = !busy,
        ),
    )
}

@Composable
internal fun CiUpdateNotice(
    state: CiUpdateCheckState,
    onRetry: () -> Unit,
    onUpdate: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val visible = state == CiUpdateCheckState.Failed || state is CiUpdateCheckState.Available
    var previousNotice by remember { mutableStateOf<CiUpdateCheckState?>(null) }
    val displayedState = if (visible) state else previousNotice
    SideEffect {
        if (visible) previousNotice = state
    }

    YukiExpandableContent(visible = visible, modifier = modifier) {
        when (val notice = displayedState) {
            CiUpdateCheckState.Failed -> WarningCard(
                message = stringResource(R.string.ci_update_check_failed_card),
                onClick = if (visible) onRetry else null,
            )
            is CiUpdateCheckState.Available -> WarningCard(
                message = stringResource(R.string.ci_update_available_card, notice.run.versionCode),
                color = MaterialTheme.colorScheme.outlineVariant,
                onClick = if (visible) onUpdate else null,
            )
            else -> Unit
        }
    }
}
