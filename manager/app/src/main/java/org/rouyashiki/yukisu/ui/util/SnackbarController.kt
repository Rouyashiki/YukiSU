package org.rouyashiki.yukisu.ui.util

import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarResult
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

class SnackbarController(private val scope: CoroutineScope) {
    val hostState = SnackbarHostState()
    private var snackbarJob: Job? = null

    fun showSnackbar(
        message: String,
        actionLabel: String? = null,
        withDismissAction: Boolean = false,
        duration: SnackbarDuration = if (actionLabel == null) SnackbarDuration.Short else SnackbarDuration.Indefinite,
        onAction: () -> Unit = {},
    ) {
        scope.launch(Dispatchers.Main.immediate) {
            activeSnackbar?.cancel()
            snackbarJob = launch {
                try {
                    val result = hostState.showSnackbar(message, actionLabel, withDismissAction, duration)
                    if (result == SnackbarResult.ActionPerformed) onAction()
                } finally {
                    if (activeSnackbar === coroutineContext[Job]) activeSnackbar = null
                }
            }
            activeSnackbar = snackbarJob
        }
    }

    fun dismiss() {
        scope.launch(Dispatchers.Main.immediate) {
            snackbarJob?.cancel()
            snackbarJob = null
        }
    }

    private companion object {
        // Local and activity hosts share one active presentation on the main thread.
        var activeSnackbar: Job? = null
    }
}

@Composable
fun rememberSnackbarController(): SnackbarController {
    val scope = rememberCoroutineScope()
    return remember(scope) { SnackbarController(scope) }
}
