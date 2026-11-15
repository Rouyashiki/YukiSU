package org.rouyashiki.yukisu.ui.viewmodel

import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.data.registry.DownloadVerificationException
import org.rouyashiki.yukisu.data.script.ReleasedException
import org.rouyashiki.yukisu.data.script.ScriptCommand
import org.rouyashiki.yukisu.data.script.ScriptPackageException
import org.rouyashiki.yukisu.data.script.ScriptRepository
import org.rouyashiki.yukisu.data.script.UnverifiedScriptException
import org.rouyashiki.yukisu.ksuApp
import org.rouyashiki.yukisu.ui.screen.scriptrun.ScriptRunArg
import org.rouyashiki.yukisu.ui.screen.scriptrun.ScriptRunPhase
import org.rouyashiki.yukisu.ui.screen.scriptrun.ScriptRunUiState
import org.rouyashiki.yukisu.ui.util.removeAsRoot
import org.rouyashiki.yukisu.ui.util.runOneshotScript
import java.util.UUID

/** Prepares a one-shot script, collects its parameters and runs it once. */
class ScriptRunViewModel : ViewModel() {

    private val _state = MutableStateFlow(ScriptRunUiState())
    val state: StateFlow<ScriptRunUiState> = _state.asStateFlow()

    /** Full stdout + stderr, for "save log". */
    val logContent = StringBuilder()

    private var visit: String? = null
    private var prepareJob: Job? = null

    /** Token of the current preparation, owning its extraction in [ScriptRepository]. */
    @Volatile
    private var owner: String? = null

    /**
     * Prepares [arg] once per page visit ([visit] survives configuration changes, so a
     * finished run stays on screen after a rotation but a new visit starts over).
     */
    fun load(arg: ScriptRunArg, visit: String) {
        if (this.visit == visit || _state.value.phase == ScriptRunPhase.RUNNING) return
        this.visit = visit
        synchronized(logContent) { logContent.setLength(0) }
        _state.value = ScriptRunUiState(displayName = arg.displayName, offerInstall = arg.offerInstall)
        releaseExtraction()
        val token = UUID.randomUUID().toString()
        owner = token
        prepareJob = viewModelScope.launch {
            runCatching { ScriptRepository.prepare(arg.uri, arg.registryId, token) }
                .onSuccess { prepared ->
                    // Superseded or left: releaseExtraction() already released this token.
                    if (owner != token) return@onSuccess
                    val values = prepared.params.defaults
                    val lastRun = withContext(Dispatchers.IO) { ScriptRepository.history.last(prepared.manifest.id) }
                    _state.update {
                        it.copy(
                            phase = ScriptRunPhase.READY,
                            prepared = prepared,
                            values = values,
                            errors = prepared.params.validate(values),
                            lastRun = lastRun,
                        )
                    }
                }
                .onFailure { e ->
                    if (e is CancellationException) throw e
                    if (e is ReleasedException || owner != token) return@onFailure
                    Log.w(TAG, "cannot prepare script ${arg.uri}", e)
                    _state.update { it.copy(phase = ScriptRunPhase.FAILED, error = errorMessage(e)) }
                }
        }
    }

    fun setValue(key: String, value: String) {
        _state.update { s ->
            val prepared = s.prepared ?: return@update s
            if (s.phase != ScriptRunPhase.READY) return@update s
            val values = s.values + (key to value)
            s.copy(values = values, errors = prepared.params.validate(values))
        }
    }

    fun run() {
        val current = _state.value
        val prepared = current.prepared ?: return
        if (!current.canRun) return
        val dir = prepared.dir.absolutePath
        val token = owner ?: return
        val command = try {
            ScriptCommand.build(
                dir,
                ScriptCommand.environment(ScriptRepository.hostInfo(), prepared.manifest.id, dir, prepared.params, current.values),
            )
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "invalid parameters", e)
            return
        }
        _state.update { it.copy(phase = ScriptRunPhase.RUNNING, output = "") }
        viewModelScope.launch(start = CoroutineStart.UNDISPATCHED) {
            withContext(NonCancellable + Dispatchers.IO) {
                fun appendOutput(line: String) {
                    synchronized(logContent) {
                        val output = "$line\n"
                        _state.update {
                            it.copy(output = if (output.startsWith("\u001b[H\u001b[J")) output.substring(6) else it.output + output)
                        }
                        logContent.append(line).append('\n')
                    }
                }
                val exitCode = try {
                    runOneshotScript(command, ::appendOutput, ::appendOutput)
                } catch (e: Exception) {
                    Log.w(TAG, "cannot run script", e)
                    appendOutput(e.message ?: e.javaClass.simpleName)
                    -1
                } finally {
                    runCatching { removeAsRoot(dir) }
                        .onFailure { Log.w(TAG, "cannot remove $dir", it) }
                    ScriptRepository.release(token)
                    if (owner == token) owner = null
                }
                runCatching { ScriptRepository.recordRun(prepared, exitCode) }
                    .onFailure { Log.w(TAG, "cannot record script result", it) }
                _state.update {
                    it.copy(
                        phase = ScriptRunPhase.FINISHED,
                        exitCode = exitCode,
                        lastRun = runCatching { ScriptRepository.history.last(prepared.manifest.id) }.getOrNull(),
                    )
                }
            }
        }
    }

    /**
     * The page is gone (not just recreated): drop the extraction and start over on the next
     * visit. A running script cleans up when it ends, even after the page is gone.
     */
    fun leave() {
        if (_state.value.phase == ScriptRunPhase.RUNNING) return
        releaseExtraction()
        visit = null
        _state.value = ScriptRunUiState()
    }

    private fun releaseExtraction() {
        prepareJob?.cancel()
        prepareJob = null
        owner?.let(ScriptRepository::release)
        owner = null
    }

    override fun onCleared() {
        if (_state.value.phase != ScriptRunPhase.RUNNING) releaseExtraction()
    }

    private fun errorMessage(e: Throwable): String {
        val res = ksuApp.resources
        return when (e) {
            is UnverifiedScriptException -> res.getString(R.string.script_error_unverified)
            is DownloadVerificationException -> res.getString(R.string.script_error_verify_failed)
            is ScriptPackageException -> when (e.reason) {
                ScriptPackageException.Reason.CORRUPT -> res.getString(R.string.script_error_corrupt)
                ScriptPackageException.Reason.UNSAFE_ENTRY -> res.getString(R.string.script_error_unsafe)
                ScriptPackageException.Reason.TOO_LARGE -> res.getString(R.string.script_error_too_large)
                ScriptPackageException.Reason.MISSING_MODULE_PROP -> res.getString(R.string.script_error_missing_prop)
                ScriptPackageException.Reason.NOT_ONESHOT -> res.getString(R.string.script_error_not_oneshot)
                ScriptPackageException.Reason.INVALID_ID -> res.getString(R.string.script_error_invalid_id)
                ScriptPackageException.Reason.ID_MISMATCH -> res.getString(R.string.script_error_id_mismatch)
                ScriptPackageException.Reason.MISSING_ACTION -> res.getString(R.string.script_error_missing_action)
                ScriptPackageException.Reason.INVALID_PARAMS ->
                    res.getString(R.string.script_error_params, e.cause?.message ?: e.message.orEmpty())
            }

            else -> res.getString(R.string.script_error_generic, e.message ?: e.javaClass.simpleName)
        }
    }

    private companion object {
        const val TAG = "ScriptRunViewModel"
    }
}
