package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.content.Context
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.component.ConfirmResult
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val SPACING_SMALL = 3.dp
private val SPACING_MEDIUM = 8.dp
private val SPACING_LARGE = 16.dp

data class UmountPathEntry(
    val path: String,
    val flags: Int,
)

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun UmountManagerScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val snackBarHost = LocalSnackbarHost.current
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val confirmDialog = rememberConfirmDialog()

    var pathList by remember { mutableStateOf<List<UmountPathEntry>>(emptyList()) }
    var isLoading by remember { mutableStateOf(false) }
    var showAddDialog by remember { mutableStateOf(false) }

    fun loadPaths() {
        scope.launch(Dispatchers.IO) {
            isLoading = true
            val result = listUmountPaths()
            val entries = parseUmountPaths(result)
            withContext(Dispatchers.Main) {
                pathList = entries
                isLoading = false
            }
        }
    }

    LaunchedEffect(Unit) {
        loadPaths()
    }

    Scaffold(
        topBar = {
            UmountManagerTopBar(
                onBack = { navigator.navigateUp() },
                onRefresh = { loadPaths() },
                scrollBehavior = scrollBehavior
            )
        },
        floatingActionButton = {
            FloatingActionButton(
                onClick = { showAddDialog = true }
            ) {
                Icon(Icons.Filled.Add, contentDescription = null)
            }
        }
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .padding(paddingValues)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
        ) {
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(SPACING_LARGE),
                colors = getCardColors(MaterialTheme.colorScheme.primaryContainer),
                elevation = getCardElevation()
            ) {
                Column(
                    modifier = Modifier.padding(SPACING_LARGE)
                ) {
                    Icon(
                        imageVector = Icons.Filled.Info,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                    Spacer(modifier = Modifier.height(SPACING_MEDIUM))
                    Text(
                        text = stringResource(R.string.umount_path_restart_notice),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                }
            }

            if (isLoading) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator()
                }
            } else {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
                    verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
                ) {
                    items(pathList, key = { it.path }) { entry ->
                        UmountPathCard(
                            entry = entry,
                            onDelete = {
                                scope.launch(Dispatchers.IO) {
                                    val success = removeUmountPath(entry.path)
                                    withContext(Dispatchers.Main) {
                                        if (success) {
                                            snackBarHost.showSnackbar(
                                                resources.getString(R.string.umount_path_removed)
                                            )
                                            loadPaths()
                                        } else {
                                            snackBarHost.showSnackbar(
                                                resources.getString(R.string.operation_failed)
                                            )
                                        }
                                    }
                                }
                            }
                        )
                    }

                    item {
                        Spacer(modifier = Modifier.height(SPACING_LARGE))
                    }

                    item {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = SPACING_LARGE),
                            horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
                        ) {
                            Button(
                                onClick = {
                                    scope.launch {
                                        if (confirmDialog.awaitConfirm(
                                                title = resources.getString(R.string.confirm_action),
                                                content = resources.getString(R.string.confirm_clear_custom_paths)
                                            ) == ConfirmResult.Confirmed) {
                                            withContext(Dispatchers.IO) {
                                                val success = clearCustomUmountPaths()
                                                withContext(Dispatchers.Main) {
                                                    if (success) {
                                                        snackBarHost.showSnackbar(
                                                            resources.getString(R.string.custom_paths_cleared)
                                                        )
                                                        loadPaths()
                                                    } else {
                                                        snackBarHost.showSnackbar(
                                                            resources.getString(R.string.operation_failed)
                                                        )
                                                    }
                                                }
                                            }
                                        }
                                    }
                                },
                                modifier = Modifier.weight(1f)
                            ) {
                                Icon(Icons.Filled.DeleteForever, contentDescription = null)
                                Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                                Text(stringResource(R.string.clear_custom_paths))
                            }

                            Button(
                                onClick = {
                                    scope.launch(Dispatchers.IO) {
                                        val success = applyUmountConfigToKernel()
                                        withContext(Dispatchers.Main) {
                                            if (success) {
                                                snackBarHost.showSnackbar(
                                                    resources.getString(R.string.config_applied)
                                                )
                                            } else {
                                                snackBarHost.showSnackbar(
                                                    resources.getString(R.string.operation_failed)
                                                )
                                            }
                                        }
                                    }
                                },
                                modifier = Modifier.weight(1f)
                            ) {
                                Icon(Icons.Filled.Check, contentDescription = null)
                                Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                                Text(stringResource(R.string.apply_config))
                            }
                        }
                    }
                }
            }
        }

        if (showAddDialog) {
            AddUmountPathDialog(
                onDismiss = { showAddDialog = false },
                onConfirm = { path, flags ->
                    showAddDialog = false

                    scope.launch(Dispatchers.IO) {
                        val success = addUmountPath(path, flags)
                        withContext(Dispatchers.Main) {
                            if (success) {
                                saveUmountConfig()
                                snackBarHost.showSnackbar(
                                    resources.getString(R.string.umount_path_added)
                                )
                                loadPaths()
                            } else {
                                snackBarHost.showSnackbar(
                                    resources.getString(R.string.operation_failed)
                                )
                            }
                        }
                    }
                }
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun UmountManagerTopBar(
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior
) {
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.umount_path_manager),
            fontWeight = FontWeight.Normal
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
        }
    }
    val actions: @Composable RowScope.() -> Unit = {
        IconButton(onClick = onRefresh) {
            Icon(Icons.Filled.Refresh, contentDescription = null)
        }
    }
    val colors = getTopAppBarColors(
        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow
    )
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    YukiLargeTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        actions = actions,
        colors = colors,
        windowInsets = windowInsets,
        scrollBehavior = scrollBehavior
    )
}

@Composable
fun UmountPathCard(
    entry: UmountPathEntry,
    onDelete: () -> Unit
) {
    val confirmDialog = rememberConfirmDialog()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val resources = LocalResources.current

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(SPACING_LARGE),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Filled.Folder,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(24.dp)
            )

            Spacer(modifier = Modifier.width(SPACING_LARGE))

            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = entry.path,
                    style = MaterialTheme.typography.titleMedium
                )
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = buildString {
                        append(resources.getString(R.string.flags))
                        append(": ")
                        append(entry.flags.toUmountFlagName(resources))
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            IconButton(
                onClick = {
                    scope.launch {
                        if (confirmDialog.awaitConfirm(
                                title = resources.getString(R.string.confirm_delete),
                                content = resources.getString(R.string.confirm_delete_umount_path, entry.path)
                            ) == ConfirmResult.Confirmed) {
                            onDelete()
                        }
                    }
                }
            ) {
                Icon(
                    imageVector = Icons.Filled.Delete,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.error
                )
            }
        }
    }
}

@Composable
fun AddUmountPathDialog(
    onDismiss: () -> Unit,
    onConfirm: (String, Int) -> Unit
) {
    var path by rememberSaveable { mutableStateOf("") }
    var flags by rememberSaveable { mutableStateOf("0") }

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.add_umount_path)) },
        text = {
            Column {
                OutlinedTextField(
                    value = path,
                    onValueChange = { path = it },
                    label = { Text(stringResource(R.string.mount_path)) },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true
                )

                Spacer(modifier = Modifier.height(SPACING_MEDIUM))

                OutlinedTextField(
                    value = flags,
                    onValueChange = { flags = it },
                    label = { Text(stringResource(R.string.umount_flags)) },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    supportingText = { Text(stringResource(R.string.umount_flags_hint)) }
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    val flagsInt = flags.toIntOrNull() ?: 0
                    onConfirm(path, flagsInt)
                },
                enabled = path.isNotBlank()
            ) {
                Text(stringResource(android.R.string.ok))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        }
    )
}

private fun parseUmountPaths(output: String): List<UmountPathEntry> {
    // ksud `umount list` prints a 2-line header from the kernel followed by
    // "<path>\t<flags>" rows. ksud's own log lines may bleed onto stdout
    // depending on how the shell wrapper merges stderr, so identify rows by
    // content (absolute path + decimal flags) rather than by line index.
    return output.lineSequence()
        .map { it.trim() }
        .filter { it.isNotEmpty() }
        .mapNotNull { line ->
            val parts = line.split(Regex("\\s+"))
            if (parts.size < 2) return@mapNotNull null
            val path = parts[0]
            val flags = parts[1].toIntOrNull() ?: return@mapNotNull null
            if (!path.startsWith("/")) return@mapNotNull null
            UmountPathEntry(path = path, flags = flags)
        }
        .toList()
}

private fun Int.toUmountFlagName(resources: android.content.res.Resources): String {
    return when (this) {
        2 -> resources.getString(R.string.mnt_detach)
        else -> this.toString()
    }
}
