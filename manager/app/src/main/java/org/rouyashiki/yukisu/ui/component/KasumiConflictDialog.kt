package org.rouyashiki.yukisu.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.util.KasumiConflict
import org.rouyashiki.yukisu.ui.util.KasumiConflictController
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun KasumiConflictDialog() {
    val controller = remember {
        KasumiConflictController(
            read = { withContext(Dispatchers.IO) { Natives.getKasumiConflicts()?.toList() } },
            acknowledge = { sequence ->
                withContext(Dispatchers.IO) { Natives.ackKasumiConflicts(sequence) }
            },
        )
    }
    val state by controller.state.collectAsStateWithLifecycle()
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()

    LaunchedEffect(lifecycleOwner, controller) {
        lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            while (isActive) {
                controller.refresh()
                delay(2000)
            }
        }
    }

    if (state.events.isEmpty()) return

    YukiAlertDialog(
        onDismissRequest = {},
        properties = DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false),
        icon = {
            Icon(painterResource(R.drawable.ms_warning), contentDescription = null)
        },
        iconContentColor = MaterialTheme.colorScheme.error,
        titleContentColor = MaterialTheme.colorScheme.error,
        title = { Text(stringResource(R.string.kasumi_conflict_title)) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                Text(stringResource(R.string.kasumi_conflict_risk))
                state.events.forEach { event ->
                    val message = when (event.kind) {
                        KasumiConflict.LOADED_MODULE -> R.string.kasumi_conflict_loaded_module
                        KasumiConflict.BUILTIN_CONFIG -> R.string.kasumi_conflict_builtin_config
                        KasumiConflict.MODULE_LOAD -> R.string.kasumi_conflict_module_load
                        else -> R.string.kasumi_conflict_unknown
                    }
                    Text(stringResource(message, event.target))
                }
                Text(stringResource(R.string.kasumi_conflict_recovery))
                if (state.confirmationFailed) {
                    Text(
                        stringResource(R.string.kasumi_conflict_ack_failed),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = !state.confirming,
                onClick = { scope.launch { controller.confirm() } },
            ) {
                Text(stringResource(R.string.kasumi_conflict_confirm))
            }
        },
    )
}
