package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import android.net.Uri
import android.widget.Toast
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.OpenInNew
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.ArrowDownward
import androidx.compose.material.icons.outlined.ArrowUpward
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Storage
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.anatdx.yukisu.R
import com.anatdx.yukisu.data.repository.PluginRepositorySource
import com.anatdx.yukisu.data.repository.RepositoryPlugin
import com.anatdx.yukisu.ui.component.DownloadProgressDialog
import com.anatdx.yukisu.ui.component.SearchAppBar
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getCardColors
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getCardElevation
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.ui.util.DownloadHandle
import com.anatdx.yukisu.ui.util.DownloadProgress
import com.anatdx.yukisu.ui.util.MAX_PLUGIN_PACKAGE_BYTES
import com.anatdx.yukisu.ui.util.copyPluginPackageTo
import com.anatdx.yukisu.ui.util.download
import com.anatdx.yukisu.ui.util.safeDownloadFileName
import com.anatdx.yukisu.ui.viewmodel.PluginInfo
import com.anatdx.yukisu.ui.viewmodel.PluginRepositoryViewModel
import com.anatdx.yukisu.ui.viewmodel.PluginViewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.PluginRepositorySourcesScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun PluginRepositoryScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<PluginRepositoryViewModel>()
    val pluginViewModel = viewModel<PluginViewModel>()
    val sources by viewModel.sources.collectAsState()
    val catalog by viewModel.catalog.collectAsState()
    val context = LocalContext.current
    val resources = LocalResources.current
    val locale = LocalConfiguration.current.locales[0]
    val scope = rememberCoroutineScope()
    val snackbarHost = rememberSnackbarController()
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    var installTarget by remember { mutableStateOf<RepositoryPlugin?>(null) }
    var downloadingPlugin by remember { mutableStateOf<RepositoryPlugin?>(null) }
    var downloadProgress by remember { mutableStateOf(DownloadProgress()) }
    var downloadHandle by remember { mutableStateOf<DownloadHandle?>(null) }
    var installingId by remember { mutableStateOf<String?>(null) }

    val installed = remember(pluginViewModel.plugins) {
        pluginViewModel.plugins.associateBy(PluginInfo::id)
    }
    val plugins = remember(catalog, viewModel.search, viewModel.selectedSourceId) {
        viewModel.visiblePlugins(catalog)
    }

    fun postSnackbar(message: String) {
        scope.launch { snackbarHost.showSnackbar(message) }
    }

    fun installDownloaded(plugin: RepositoryPlugin, uri: Uri) {
        downloadingPlugin = null
        downloadHandle = null
        installingId = plugin.stableId
        scope.launch {
            var cacheFile: File? = null
            try {
                val result = withContext(NonCancellable) {
                    cacheFile = withContext(Dispatchers.IO) {
                        File.createTempFile("plugin_repository_", ".zip", context.cacheDir).also { file ->
                            context.contentResolver.openInputStream(uri)?.use { input ->
                                file.outputStream().use { output -> input.copyPluginPackageTo(output) }
                            } ?: error("Unable to open downloaded plugin")
                        }
                    }
                    pluginViewModel.installPlugin(checkNotNull(cacheFile).absolutePath)
                }
                val message = if (result.isSuccess) {
                    resources.getString(R.string.plugin_install_success)
                } else {
                    result.output.takeIf(String::isNotBlank)?.let { reason ->
                        resources.getString(R.string.plugin_install_failed_reason, reason)
                    } ?: resources.getString(R.string.plugin_install_failed)
                }
                postSnackbar(message)
            } catch (error: Exception) {
                postSnackbar(
                    resources.getString(
                        R.string.plugin_repository_download_failed_reason,
                        error.message ?: error.javaClass.simpleName,
                    )
                )
            } finally {
                cacheFile?.delete()
                installingId = null
            }
        }
    }

    fun startDownload(plugin: RepositoryPlugin) {
        val fileName = safeDownloadFileName(
            "${plugin.pluginId ?: plugin.name}-${plugin.version.ifBlank { "latest" }}.zip"
        )
        downloadingPlugin = plugin
        downloadProgress = DownloadProgress()
        downloadHandle = download(
            context = context,
            url = plugin.downloadUrl,
            fileName = fileName,
            description = plugin.name,
            maxBytes = MAX_PLUGIN_PACKAGE_BYTES,
            requireSecureTransport = true,
            onDownloaded = { uri -> installDownloaded(plugin, uri) },
            onProgress = { progress -> downloadProgress = progress },
            onError = { error ->
                downloadingPlugin = null
                downloadHandle = null
                postSnackbar(
                    resources.getString(R.string.plugin_repository_download_failed_reason, error)
                )
            },
        )
    }

    LaunchedEffect(Unit) {
        pluginViewModel.fetchPlugins()
        if (catalog.isEmpty()) viewModel.refreshAll()
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = {
                    Text(
                        stringResource(R.string.plugin_repositories),
                        fontWeight = FontWeight.Normal,
                    )
                },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                onBackClick = navigator::popBackStack,
                dropdownContent = {
                    IconButton(onClick = viewModel::refreshAll) {
                        YukiIcon(Icons.Outlined.Refresh, stringResource(R.string.refresh))
                    }
                    IconButton(
                        onClick = { navigator.navigate(PluginRepositorySourcesScreenDestination) }
                    ) {
                        YukiIcon(Icons.Outlined.Storage, stringResource(R.string.repository_sources))
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        ),
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection),
        ) {
            if (viewModel.isRefreshing) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            PluginRepositorySourceFilter(
                sources = sources.filter(PluginRepositorySource::enabled),
                selectedSourceId = viewModel.selectedSourceId,
                onSelect = { viewModel.selectedSourceId = it },
            )
            when {
                plugins.isEmpty() && viewModel.isRefreshing -> {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator()
                    }
                }

                plugins.isEmpty() -> {
                    PluginRepositoryEmptyState(
                        sources = sources,
                        onManageSources = {
                            navigator.navigate(PluginRepositorySourcesScreenDestination)
                        },
                    )
                }

                else -> LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    items(plugins, key = { "${it.sourceId}:${it.stableId}" }) { plugin ->
                        PluginRepositoryItem(
                            plugin = plugin,
                            source = sources.firstOrNull { it.id == plugin.sourceId },
                            installed = plugin.pluginId?.let(installed::get),
                            locale = locale,
                            installing = installingId == plugin.stableId,
                            onInstall = { installTarget = plugin },
                        )
                    }
                }
            }
        }
    }

    installTarget?.let { plugin ->
        YukiAlertDialog(
            onDismissRequest = { installTarget = null },
            title = { Text(stringResource(R.string.plugin_repository_install_title)) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(
                        stringResource(
                            R.string.plugin_repository_install_confirm,
                            plugin.name,
                            plugin.version,
                        )
                    )
                    Text(
                        stringResource(R.string.plugin_repository_unverified_warning),
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    installTarget = null
                    startDownload(plugin)
                }) { Text(stringResource(R.string.install)) }
            },
            dismissButton = {
                TextButton(onClick = { installTarget = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }

    downloadingPlugin?.let { plugin ->
        DownloadProgressDialog(
            title = stringResource(R.string.plugin_repository_downloading, plugin.name),
            message = stringResource(R.string.plugin_repository_download_message, plugin.version),
            progress = downloadProgress,
            onCancel = {
                downloadHandle?.cancel()
                downloadHandle = null
                downloadingPlugin = null
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun PluginRepositorySourcesScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<PluginRepositoryViewModel>()
    val sources by viewModel.sources.collectAsState()
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val snackbarHost = rememberSnackbarController()
    var showAddDialog by remember { mutableStateOf(false) }
    var deleteSource by remember { mutableStateOf<PluginRepositorySource?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.repository_sources)) },
                navigationIcon = {
                    IconButton(onClick = navigator::popBackStack) {
                        YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, null)
                    }
                },
                actions = {
                    IconButton(onClick = viewModel::refreshAll) {
                        YukiIcon(Icons.Outlined.Refresh, stringResource(R.string.refresh))
                    }
                },
                windowInsets = WindowInsets.safeDrawing.only(
                    WindowInsetsSides.Top + WindowInsetsSides.Horizontal
                ),
            )
        },
        floatingActionButton = {
            FloatingActionButton(
                onClick = { showAddDialog = true },
                shape = CircleShape,
            ) {
                YukiIcon(Icons.Outlined.Add, stringResource(R.string.repository_add_source))
            }
        },
        snackbarHost = { SnackbarHost(snackbarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        ),
    ) { innerPadding ->
        Column(Modifier.fillMaxSize().padding(innerPadding)) {
            if (viewModel.isRefreshing) LinearProgressIndicator(Modifier.fillMaxWidth())
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 96.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                items(sources, key = PluginRepositorySource::id) { source ->
                    val index = sources.indexOf(source)
                    PluginRepositorySourceItem(
                        source = source,
                        pluginCount = viewModel.pluginCount(source.id),
                        canMoveUp = index > 0,
                        canMoveDown = index < sources.lastIndex,
                        onToggle = { viewModel.setSourceEnabled(source.id, it) },
                        onMoveUp = { viewModel.moveSource(source.id, -1) },
                        onMoveDown = { viewModel.moveSource(source.id, 1) },
                        onRefresh = { viewModel.refreshSource(source.id) },
                        onDelete = { deleteSource = source },
                    )
                }
            }
        }
    }

    if (showAddDialog) {
        AddPluginRepositorySourceDialog(
            isAdding = viewModel.isAddingSource,
            onDismiss = { if (!viewModel.isAddingSource) showAddDialog = false },
            onAdd = { name, url ->
                viewModel.addSource(name, url) { result ->
                    result.onSuccess {
                        showAddDialog = false
                        scope.launch {
                            snackbarHost.showSnackbar(resources.getString(R.string.repository_source_added))
                        }
                    }.onFailure { error ->
                        scope.launch {
                            snackbarHost.showSnackbar(
                                error.message ?: resources.getString(R.string.repository_source_add_failed)
                            )
                        }
                    }
                }
            },
        )
    }

    deleteSource?.let { source ->
        YukiAlertDialog(
            onDismissRequest = { deleteSource = null },
            title = { Text(stringResource(R.string.repository_remove_source)) },
            text = { Text(stringResource(R.string.repository_remove_source_confirm, source.name)) },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.removeSource(source.id) { result ->
                        result.onFailure { error ->
                            Toast.makeText(
                                context,
                                error.message ?: resources.getString(R.string.repository_remove_source_failed),
                                Toast.LENGTH_LONG,
                            ).show()
                        }
                    }
                    deleteSource = null
                }) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = {
                TextButton(onClick = { deleteSource = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }
}

@Composable
private fun PluginRepositorySourceFilter(
    sources: List<PluginRepositorySource>,
    selectedSourceId: String?,
    onSelect: (String?) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        FilterChip(
            selected = selectedSourceId == null,
            onClick = { onSelect(null) },
            shape = CircleShape,
            label = { Text(stringResource(R.string.repository_all_sources)) },
        )
        sources.forEach { source ->
            FilterChip(
                selected = selectedSourceId == source.id,
                onClick = { onSelect(source.id) },
                shape = CircleShape,
                label = { Text(source.name, maxLines = 1) },
            )
        }
    }
}

@Composable
private fun PluginRepositoryItem(
    plugin: RepositoryPlugin,
    source: PluginRepositorySource?,
    installed: PluginInfo?,
    locale: java.util.Locale,
    installing: Boolean,
    onInstall: () -> Unit,
) {
    val description = localizedText(plugin.descriptions, locale, plugin.description)
    val uriHandler = LocalUriHandler.current
    PluginRepositoryCard {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Column {
                Text(
                    plugin.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Normal,
                )
                plugin.pluginId?.let { id ->
                    Text(
                        id,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            if (description.isNotBlank()) {
                Text(
                    description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                itemVerticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                source?.let { PluginRepositoryTag(it.name) }
                if (plugin.version.isNotBlank()) PluginRepositoryTag(plugin.version)
                plugin.author.takeIf(String::isNotBlank)?.let { PluginRepositoryTag(it) }
                installed?.let {
                    PluginRepositoryTag(stringResource(R.string.repository_installed_version, it.version))
                }
                Row(
                    modifier = Modifier.weight(1f).width(IntrinsicSize.Max),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                ) {
                    plugin.homepage?.let { url ->
                        IconButton(onClick = { uriHandler.openUri(url) }) {
                            YukiIcon(
                                Icons.AutoMirrored.Outlined.OpenInNew,
                                stringResource(R.string.repository_homepage),
                            )
                        }
                    }
                    Button(onClick = onInstall, enabled = !installing) {
                        if (installing) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(18.dp),
                                color = LocalContentColor.current,
                                strokeWidth = 2.dp,
                            )
                            Spacer(Modifier.width(8.dp))
                        }
                        Text(
                            when {
                                installed == null -> stringResource(R.string.install)
                                installed.version != plugin.version && plugin.version.isNotBlank() ->
                                    stringResource(R.string.plugin_repository_update)
                                else -> stringResource(R.string.repository_reinstall)
                            }
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PluginRepositoryEmptyState(
    sources: List<PluginRepositorySource>,
    onManageSources: () -> Unit,
) {
    val error = sources.firstNotNullOfOrNull(PluginRepositorySource::lastError)
    Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(
                stringResource(R.string.plugin_repository_empty),
                style = MaterialTheme.typography.bodyLarge,
            )
            if (!error.isNullOrBlank()) {
                Text(
                    error,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            OutlinedButton(onClick = onManageSources) {
                Text(stringResource(R.string.repository_manage_sources))
            }
        }
    }
}

@Composable
private fun PluginRepositorySourceItem(
    source: PluginRepositorySource,
    pluginCount: Int,
    canMoveUp: Boolean,
    canMoveDown: Boolean,
    onToggle: (Boolean) -> Unit,
    onMoveUp: () -> Unit,
    onMoveDown: () -> Unit,
    onRefresh: () -> Unit,
    onDelete: () -> Unit,
) {
    PluginRepositoryCard {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(source.name, style = MaterialTheme.typography.titleMedium)
                    Text(
                        source.url,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                YukiSwitch(checked = source.enabled, onCheckedChange = onToggle)
            }
            Text(
                stringResource(R.string.plugin_repository_plugin_count, pluginCount),
                style = MaterialTheme.typography.bodySmall,
            )
            source.lastSyncAt?.let { timestamp ->
                Text(
                    stringResource(
                        R.string.repository_last_sync,
                        DateFormat.getDateTimeInstance().format(Date(timestamp)),
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            source.lastError?.let { error ->
                Text(
                    error,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                IconButton(onClick = onMoveUp, enabled = canMoveUp) {
                    YukiIcon(Icons.Outlined.ArrowUpward, null)
                }
                IconButton(onClick = onMoveDown, enabled = canMoveDown) {
                    YukiIcon(Icons.Outlined.ArrowDownward, null)
                }
                IconButton(onClick = onRefresh) {
                    YukiIcon(Icons.Outlined.Refresh, stringResource(R.string.refresh))
                }
                if (!source.builtIn) {
                    IconButton(onClick = onDelete) {
                        YukiIcon(Icons.Outlined.Delete, stringResource(R.string.repository_remove_source))
                    }
                }
            }
        }
    }
}

@Composable
private fun AddPluginRepositorySourceDialog(
    isAdding: Boolean,
    onDismiss: () -> Unit,
    onAdd: (String, String) -> Unit,
) {
    var name by remember { mutableStateOf("") }
    var url by remember { mutableStateOf("") }
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.repository_add_source)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    stringResource(R.string.plugin_repository_unverified_warning),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.repository_source_name_optional)) },
                    singleLine = true,
                )
                OutlinedTextField(
                    value = url,
                    onValueChange = { url = it },
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.repository_source_url)) },
                    singleLine = true,
                )
                if (isAdding) LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onAdd(name, url) },
                enabled = url.isNotBlank() && !isAdding,
            ) { Text(stringResource(R.string.repository_add)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = !isAdding) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}

@Composable
private fun PluginRepositoryCard(content: @Composable ColumnScope.() -> Unit) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        colors = getCardColors(
            MaterialTheme.colorScheme.surfaceContainer
        ),
        elevation = getCardElevation(),
        shape = MaterialTheme.shapes.large,
        content = content,
    )
}

@Composable
private fun PluginRepositoryTag(text: String) {
    Surface(
        shape = CircleShape,
        color = MaterialTheme.colorScheme.secondaryContainer,
        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
    ) {
        Text(
            text,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
