package com.anatdx.yukisu.ui.screen

import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.Extension
import androidx.compose.material.icons.outlined.Inventory2
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.FloatingActionButtonDefaults
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.anatdx.yukisu.ui.component.YukiModalBottomSheet
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.AnimatedFab
import com.anatdx.yukisu.ui.component.ConfirmResult
import com.anatdx.yukisu.ui.component.SearchAppBar
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.component.YukiPullToRefreshBox
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.component.rememberConfirmDialog
import com.anatdx.yukisu.ui.component.rememberFabVisibilityState
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getElevatedCardColors
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getElevatedCardElevation
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.ui.util.PluginCommandResult
import com.anatdx.yukisu.ui.util.copyPluginPackageTo
import com.anatdx.yukisu.ui.viewmodel.PluginConfigField
import com.anatdx.yukisu.ui.viewmodel.PluginInfo
import com.anatdx.yukisu.ui.viewmodel.PluginQuickAction
import com.anatdx.yukisu.ui.viewmodel.PluginViewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.PluginRepositoryScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

private const val TAG = "PluginScreen"
private data class PluginLogState(
    val id: String,
    val name: String,
    val output: String,
)

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun PluginScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<PluginViewModel>()
    val context = LocalContext.current
    val resources = LocalResources.current
    val locale = LocalConfiguration.current.locales[0]
    val scope = rememberCoroutineScope()
    val snackBarHost = rememberSnackbarController()
    val confirmDialog = rememberConfirmDialog()
    val listState = rememberLazyListState()
    val fabVisible by rememberFabVisibilityState(listState)
    val pendingPluginOperations = remember { mutableStateMapOf<String, Boolean>() }

    var searchQuery by remember { mutableStateOf("") }
    var showSortSheet by remember { mutableStateOf(false) }
    var enabledFirst by remember { mutableStateOf(true) }
    var isInstalling by remember { mutableStateOf(false) }
    var isLoadingConfig by remember { mutableStateOf(false) }
    var isSavingConfig by remember { mutableStateOf(false) }
    var configPlugin by remember { mutableStateOf<PluginInfo?>(null) }
    var configValues by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    var logState by remember { mutableStateOf<PluginLogState?>(null) }
    var isClearingLog by remember { mutableStateOf(false) }

    fun postSnackbar(message: String) {
        scope.launch { snackBarHost.showSnackbar(message) }
    }

    fun beginPluginOperation(id: String): Boolean {
        if (pendingPluginOperations[id] == true) return false
        pendingPluginOperations[id] = true
        return true
    }

    fun finishPluginOperation(id: String) {
        pendingPluginOperations.remove(id)
    }

    fun presentCommandResult(
        plugin: PluginInfo,
        result: PluginCommandResult,
        successMessage: Int,
        failureMessage: Int,
    ) {
        if (result.output.isNotBlank()) {
            logState = PluginLogState(
                id = plugin.id,
                name = plugin.name.ifBlank { plugin.id },
                output = result.output,
            )
        }
        if (result.output.isBlank() || !result.isSuccess) {
            postSnackbar(
                resources.getString(if (result.isSuccess) successMessage else failureMessage),
            )
        }
    }

    val installLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent(),
    ) { uri: Uri? ->
        if (uri == null || isInstalling) return@rememberLauncherForActivityResult
        isInstalling = true
        scope.launch {
            var cacheFile: File? = null
            try {
                cacheFile = withContext(Dispatchers.IO) {
                    File.createTempFile("plugin_install_", ".zip", context.cacheDir)
                }
                val selectedPlugin = checkNotNull(cacheFile)
                withContext(Dispatchers.IO) {
                    context.contentResolver.openInputStream(uri)?.use { input ->
                        selectedPlugin.outputStream().use { output ->
                            input.copyPluginPackageTo(output)
                        }
                    } ?: throw IllegalStateException("Unable to open selected plugin")
                }
                val result = viewModel.installPlugin(selectedPlugin.absolutePath)
                if (result.isSuccess) {
                    postSnackbar(resources.getString(R.string.plugin_install_success))
                } else {
                    val message = result.output.takeIf { it.isNotBlank() }?.let { reason ->
                        resources.getString(R.string.plugin_install_failed_reason, reason)
                    } ?: resources.getString(R.string.plugin_install_failed)
                    postSnackbar(message)
                }
            } catch (error: Exception) {
                Log.e(TAG, "Failed to install plugin", error)
                postSnackbar(resources.getString(R.string.plugin_install_failed))
            } finally {
                cacheFile?.delete()
                isInstalling = false
            }
        }
    }

    LaunchedEffect(Unit) { viewModel.fetchPlugins() }

    LaunchedEffect(viewModel.errorMessage) {
        if (viewModel.errorMessage != null) {
            postSnackbar(resources.getString(R.string.plugin_load_failed))
        }
    }

    val localeTag = locale.toLanguageTag()
    val filteredPlugins = remember(
        viewModel.plugins,
        searchQuery,
        enabledFirst,
        localeTag,
    ) {
        viewModel.plugins
            .filter { plugin ->
                val description = localizedText(plugin.descriptions, locale, plugin.description)
                searchQuery.isBlank() || listOf(
                    plugin.id,
                    plugin.name,
                    plugin.author,
                    description,
                    plugin.error,
                ).any { it.contains(searchQuery, ignoreCase = true) }
            }
            .let { plugins ->
                if (enabledFirst) plugins.sortedByDescending { it.enabled } else plugins
            }
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = {
                    Text(
                        text = stringResource(R.string.plugin),
                        fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
                    )
                },
                searchText = searchQuery,
                onSearchTextChange = { searchQuery = it },
                onClearClick = { searchQuery = "" },
                dropdownContent = {
                    IconButton(onClick = { navigator.navigate(PluginRepositoryScreenDestination) }) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Inventory2,
                            contentDescription = stringResource(R.string.plugin_repositories),
                        )
                    }
                    IconButton(onClick = { showSortSheet = true }) {
                        YukiIcon(
                            imageVector = Icons.Filled.MoreVert,
                            contentDescription = stringResource(R.string.plugin_sort_options),
                        )
                    }
                },
            )
        },
        floatingActionButton = {
            AnimatedFab(visible = fabVisible && !isInstalling) {
                FloatingActionButton(
                    shape = if (isExpressiveUi) CircleShape else FloatingActionButtonDefaults.shape,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                    containerColor = MaterialTheme.colorScheme.primary,
                    onClick = { installLauncher.launch("application/zip") },
                ) {
                    YukiIcon(
                        imageVector = Icons.Outlined.Add,
                        contentDescription = stringResource(R.string.plugin_install),
                    )
                }
            }
        },
        snackbarHost = { SnackbarHost(snackBarHost.hostState) },
    ) { padding ->
        when {
            viewModel.plugins.isEmpty() && viewModel.isRefreshing -> {
                Box(
                    modifier = Modifier.fillMaxSize().padding(padding),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator()
                }
            }

            viewModel.plugins.isEmpty() -> {
                PluginEmptyState(
                    loadFailed = viewModel.errorMessage != null,
                    modifier = Modifier.fillMaxSize().padding(padding),
                )
            }

            filteredPlugins.isEmpty() -> {
                PluginEmptyState(
                    noSearchResults = true,
                    modifier = Modifier.fillMaxSize().padding(padding),
                )
            }

            else -> {
                YukiPullToRefreshBox(
                    modifier = Modifier.fillMaxSize().padding(padding),
                    onRefresh = viewModel::fetchPlugins,
                    isRefreshing = viewModel.isRefreshing,
                ) {
                    LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(
                            start = 16.dp,
                            top = 12.dp,
                            end = 16.dp,
                            bottom = 96.dp,
                        ),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        items(filteredPlugins, key = { it.id }) { plugin ->
                            PluginCard(
                                plugin = plugin,
                                locale = locale,
                                operationInProgress = pendingPluginOperations[plugin.id] == true,
                                onToggle = { enabled ->
                                    if (beginPluginOperation(plugin.id)) {
                                        scope.launch {
                                            try {
                                                val success = viewModel.setPluginEnabled(plugin.id, enabled)
                                                val message = when {
                                                    !success -> R.string.plugin_toggle_failed
                                                    enabled -> R.string.plugin_state_enabled
                                                    else -> R.string.plugin_state_disabled
                                                }
                                                postSnackbar(resources.getString(message))
                                            } finally {
                                                finishPluginOperation(plugin.id)
                                            }
                                        }
                                    }
                                },
                                onAction = {
                                    val canRun = plugin.hasAction && plugin.enabled &&
                                        plugin.hasManifest && plugin.error.isBlank()
                                    if (canRun && beginPluginOperation(plugin.id)) {
                                        scope.launch {
                                            try {
                                                presentCommandResult(
                                                    plugin = plugin,
                                                    result = viewModel.runAction(plugin.id),
                                                    successMessage = R.string.plugin_action_success,
                                                    failureMessage = R.string.plugin_action_failed,
                                                )
                                            } finally {
                                                finishPluginOperation(plugin.id)
                                            }
                                        }
                                    }
                                },
                                onQuickAction = {
                                    val canRun = plugin.enabled && plugin.hasManifest && plugin.error.isBlank()
                                    plugin.quickAction?.function?.takeIf {
                                        canRun && beginPluginOperation(plugin.id)
                                    }?.let { function ->
                                        scope.launch {
                                            try {
                                                presentCommandResult(
                                                    plugin = plugin,
                                                    result = viewModel.runCallback(plugin.id, function),
                                                    successMessage = R.string.plugin_quick_action_success,
                                                    failureMessage = R.string.plugin_quick_action_failed,
                                                )
                                            } finally {
                                                finishPluginOperation(plugin.id)
                                            }
                                        }
                                    }
                                },
                                onConfig = {
                                    if (!isLoadingConfig) {
                                        isLoadingConfig = true
                                        scope.launch {
                                            try {
                                                val values = viewModel.loadConfigValues(plugin)
                                                if (values == null) {
                                                    postSnackbar(
                                                        resources.getString(R.string.plugin_config_load_failed)
                                                    )
                                                } else {
                                                    configValues = values
                                                    configPlugin = plugin
                                                }
                                            } finally {
                                                isLoadingConfig = false
                                            }
                                        }
                                    }
                                },
                                onViewLog = {
                                    scope.launch {
                                        val result = viewModel.fetchLog(plugin.id)
                                        if (result.isSuccess) {
                                            logState = PluginLogState(
                                                id = plugin.id,
                                                name = plugin.name.ifBlank { plugin.id },
                                                output = result.stdout.ifBlank {
                                                    resources.getString(R.string.plugin_log_empty)
                                                },
                                            )
                                        } else {
                                            postSnackbar(
                                                resources.getString(R.string.plugin_log_load_failed)
                                            )
                                        }
                                    }
                                },
                                onClearLog = {
                                    scope.launch {
                                        val success = viewModel.clearLog(plugin.id)
                                        postSnackbar(
                                            resources.getString(
                                                if (success) {
                                                    R.string.plugin_log_cleared
                                                } else {
                                                    R.string.plugin_log_clear_failed
                                                }
                                            )
                                        )
                                    }
                                },
                                onUninstall = {
                                    if (beginPluginOperation(plugin.id)) {
                                        scope.launch {
                                            try {
                                                val displayName = plugin.name.ifBlank { plugin.id }
                                                val confirmation = confirmDialog.awaitConfirm(
                                                    title = resources.getString(R.string.plugin_uninstall_title),
                                                    content = resources.getString(
                                                        R.string.plugin_uninstall_confirm,
                                                        displayName,
                                                    ),
                                                    confirm = resources.getString(R.string.plugin_uninstall),
                                                    dismiss = resources.getString(R.string.cancel),
                                                )
                                                if (confirmation != ConfirmResult.Confirmed) return@launch

                                                val success = viewModel.removePlugin(plugin.id)
                                                postSnackbar(
                                                    resources.getString(
                                                        if (success) {
                                                            R.string.plugin_uninstall_success
                                                        } else {
                                                            R.string.plugin_uninstall_failed
                                                        }
                                                    )
                                                )
                                            } finally {
                                                finishPluginOperation(plugin.id)
                                            }
                                        }
                                    }
                                },
                            )
                        }
                    }
                }
            }
        }
    }

    if (showSortSheet) {
        YukiModalBottomSheet(onDismissRequest = { showSortSheet = false }) {
            Column(Modifier.padding(horizontal = 24.dp, vertical = 8.dp)) {
                Text(
                    text = stringResource(R.string.plugin_sort_options),
                    style = MaterialTheme.typography.titleMedium,
                )
                Spacer(Modifier.height(12.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(stringResource(R.string.plugin_enabled_first))
                    YukiSwitch(
                        checked = enabledFirst,
                        onCheckedChange = { enabledFirst = it },
                    )
                }
                Spacer(Modifier.height(24.dp))
            }
        }
    }

    configPlugin?.let { plugin ->
        PluginConfigDialog(
            plugin = plugin,
            initial = configValues,
            locale = locale,
            isSaving = isSavingConfig,
            onDismiss = {
                if (!isSavingConfig) {
                    configPlugin = null
                    configValues = emptyMap()
                }
            },
            onSave = { values ->
                if (!isSavingConfig) {
                    isSavingConfig = true
                    scope.launch {
                        try {
                            val success = viewModel.saveConfigValues(plugin.id, values)
                            if (success) {
                                configPlugin = null
                                configValues = emptyMap()
                            }
                            postSnackbar(
                                resources.getString(
                                    if (success) {
                                        R.string.plugin_config_saved
                                    } else {
                                        R.string.plugin_config_save_failed
                                    }
                                )
                            )
                        } finally {
                            isSavingConfig = false
                        }
                    }
                }
            },
        )
    }

    logState?.let { state ->
        PluginLogDialog(
            state = state,
            isClearing = isClearingLog,
            onDismiss = {
                if (!isClearingLog) logState = null
            },
            onClear = {
                if (!isClearingLog) {
                    isClearingLog = true
                    scope.launch {
                        try {
                            val success = viewModel.clearLog(state.id)
                            if (success) logState = null
                            postSnackbar(
                                resources.getString(
                                    if (success) {
                                        R.string.plugin_log_cleared
                                    } else {
                                        R.string.plugin_log_clear_failed
                                    }
                                )
                            )
                        } finally {
                            isClearingLog = false
                        }
                    }
                }
            },
        )
    }
}

@Composable
private fun PluginCard(
    plugin: PluginInfo,
    locale: Locale,
    operationInProgress: Boolean,
    onToggle: (Boolean) -> Unit,
    onAction: () -> Unit,
    onQuickAction: () -> Unit,
    onConfig: () -> Unit,
    onViewLog: () -> Unit,
    onClearLog: () -> Unit,
    onUninstall: () -> Unit,
) {
    var showMenu by remember { mutableStateOf(false) }
    val description = localizedText(plugin.descriptions, locale, plugin.description)
    val manifestError = plugin.error.ifBlank {
        if (plugin.hasManifest) "" else stringResource(R.string.plugin_invalid_manifest)
    }
    val canRunActions = plugin.enabled && plugin.hasManifest && plugin.error.isBlank()
    val version = plugin.version.takeIf { it.isNotBlank() }?.let {
        stringResource(R.string.plugin_version, it)
    }
    val author = plugin.author.takeIf { it.isNotBlank() }?.let {
        stringResource(R.string.plugin_author, it)
    }
    val metadata = listOfNotNull(version, author, plugin.license.takeIf { it.isNotBlank() })
        .joinToString(" · ")

    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        colors = getElevatedCardColors(),
        elevation = getElevatedCardElevation(),
        shape = if (isExpressiveUi) RoundedCornerShape(20.dp) else RoundedCornerShape(16.dp),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                YukiIcon(
                    imageVector = Icons.Outlined.Extension,
                    contentDescription = null,
                    modifier = Modifier.size(28.dp),
                    tint = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        text = plugin.name.ifBlank { plugin.id },
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    if (metadata.isNotBlank()) {
                        Spacer(Modifier.height(2.dp))
                        Text(
                            text = metadata,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Box {
                    IconButton(onClick = { showMenu = true }, enabled = !operationInProgress) {
                        YukiIcon(
                            imageVector = Icons.Filled.MoreVert,
                            contentDescription = stringResource(R.string.plugin_more_options),
                        )
                    }
                    DropdownMenu(
                        expanded = showMenu,
                        onDismissRequest = { showMenu = false },
                    ) {
                        if (plugin.hasAction) {
                            DropdownMenuItem(
                                text = { Text(stringResource(R.string.plugin_action)) },
                                leadingIcon = {
                                    YukiIcon(Icons.Outlined.PlayArrow, contentDescription = null)
                                },
                                enabled = canRunActions && !operationInProgress,
                                onClick = {
                                    showMenu = false
                                    onAction()
                                },
                            )
                        }
                        DropdownMenuItem(
                            text = { Text(stringResource(R.string.plugin_log_view)) },
                            leadingIcon = { YukiIcon(Icons.Outlined.Description, contentDescription = null) },
                            onClick = {
                                showMenu = false
                                onViewLog()
                            },
                        )
                        DropdownMenuItem(
                            text = { Text(stringResource(R.string.plugin_log_clear)) },
                            leadingIcon = { YukiIcon(Icons.Filled.DeleteSweep, contentDescription = null) },
                            onClick = {
                                showMenu = false
                                onClearLog()
                            },
                        )
                        DropdownMenuItem(
                            text = { Text(stringResource(R.string.plugin_uninstall)) },
                            leadingIcon = { YukiIcon(Icons.Outlined.Delete, contentDescription = null) },
                            onClick = {
                                showMenu = false
                                onUninstall()
                            },
                        )
                    }
                }
                YukiSwitch(
                    modifier = Modifier.semantics { contentDescription = plugin.name.ifBlank { plugin.id } },
                    checked = plugin.enabled,
                    onCheckedChange = onToggle,
                    enabled = !operationInProgress,
                )
            }

            if (description.isNotBlank()) {
                Spacer(Modifier.height(10.dp))
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            if (manifestError.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    YukiIcon(
                        imageVector = Icons.Outlined.Warning,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp),
                        tint = MaterialTheme.colorScheme.error,
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = stringResource(R.string.plugin_error, manifestError),
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }

            if (plugin.depends.isNotEmpty()) {
                Spacer(Modifier.height(6.dp))
                Text(
                    text = stringResource(
                        R.string.plugin_dependencies,
                        plugin.depends.joinToString(", "),
                    ),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.tertiary,
                )
            }

            if (plugin.quickAction != null || plugin.config.isNotEmpty()) {
                Spacer(Modifier.height(14.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    plugin.quickAction?.let { quickAction ->
                        FilledTonalButton(
                            onClick = onQuickAction,
                            modifier = Modifier.weight(1f),
                            enabled = canRunActions && !operationInProgress,
                            contentPadding = ButtonDefaults.ContentPadding,
                        ) {
                            YukiIcon(
                                imageVector = Icons.Outlined.PlayArrow,
                                contentDescription = null,
                                modifier = Modifier.size(18.dp),
                            )
                            Spacer(Modifier.width(6.dp))
                            Text(
                                text = quickActionLabel(quickAction, locale),
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                    if (plugin.config.isNotEmpty()) {
                        FilledTonalButton(
                            onClick = onConfig,
                            modifier = Modifier.weight(1f),
                            enabled = !operationInProgress,
                            contentPadding = ButtonDefaults.ContentPadding,
                        ) {
                            YukiIcon(
                                imageVector = Icons.Outlined.Settings,
                                contentDescription = null,
                                modifier = Modifier.size(18.dp),
                            )
                            Spacer(Modifier.width(6.dp))
                            Text(stringResource(R.string.plugin_config))
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PluginEmptyState(
    modifier: Modifier = Modifier,
    loadFailed: Boolean = false,
    noSearchResults: Boolean = false,
) {
    Column(
        modifier = modifier.padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        YukiIcon(
            imageVector = if (loadFailed) Icons.Outlined.Warning else Icons.Outlined.Extension,
            contentDescription = null,
            modifier = Modifier.size(56.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(16.dp))
        Text(
            text = stringResource(
                when {
                    loadFailed -> R.string.plugin_load_failed
                    noSearchResults -> R.string.plugin_no_results
                    else -> R.string.plugin_empty
                }
            ),
            style = MaterialTheme.typography.titleMedium,
            textAlign = TextAlign.Center,
        )
        if (!loadFailed && !noSearchResults) {
            Spacer(Modifier.height(8.dp))
            Text(
                text = stringResource(R.string.plugin_empty_hint),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PluginConfigDialog(
    plugin: PluginInfo,
    initial: Map<String, String>,
    locale: Locale,
    isSaving: Boolean,
    onDismiss: () -> Unit,
    onSave: (Map<String, String>) -> Unit,
) {
    val values = remember(plugin.id, initial) {
        mutableStateMapOf<String, String>().apply { putAll(initial) }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(
                stringResource(
                    R.string.plugin_config_title,
                    plugin.name.ifBlank { plugin.id },
                )
            )
        },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().heightIn(max = 420.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                plugin.config.forEach { field ->
                    when (field.type) {
                        "bool" -> {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    text = configFieldLabel(field, locale),
                                    modifier = Modifier.weight(1f),
                                    style = MaterialTheme.typography.bodyMedium,
                                )
                                YukiSwitch(
                                    checked = (values[field.key] ?: field.default) == "true",
                                    onCheckedChange = {
                                        values[field.key] = if (it) "true" else "false"
                                    },
                                    enabled = !isSaving,
                                )
                            }
                        }

                        "select" -> {
                            var expanded by remember(field.key) { mutableStateOf(false) }
                            Text(
                                text = configFieldLabel(field, locale),
                                style = MaterialTheme.typography.labelMedium,
                            )
                            ExposedDropdownMenuBox(
                                expanded = expanded,
                                onExpandedChange = { if (!isSaving) expanded = it },
                            ) {
                                OutlinedTextField(
                                    value = values[field.key] ?: field.default,
                                    onValueChange = {},
                                    readOnly = true,
                                    enabled = !isSaving,
                                    singleLine = true,
                                    modifier = Modifier.fillMaxWidth().menuAnchor(
                                        type = ExposedDropdownMenuAnchorType.PrimaryNotEditable,
                                        enabled = !isSaving,
                                    ),
                                    trailingIcon = {
                                        YukiExpandArrow(expanded = expanded)
                                    },
                                )
                                DropdownMenu(
                                    expanded = expanded,
                                    onDismissRequest = { expanded = false },
                                ) {
                                    field.options.forEach { option ->
                                        DropdownMenuItem(
                                            text = { Text(option) },
                                            onClick = {
                                                values[field.key] = option
                                                expanded = false
                                            },
                                        )
                                    }
                                }
                            }
                        }

                        else -> {
                            Text(
                                text = configFieldLabel(field, locale),
                                style = MaterialTheme.typography.labelMedium,
                            )
                            OutlinedTextField(
                                value = values[field.key] ?: field.default,
                                onValueChange = { values[field.key] = it },
                                enabled = !isSaving,
                                singleLine = true,
                                modifier = Modifier.fillMaxWidth(),
                                keyboardOptions = if (field.type == "number") {
                                    KeyboardOptions(keyboardType = KeyboardType.Number)
                                } else {
                                    KeyboardOptions.Default
                                },
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = !isSaving,
                onClick = {
                    onSave(
                        plugin.config.associate { field ->
                            field.key to (values[field.key] ?: field.default)
                        }
                    )
                },
            ) {
                Text(stringResource(R.string.plugin_save))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = !isSaving) {
                Text(stringResource(R.string.cancel))
            }
        },
    )
}

@Composable
private fun PluginLogDialog(
    state: PluginLogState,
    isClearing: Boolean,
    onDismiss: () -> Unit,
    onClear: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.plugin_log_title, state.name)) },
        text = {
            Box(Modifier.fillMaxWidth().heightIn(max = 360.dp).verticalScroll(rememberScrollState())) {
                SelectionContainer {
                    Text(
                        text = state.output,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onClear, enabled = !isClearing) {
                Text(stringResource(R.string.plugin_log_clear))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = !isClearing) {
                Text(stringResource(R.string.close))
            }
        },
    )
}

private fun quickActionLabel(action: PluginQuickAction, locale: Locale): String =
    localizedText(action.labels, locale, action.label.ifBlank { action.function })

private fun configFieldLabel(field: PluginConfigField, locale: Locale): String =
    localizedText(field.labels, locale, field.label.ifBlank { field.key })

internal fun localizedText(
    translations: Map<String, String>,
    locale: Locale,
    fallback: String,
): String {
    if (translations.isEmpty()) return fallback

    fun normalize(tag: String): String = tag
        .replace('_', '-')
        .replace("-r", "-", ignoreCase = true)
        .lowercase(Locale.ROOT)

    val normalized = translations.entries
        .filter { it.value.isNotBlank() }
        .associate { normalize(it.key) to it.value }
    val language = locale.language.lowercase(Locale.ROOT)
    val candidates = buildList {
        add(normalize(locale.toLanguageTag()))
        if (locale.script.isNotBlank()) add(normalize("$language-${locale.script}"))
        if (locale.country.isNotBlank()) add(normalize("$language-${locale.country}"))
        add(language)
    }
    candidates.firstNotNullOfOrNull(normalized::get)?.let { return it }

    val languageVariants = normalized.filterKeys { it.startsWith("$language-") }
    if (language == "zh" && languageVariants.isNotEmpty()) {
        val traditional = locale.script.equals("Hant", ignoreCase = true) ||
            locale.country.uppercase(Locale.ROOT) in setOf("TW", "HK", "MO")
        val markers = if (traditional) {
            listOf("hant", "tw", "hk", "mo")
        } else {
            listOf("hans", "cn", "sg")
        }
        languageVariants.entries.firstOrNull { entry ->
            markers.any { marker -> entry.key.contains(marker) }
        }?.let { return it.value }
    }

    return languageVariants.values.firstOrNull() ?: fallback
}
