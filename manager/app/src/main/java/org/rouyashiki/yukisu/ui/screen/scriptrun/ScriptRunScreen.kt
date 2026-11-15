package org.rouyashiki.yukisu.ui.screen.scriptrun

import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.material3.*
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.data.script.*
import org.rouyashiki.yukisu.ui.component.YukiAlertDialog
import org.rouyashiki.yukisu.ui.screen.ActionOutputScreen
import org.rouyashiki.yukisu.ui.screen.FlashIt
import org.rouyashiki.yukisu.ui.util.LocalNavigationLeaveGuard
import org.rouyashiki.yukisu.ui.viewmodel.ScriptRunViewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ScriptRunScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import java.text.DateFormat
import java.util.Date
import java.util.UUID

@Composable
@Destination<RootGraph>
fun ScriptRunScreen(navigator: DestinationsNavigator, arg: ScriptRunArg) {
    val viewModel = viewModel<ScriptRunViewModel>()
    val state by viewModel.state.collectAsStateWithLifecycle()
    val visit = rememberSaveable { UUID.randomUUID().toString() }
    LaunchedEffect(arg, visit) { viewModel.load(arg, visit) }
    val running = state.phase == ScriptRunPhase.RUNNING
    val currentRunning by rememberUpdatedState(running)
    val guard = LocalNavigationLeaveGuard.current
    val owner = remember { Any() }
    DisposableEffect(guard, owner) {
        guard.register(owner, ScriptRunScreenDestination.route) { navigate, intercepted ->
            if (currentRunning) intercepted() else navigate()
        }
        onDispose { guard.unregister(owner) }
    }
    BackHandler(enabled = running) {}
    val activity = LocalActivity.current
    DisposableEffect(viewModel) {
        onDispose { if (activity?.isChangingConfigurations != true) viewModel.leave() }
    }
    val finished = state.phase == ScriptRunPhase.FINISHED
    if (running || finished) {
        val notice = state.exitCode?.let {
            when (ScriptOutcome.of(it)) {
                ScriptOutcome.SUCCESS -> stringResource(R.string.script_result_success)
                ScriptOutcome.NOT_APPLICABLE -> stringResource(R.string.script_result_not_applicable)
                ScriptOutcome.FAILURE -> stringResource(R.string.script_result_failed, it)
            }
        }
        ActionOutputScreen(
            text = state.output,
            isActionRunning = running,
            logContent = viewModel.logContent,
            onClose = { navigator.popBackStack() },
            title = state.prepared?.manifest?.name ?: stringResource(R.string.script_run_title),
            resultNotice = notice,
            logPrefix = "YukiSU_script_log",
        )
        return
    }
    var confirm by remember { mutableStateOf(false) }
    if (confirm) {
        YukiAlertDialog(
            onDismissRequest = { confirm = false },
            title = { Text(stringResource(R.string.script_run_title)) },
            text = { Text(stringResource(R.string.script_run_confirm, state.prepared?.manifest?.name.orEmpty())) },
            confirmButton = {
                TextButton(onClick = { confirm = false; viewModel.run() }) { Text(stringResource(R.string.script_run)) }
            },
            dismissButton = {
                TextButton(onClick = { confirm = false }) { Text(stringResource(android.R.string.cancel)) }
            },
        )
    }
    ScriptRunContent(state, ScriptRunActions(
        onBack = { navigator.popBackStack() },
        onValueChange = viewModel::setValue,
        onRun = { if (state.canRun) confirm = true },
        onInstallAsModule = {
            if (arg.offerInstall) {
                navigator.popBackStack()
                navigator.navigate(FlashScreenDestination(FlashIt.FlashModule(arg.uri)))
            }
        },
    ))
}

@Composable
internal fun paramErrorText(param: ScriptParam, error: ParamError): String = when (error) {
    ParamError.REQUIRED -> stringResource(R.string.script_param_required)
    ParamError.TOO_LONG -> stringResource(R.string.script_param_too_long, (param as? ScriptParam.Text)?.maxLength ?: 0)
    ParamError.INVALID_CHARACTERS -> stringResource(R.string.script_param_invalid_characters)
    ParamError.NOT_AN_INTEGER -> stringResource(R.string.script_param_not_integer)
    ParamError.BELOW_MIN -> stringResource(R.string.script_param_below_min, (param as? ScriptParam.Integer)?.min ?: 0)
    ParamError.ABOVE_MAX -> stringResource(R.string.script_param_above_max, (param as? ScriptParam.Integer)?.max ?: 0)
    ParamError.INVALID_OPTION, ParamError.INVALID_BOOLEAN -> stringResource(R.string.script_param_invalid)
}

internal fun lastRunText(context: Context, record: ScriptRunRecord?): String {
    if (record == null) return context.getString(R.string.script_never_run)
    val time = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(record.time))
    return if (record.versionCode != null) {
        context.getString(R.string.script_last_run_version, time, record.versionCode, record.exitCode)
    } else {
        context.getString(R.string.script_last_run, time, record.exitCode)
    }
}

/** "ID: foo · v1.0 (100)" */
internal fun scriptSubtitle(id: String, version: String, versionCode: Int?): String = buildString {
    append("ID: ").append(id)
    if (version.isNotBlank()) append(" · ").append(version)
    if (versionCode != null) append(" (").append(versionCode).append(')')
}

/** The plain number keypad of most IMEs has no "-": use a text keyboard when negatives are allowed. */
internal fun integerKeyboardType(param: ScriptParam.Integer): KeyboardType =
    if ((param.min ?: Int.MIN_VALUE) < 0) KeyboardType.Ascii else KeyboardType.Number
