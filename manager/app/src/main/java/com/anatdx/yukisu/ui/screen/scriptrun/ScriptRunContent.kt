package com.anatdx.yukisu.ui.screen.scriptrun

import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.data.script.ScriptParam
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.ShellHighlightColors
import com.anatdx.yukisu.ui.util.rememberShellHighlight

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ScriptRunContent(state: ScriptRunUiState, actions: ScriptRunActions) {
    val context = LocalContext.current
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    Scaffold(
        topBar = {
            val title: @Composable () -> Unit = { Text(stringResource(R.string.script_run_title)) }
            val back: @Composable () -> Unit = {
                IconButton(onClick = actions.onBack) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                }
            }
            if (isExpressiveUi) {
                YukiLargeTopAppBar(title = title, navigationIcon = back, scrollBehavior = scrollBehavior)
            } else {
                TopAppBar(title = title, navigationIcon = back, scrollBehavior = scrollBehavior)
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        Column(
            Modifier.fillMaxSize().padding(padding).imePadding()
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            val prepared = state.prepared
            when {
                state.phase == ScriptRunPhase.FAILED -> {
                    Text(stringResource(R.string.script_error_title), color = MaterialTheme.colorScheme.error)
                    Text(state.error.orEmpty())
                }
                prepared == null -> {
                    CircularProgressIndicator(Modifier.align(Alignment.CenterHorizontally))
                    Text(stringResource(R.string.script_preparing))
                }
                else -> {
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            val manifest = prepared.manifest
                            Text(manifest.name, style = MaterialTheme.typography.titleLarge)
                            Text(scriptSubtitle(manifest.id, manifest.version, manifest.versionCode))
                            if (manifest.author.isNotBlank()) Text(manifest.author)
                            if (manifest.description.isNotBlank()) Text(manifest.description)
                            Text(stringResource(R.string.script_notice))
                            Text(lastRunText(context, state.lastRun), style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    if (prepared.params.params.isNotEmpty()) {
                        Text(stringResource(R.string.script_params), style = MaterialTheme.typography.titleMedium)
                        prepared.params.params.forEach { param ->
                            val value = state.values[param.key] ?: param.defaultValue
                            val error = state.errors[param.key]
                            when (param) {
                                is ScriptParam.Bool -> Row(
                                    Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
                                ) {
                                    Column(Modifier.weight(1f)) {
                                        Text(param.label)
                                        param.description?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                                    }
                                    Switch(checked = value == "true", onCheckedChange = { actions.onValueChange(param.key, it.toString()) })
                                }
                                is ScriptParam.Choice -> {
                                    var expanded by rememberSaveable(param.key) { mutableStateOf(false) }
                                    Column {
                                        Text(param.label)
                                        param.description?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                                        Box {
                                            OutlinedButton(onClick = { expanded = true }) {
                                                Text(param.options.firstOrNull { it.value == value }?.let { it.label ?: it.value } ?: value)
                                            }
                                            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                                                param.options.forEach { option ->
                                                    DropdownMenuItem(
                                                        text = { Text(option.label ?: option.value) },
                                                        onClick = { expanded = false; actions.onValueChange(param.key, option.value) },
                                                    )
                                                }
                                            }
                                        }
                                        error?.let { Text(paramErrorText(param, it), color = MaterialTheme.colorScheme.error) }
                                    }
                                }
                                is ScriptParam.Text, is ScriptParam.Integer -> OutlinedTextField(
                                    value = value,
                                    onValueChange = { actions.onValueChange(param.key, it) },
                                    label = { Text(param.label) },
                                    modifier = Modifier.fillMaxWidth(),
                                    singleLine = true,
                                    isError = error != null,
                                    supportingText = {
                                        if (error != null) Text(paramErrorText(param, error))
                                        else param.description?.let { Text(it) }
                                    },
                                    keyboardOptions = if (param is ScriptParam.Integer) {
                                        KeyboardOptions(keyboardType = integerKeyboardType(param))
                                    } else KeyboardOptions.Default,
                                )
                            }
                        }
                    }
                    var expanded by rememberSaveable { mutableStateOf(false) }
                    OutlinedCard(
                        onClick = { expanded = !expanded },
                        modifier = Modifier.fillMaxWidth().yukiExpansionSemantics(expanded),
                    ) {
                        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(stringResource(R.string.script_source))
                                if (prepared.actionSourceTruncated) Text(stringResource(R.string.script_source_truncated))
                            }
                            YukiExpandArrow(expanded = expanded)
                        }
                    }
                    YukiExpandableContent(visible = expanded, large = true) {
                        val highlighted = rememberShellHighlight(
                            prepared.actionSource,
                            ShellHighlightColors.forBackground(MaterialTheme.colorScheme.surface),
                        )
                        SelectionContainer {
                            Text(
                                highlighted,
                                modifier = Modifier.horizontalScroll(rememberScrollState()),
                                fontFamily = FontFamily.Monospace,
                                style = MaterialTheme.typography.bodySmall,
                                softWrap = false,
                            )
                        }
                    }
                    Button(onClick = actions.onRun, enabled = state.canRun, modifier = Modifier.fillMaxWidth()) {
                        Icon(Icons.Filled.PlayArrow, null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.script_run))
                    }
                }
            }
            if (state.offerInstall && state.phase != ScriptRunPhase.PREPARING) {
                Text(stringResource(R.string.script_local_notice))
                OutlinedButton(onClick = actions.onInstallAsModule) { Text(stringResource(R.string.script_install_anyway)) }
            }
        }
    }
}
