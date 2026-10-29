package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import android.annotation.SuppressLint
import android.os.Environment
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.KeyEventBlocker
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.LocalSnackbarHost
import com.anatdx.yukisu.ui.util.listModules
import com.anatdx.yukisu.ui.util.module.moduleActionAvailability
import com.anatdx.yukisu.ui.util.runModuleAction
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

@SuppressLint("LocalContextGetResourceValueCall")
@OptIn(ExperimentalMaterial3Api::class)
@Composable
@Destination<RootGraph>
fun ExecuteModuleActionScreen(navigator: DestinationsNavigator, moduleId: String, fromShortcut: Boolean = false) {
    var text by rememberSaveable { mutableStateOf("") }
    var tempText : String
    val logContent = remember { StringBuilder() }
    val activity = LocalActivity.current
    val context = LocalContext.current
    var isActionRunning by rememberSaveable { mutableStateOf(true) }
    val exitAction: () -> Unit = {
        if (fromShortcut && activity != null) activity.finish() else navigator.popBackStack()
    }

    BackHandler(enabled = isActionRunning) {
        // Disable back button if action is running
    }

    LaunchedEffect(Unit) {
        if (text.isNotEmpty()) {
            return@LaunchedEffect
        }
        val availability = try {
            withContext(Dispatchers.IO) { moduleActionAvailability(listModules(), moduleId) }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            Log.e("ModuleAction", "Cannot read module state", error)
            Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
            isActionRunning = false
            exitAction()
            return@LaunchedEffect
        }
        if (availability?.available != true) {
            val message = if (availability == null) {
                context.getString(R.string.no_such_module, moduleId)
            } else {
                context.getString(R.string.module_unavailable, availability.name)
            }
            Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
            isActionRunning = false
            exitAction()
            return@LaunchedEffect
        }
        val success = withContext(Dispatchers.IO) {
            runModuleAction(
                moduleId = moduleId,
                onStdout = {
                    tempText = "$it\n"
                    if (tempText.startsWith("[H[J")) { // clear command
                        text = tempText.substring(6)
                    } else {
                        text += tempText
                    }
                    logContent.append(it).append("\n")
                },
                onStderr = {
                    logContent.append(it).append("\n")
                }
            )
        }
        isActionRunning = false
        if (fromShortcut) {
            activity?.let { act ->
                if (success) {
                    Toast.makeText(
                        act,
                        act.getString(R.string.module_action_success),
                        Toast.LENGTH_SHORT
                    ).show()
                }
                act.finish()
            }
        }
    }

    ActionOutputScreen(
        text = text,
        isActionRunning = isActionRunning,
        logContent = logContent,
        onClose = exitAction,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ActionOutputScreen(
    text: String,
    isActionRunning: Boolean,
    logContent: StringBuilder,
    onClose: () -> Unit,
    title: String = stringResource(R.string.action),
    resultNotice: String? = null,
    logPrefix: String = "KernelSU_module_action_log",
) {
    val snackBarHost = LocalSnackbarHost.current
    val scope = rememberCoroutineScope()
    val scrollState = rememberScrollState()
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    Scaffold(
        topBar = {
            TopBar(
                isActionRunning = isActionRunning,
                scrollBehavior = scrollBehavior,
                titleText = title,
                onSave = {
                    if (!isActionRunning) {
                        scope.launch {
                            val format = SimpleDateFormat("yyyy-MM-dd-HH-mm-ss", Locale.getDefault())
                            val date = format.format(Date())
                            val file = File(
                                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                                "${logPrefix}_${date}.log"
                            )
                            file.writeText(logContent.toString())
                            snackBarHost.showSnackbar("Log saved to ${file.absolutePath}")
                        }
                    }
                }
            )
        },
        floatingActionButton = {
            if (!isActionRunning) {
                ExtendedFloatingActionButton(
                    text = { Text(text = stringResource(R.string.close)) },
                    icon = {
                        YukiIcon(Icons.Filled.Close, contentDescription = null)
                    },
                    onClick = {
                        onClose()
                    }
                )
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing
    ) { innerPadding ->
        KeyEventBlocker {
            it.key == Key.VolumeDown || it.key == Key.VolumeUp
        }
        Column(
            modifier = Modifier
                .fillMaxSize(1f)
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .verticalScroll(scrollState),
        ) {
            val motion = LocalYukiMotion.current
            LaunchedEffect(text, motion) {
                if (motion.enabled) scrollState.animateScrollTo(scrollState.maxValue, motion.effects())
                else scrollState.scrollTo(scrollState.maxValue)
            }
            resultNotice?.let {
                Text(it, modifier = Modifier.padding(16.dp), color = MaterialTheme.colorScheme.primary)
            }
            Text(
                modifier = Modifier.padding(if (isExpressiveUi) 16.dp else 8.dp),
                text = text,
                fontSize = MaterialTheme.typography.bodySmall.fontSize,
                fontFamily = FontFamily.Monospace,
                lineHeight = MaterialTheme.typography.bodySmall.lineHeight,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    isActionRunning: Boolean,
    titleText: String,
    onSave: () -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null,
) {
    val title: @Composable () -> Unit = {
        Text(
            text = titleText,
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
        )
    }
    val actions: @Composable RowScope.() -> Unit = {
        IconButton(
            onClick = onSave,
            enabled = !isActionRunning
        ) {
            YukiIcon(
                imageVector = Icons.Filled.Save,
                contentDescription = stringResource(id = R.string.save_log),
            )
        }
    }
    val colors = getTopAppBarColors(
        containerColor = if (isExpressiveUi) {
            MaterialTheme.colorScheme.surfaceContainerLow
        } else {
            MaterialTheme.colorScheme.background
        },
        scrolledContainerColor = if (isExpressiveUi) {
            MaterialTheme.colorScheme.surfaceContainerLow
        } else {
            MaterialTheme.colorScheme.background
        },
    )

    if (isExpressiveUi) {
        YukiLargeTopAppBar(
            title = title,
            actions = actions,
            colors = colors,
            scrollBehavior = scrollBehavior,
        )
    } else {
        TopAppBar(
            title = title,
            actions = actions,
            colors = colors,
            scrollBehavior = scrollBehavior,
        )
    }
}
