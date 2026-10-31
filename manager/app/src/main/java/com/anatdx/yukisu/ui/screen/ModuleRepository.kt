package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import com.ramcosta.composedestinations.generated.destinations.ScriptRunScreenDestination
import com.anatdx.yukisu.ui.screen.scriptrun.ScriptRunArg
import android.text.format.Formatter
import android.widget.Toast
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.OpenInNew
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.ArrowDownward
import androidx.compose.material.icons.outlined.ArrowUpward
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.Inventory2
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.LockOpen
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material.icons.outlined.Source
import androidx.compose.material.icons.outlined.Storage
import androidx.compose.material.icons.outlined.Star
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LinearWavyProgressIndicator
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.anatdx.yukisu.R
import com.anatdx.yukisu.data.registry.RegistryUrls
import com.anatdx.yukisu.data.repository.CompatibilityStatus
import com.anatdx.yukisu.data.repository.InstalledModuleBinding
import com.anatdx.yukisu.data.repository.InstalledModuleState
import com.anatdx.yukisu.data.repository.MmrlRepositoryDirectoryEntry
import com.anatdx.yukisu.data.repository.MmrlRepositoryDirectoryState
import com.anatdx.yukisu.data.repository.RepositoryFormat
import com.anatdx.yukisu.data.repository.RepositoryModule
import com.anatdx.yukisu.data.repository.RepositoryModuleVersion
import com.anatdx.yukisu.data.repository.RepositorySource
import com.anatdx.yukisu.data.repository.displayName
import com.anatdx.yukisu.ui.component.SearchAppBar
import com.anatdx.yukisu.ui.component.DownloadProgressDialog
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getCardColors
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getCardElevation
import com.anatdx.yukisu.ui.util.download
import com.anatdx.yukisu.ui.util.DownloadHandle
import com.anatdx.yukisu.ui.util.DownloadProgress
import com.anatdx.yukisu.ui.viewmodel.ModuleRepositoryViewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ModuleRepositoryDetailScreenDestination
import com.ramcosta.composedestinations.generated.destinations.RepositorySourcesScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    containerColor: Color? = null,
    contentColor: Color? = null,
    shape: Shape? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val resolvedContainerColor = containerColor ?: MaterialTheme.colorScheme.surfaceContainer
    val colors = if (containerColor == null) {
        getCardColors(resolvedContainerColor)
    } else {
        CardDefaults.cardColors(
            containerColor = resolvedContainerColor,
            contentColor = contentColor ?: MaterialTheme.colorScheme.onSurface,
        )
    }
    val resolvedShape = shape ?: MaterialTheme.shapes.large

    if (onClick == null) {
        ElevatedCard(
            modifier = modifier,
            colors = colors,
            elevation = getCardElevation(),
            shape = resolvedShape,
            content = content,
        )
    } else {
        ElevatedCard(
            onClick = onClick,
            modifier = modifier,
            colors = colors,
            elevation = getCardElevation(),
            shape = resolvedShape,
            content = content,
        )
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    Button(
        onClick = onClick,
        shapes = ButtonDefaults.shapes(),
        modifier = modifier,
        enabled = enabled,
        content = content,
    )
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryOutlinedButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    OutlinedButton(
        onClick = onClick,
        shapes = ButtonDefaults.shapes(),
        modifier = modifier,
        enabled = enabled,
        content = content,
    )
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryTextButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    TextButton(
        onClick = onClick,
        shapes = ButtonDefaults.shapes(),
        modifier = modifier,
        enabled = enabled,
        content = content,
    )
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryIconButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable () -> Unit,
) {
    IconButton(
        onClick = onClick,
        shapes = IconButtonDefaults.shapes(),
        modifier = modifier,
        enabled = enabled,
        content = content,
    )
}

@Composable
private fun RepositoryFilterChip(
    selected: Boolean,
    onClick: () -> Unit,
    enabled: Boolean = true,
    label: @Composable () -> Unit,
) {
    FilterChip(
        selected = selected,
        onClick = onClick,
        enabled = enabled,
        shape = CircleShape,
        label = label,
    )
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryLinearProgress(modifier: Modifier = Modifier) {
    LinearWavyProgressIndicator(modifier = modifier)
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryLoadingIndicator(modifier: Modifier = Modifier) {
    LoadingIndicator(modifier = modifier)
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun RepositoryTopAppBar(
    title: @Composable () -> Unit,
    onBack: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior,
    actions: @Composable RowScope.() -> Unit = {},
) {
    val colorScheme = MaterialTheme.colorScheme
    val containerColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }
    val colors = getTopAppBarColors(
        containerColor = containerColor,
        scrolledContainerColor = containerColor,
    )
    val navigationIcon: @Composable () -> Unit = {
        RepositoryIconButton(onClick = onBack) {
            YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, null)
        }
    }

    YukiLargeTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        actions = actions,
        scrollBehavior = scrollBehavior,
        colors = colors,
        windowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun ModuleRepositoryScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<ModuleRepositoryViewModel>()
    val sources by viewModel.sources.collectAsState()
    val catalog by viewModel.catalog.collectAsState()
    val bindings by viewModel.bindings.collectAsState()
    val modules = remember(catalog, viewModel.search, viewModel.selectedSourceId, viewModel.installedModules) {
        viewModel.visibleModules(catalog)
    }
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    LaunchedEffect(Unit) {
        viewModel.refreshInstalledModules()
        if (catalog.isEmpty()) viewModel.refreshAll()
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = {
                    Text(
                        stringResource(R.string.module_repositories),
                        fontWeight = FontWeight.Normal,
                    )
                },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                onBackClick = navigator::popBackStack,
                dropdownContent = {
                    RepositoryIconButton(onClick = viewModel::refreshAll) {
                        YukiIcon(Icons.Outlined.Refresh, stringResource(R.string.refresh))
                    }
                    RepositoryIconButton(onClick = { navigator.navigate(RepositorySourcesScreenDestination) }) {
                        YukiIcon(Icons.Outlined.Storage, stringResource(R.string.repository_sources))
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        ),
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
        ) {
            if (viewModel.isRefreshing) {
                RepositoryLinearProgress(Modifier.fillMaxWidth())
            }
            SourceFilterRow(
                sources = sources.filter(RepositorySource::enabled),
                selectedSourceId = viewModel.selectedSourceId,
                onSelect = { viewModel.selectedSourceId = it },
            )
            if (modules.isEmpty() && viewModel.isRefreshing) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    RepositoryLoadingIndicator()
                }
            } else if (modules.isEmpty()) {
                EmptyRepositoryState(onManageSources = {
                    navigator.navigate(RepositorySourcesScreenDestination)
                })
            } else {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    items(modules, key = { "${it.sourceId}:${it.moduleId}" }) { module ->
                        RepositoryModuleCard(
                            module = module,
                            source = sources.firstOrNull { it.id == module.sourceId },
                            duplicateCount = viewModel.duplicateCount(module.moduleId, catalog),
                            installed = viewModel.installedModules[module.moduleId].takeUnless { module.isScript },
                            binding = bindings[module.moduleId].takeUnless { module.isScript },
                            compatibilityStatus = viewModel.compatibility(module).status,
                            onClick = {
                                navigator.navigate(
                                    ModuleRepositoryDetailScreenDestination(
                                        sourceId = module.sourceId,
                                        moduleId = module.moduleId,
                                    )
                                )
                            },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun SourceFilterRow(
    sources: List<RepositorySource>,
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
        RepositoryFilterChip(
            selected = selectedSourceId == null,
            onClick = { onSelect(null) },
            label = { Text(stringResource(R.string.repository_all_sources)) },
        )
        sources.forEach { source ->
            RepositoryFilterChip(
                selected = selectedSourceId == source.id,
                onClick = { onSelect(source.id) },
                label = { Text(source.name, maxLines = 1) },
            )
        }
    }
}

@Composable
private fun RepositoryModuleCard(
    module: RepositoryModule,
    source: RepositorySource?,
    duplicateCount: Int,
    installed: InstalledModuleState?,
    binding: InstalledModuleBinding?,
    compatibilityStatus: CompatibilityStatus,
    onClick: () -> Unit,
) {
    val updateAvailable = installed != null &&
        binding?.sourceId == module.sourceId &&
        binding.pinnedVersionCode == null &&
        (if (source?.format == RepositoryFormat.KERNEL_SU_V1) module.declaredVersionCode
        else module.declaredArtifact()?.versionCode ?: 0L) > installed.versionCode
    RepositoryCard(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Column {
                Text(
                    module.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Normal,
                )
                Text(
                    module.moduleId,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (module.description.isNotBlank()) {
                Text(
                    module.description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
                itemVerticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    module.declaredVersion.ifBlank { module.declaredVersionCode.toString() },
                    style = MaterialTheme.typography.labelMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    source?.name ?: stringResource(R.string.repository_removed_source),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (module.isScript) RepositoryTag(stringResource(R.string.module_repo_script_badge))
                if (installed != null) {
                    RepositoryTag(stringResource(R.string.repository_installed_version, installed.version))
                }
                if (duplicateCount > 1) {
                    RepositoryTag(stringResource(R.string.repository_source_count, duplicateCount))
                }
                module.stars?.let { stars ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Icon(Icons.Outlined.Star, null, modifier = Modifier.size(16.dp))
                        Text(stars.toString(), style = MaterialTheme.typography.labelMedium)
                    }
                }
                if (updateAvailable) {
                    RepositoryTag(stringResource(R.string.repository_update_available))
                } else if (binding?.pinnedVersionCode != null && binding.sourceId == module.sourceId) {
                    RepositoryTag(stringResource(R.string.repository_pinned))
                }
                if (binding?.sourceId == module.sourceId) {
                    Icon(
                        Icons.Outlined.Link,
                        contentDescription = stringResource(R.string.repository_bound_source),
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(18.dp),
                    )
                }
                if (compatibilityStatus == CompatibilityStatus.INCOMPATIBLE) {
                    Icon(
                        Icons.Outlined.Warning,
                        contentDescription = stringResource(R.string.repository_incompatible),
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier.size(18.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun RepositoryTag(text: String) {
    Surface(
        color = MaterialTheme.colorScheme.secondaryContainer,
        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
        shape = MaterialTheme.shapes.extraSmall,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun EmptyRepositoryState(onManageSources: () -> Unit) {
    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(Icons.Outlined.Inventory2, null, Modifier.size(56.dp))
            Spacer(Modifier.height(16.dp))
            Text(stringResource(R.string.repository_empty), style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(12.dp))
            RepositoryButton(onClick = onManageSources) {
                Text(stringResource(R.string.repository_manage_sources))
            }
        }
    }
}

private enum class RepositorySourcesSection {
    SOURCES,
    MMRL,
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun RepositorySourcesScreen(navigator: DestinationsNavigator) {
    val viewModel = viewModel<ModuleRepositoryViewModel>()
    val sources by viewModel.sources.collectAsState()
    val mmrlDirectory by viewModel.mmrlDirectory.collectAsState()
    var section by remember { mutableStateOf(RepositorySourcesSection.SOURCES) }
    var mmrlSearch by remember { mutableStateOf("") }
    var mmrlAddError by remember { mutableStateOf<String?>(null) }
    var showAddDialog by remember { mutableStateOf(false) }
    var deleteSource by remember { mutableStateOf<RepositorySource?>(null) }
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    LaunchedEffect(section, mmrlDirectory.entries.isEmpty()) {
        if (section == RepositorySourcesSection.MMRL && mmrlDirectory.entries.isEmpty()) {
            viewModel.refreshMmrlDirectory()
        }
    }

    Scaffold(
        topBar = {
            RepositoryTopAppBar(
                title = {
                    Text(
                        stringResource(R.string.repository_sources),
                        fontWeight = FontWeight.Normal,
                    )
                },
                onBack = navigator::popBackStack,
                scrollBehavior = scrollBehavior,
                actions = {
                    RepositoryIconButton(
                        onClick = if (section == RepositorySourcesSection.SOURCES) {
                            viewModel::refreshAll
                        } else {
                            viewModel::refreshMmrlDirectory
                        },
                    ) {
                        YukiIcon(Icons.Outlined.Refresh, stringResource(R.string.refresh))
                    }
                },
            )
        },
        floatingActionButton = {
            if (section == RepositorySourcesSection.SOURCES) {
                FloatingActionButton(
                    onClick = { showAddDialog = true },
                    shape = CircleShape,
                ) {
                    YukiIcon(Icons.Outlined.Add, stringResource(R.string.repository_add_source))
                }
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        ),
    ) { innerPadding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
        ) {
            val sectionRefreshing = if (section == RepositorySourcesSection.SOURCES) {
                viewModel.isRefreshing
            } else {
                viewModel.isRefreshingMmrlDirectory
            }
            if (sectionRefreshing) RepositoryLinearProgress(Modifier.fillMaxWidth())
            RepositorySourcesSectionRow(
                section = section,
                onSectionChange = {
                    section = it
                    mmrlAddError = null
                },
            )
            if (section == RepositorySourcesSection.SOURCES) {
                LazyColumn(
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    items(sources, key = RepositorySource::id) { source ->
                        RepositorySourceCard(
                            source = source,
                            moduleCount = viewModel.moduleCount(source.id),
                            onEnabledChange = { viewModel.setSourceEnabled(source.id, it) },
                            onRefresh = { viewModel.refreshSource(source.id) },
                            onMoveUp = { viewModel.moveSource(source.id, -1) },
                            onMoveDown = { viewModel.moveSource(source.id, 1) },
                            onDelete = { deleteSource = source },
                        )
                    }
                }
            } else {
                MmrlDirectorySection(
                    state = mmrlDirectory,
                    search = mmrlSearch,
                    onSearchChange = { mmrlSearch = it },
                    isRefreshing = viewModel.isRefreshingMmrlDirectory,
                    isAddingSource = viewModel.isAddingSource,
                    addingUrl = viewModel.addingMmrlRepositoryUrl,
                    addError = mmrlAddError,
                    sourceForUrl = viewModel::sourceForUrl,
                    onAdd = { entry ->
                        mmrlAddError = null
                        viewModel.addMmrlRepository(entry) { result ->
                            mmrlAddError = result.exceptionOrNull()?.message
                        }
                    },
                    onRetry = viewModel::refreshMmrlDirectory,
                )
            }
        }
    }

    if (showAddDialog) {
        AddRepositorySourceDialog(
            isAdding = viewModel.isAddingSource,
            onDismiss = { if (!viewModel.isAddingSource) showAddDialog = false },
            onAdd = { name, url, format, result ->
                viewModel.addSource(name, url, format) { addResult ->
                    result(addResult.exceptionOrNull()?.message)
                    if (addResult.isSuccess) showAddDialog = false
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
                RepositoryTextButton(onClick = {
                    viewModel.removeSource(source.id)
                    deleteSource = null
                }) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = {
                RepositoryTextButton(onClick = { deleteSource = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }
}

@Composable
private fun RepositorySourcesSectionRow(
    section: RepositorySourcesSection,
    onSectionChange: (RepositorySourcesSection) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        RepositoryFilterChip(
            selected = section == RepositorySourcesSection.SOURCES,
            onClick = { onSectionChange(RepositorySourcesSection.SOURCES) },
            label = { Text(stringResource(R.string.repository_my_sources)) },
        )
        RepositoryFilterChip(
            selected = section == RepositorySourcesSection.MMRL,
            onClick = { onSectionChange(RepositorySourcesSection.MMRL) },
            label = { Text(stringResource(R.string.repository_mmrl_directory)) },
        )
    }
}

@Composable
private fun MmrlDirectorySection(
    state: MmrlRepositoryDirectoryState,
    search: String,
    onSearchChange: (String) -> Unit,
    isRefreshing: Boolean,
    isAddingSource: Boolean,
    addingUrl: String?,
    addError: String?,
    sourceForUrl: (String) -> RepositorySource?,
    onAdd: (MmrlRepositoryDirectoryEntry) -> Unit,
    onRetry: () -> Unit,
) {
    val uriHandler = LocalUriHandler.current
    val query = search.trim()
    val visibleEntries = remember(state.entries, query) {
        state.entries.filter { entry ->
            query.isEmpty() ||
                entry.name.contains(query, ignoreCase = true) ||
                entry.url.contains(query, ignoreCase = true) ||
                entry.description?.contains(query, ignoreCase = true) == true
        }
    }

    Column(Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = search,
            onValueChange = onSearchChange,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            label = { Text(stringResource(R.string.repository_mmrl_search)) },
            leadingIcon = { Icon(Icons.Outlined.Search, null) },
            singleLine = true,
        )

        if (state.entries.isEmpty() && isRefreshing) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                RepositoryLoadingIndicator()
            }
            return@Column
        }

        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                RepositoryNotice(
                    title = stringResource(R.string.repository_mmrl_directory),
                    message = pluralStringResource(
                        R.plurals.repository_mmrl_directory_description,
                        state.entries.size,
                        state.entries.size,
                    ),
                    error = false,
                )
            }
            state.lastSyncAt?.let { syncTime ->
                item {
                    Text(
                        stringResource(
                            R.string.repository_last_sync,
                            DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                                .format(Date(syncTime)),
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            val error = addError ?: state.lastError
            if (error != null) {
                item {
                    RepositoryCard(
                        modifier = Modifier.fillMaxWidth(),
                        containerColor = MaterialTheme.colorScheme.errorContainer,
                        contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    ) {
                        Column(
                            Modifier.padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Text(error, style = MaterialTheme.typography.bodyMedium)
                            if (state.entries.isEmpty()) {
                                RepositoryTextButton(onClick = onRetry) {
                                    Text(stringResource(R.string.repository_retry))
                                }
                            }
                        }
                    }
                }
            }
            if (visibleEntries.isEmpty() && state.entries.isNotEmpty()) {
                item {
                    Text(
                        stringResource(R.string.repository_mmrl_no_results),
                        modifier = Modifier.fillMaxWidth().padding(vertical = 32.dp),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            items(visibleEntries, key = MmrlRepositoryDirectoryEntry::url) { entry ->
                MmrlDirectoryRepositoryCard(
                    entry = entry,
                    addedSource = sourceForUrl(entry.url),
                    isAdding = addingUrl == entry.url,
                    addEnabled = !isAddingSource,
                    onAdd = { onAdd(entry) },
                    onOpen = { uriHandler.openUri(entry.url) },
                )
            }
        }
    }
}

@Composable
private fun MmrlDirectoryRepositoryCard(
    entry: MmrlRepositoryDirectoryEntry,
    addedSource: RepositorySource?,
    isAdding: Boolean,
    addEnabled: Boolean,
    onAdd: () -> Unit,
    onOpen: () -> Unit,
) {
    RepositoryCard(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(
                        entry.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Normal,
                    )
                    entry.modulesCount?.let { count ->
                        Text(
                            stringResource(R.string.repository_mmrl_module_count, count),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                }
                RepositoryIconButton(onClick = onOpen) {
                    Icon(
                        Icons.AutoMirrored.Outlined.OpenInNew,
                        stringResource(R.string.repository_homepage),
                    )
                }
            }
            entry.description?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                entry.url,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            entry.updatedAt?.let { updatedAt ->
                Text(
                    stringResource(
                        R.string.repository_mmrl_updated,
                        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                            .format(Date(updatedAt)),
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (addedSource != null) {
                    RepositoryOutlinedButton(onClick = {}, enabled = false) {
                        Icon(Icons.Outlined.CheckCircle, null)
                        Spacer(Modifier.width(8.dp))
                        Text(
                            if (addedSource.enabled) {
                                stringResource(R.string.repository_mmrl_added)
                            } else {
                                stringResource(R.string.repository_mmrl_added_disabled)
                            }
                        )
                    }
                } else {
                    RepositoryButton(
                        onClick = onAdd,
                        enabled = addEnabled && !isAdding,
                    ) {
                        if (isAdding) {
                            RepositoryLoadingIndicator(Modifier.size(20.dp))
                            Spacer(Modifier.width(8.dp))
                        } else {
                            Icon(Icons.Outlined.Add, null)
                            Spacer(Modifier.width(8.dp))
                        }
                        Text(stringResource(R.string.repository_add))
                    }
                }
            }
        }
    }
}

@Composable
private fun RepositorySourceCard(
    source: RepositorySource,
    moduleCount: Int,
    onEnabledChange: (Boolean) -> Unit,
    onRefresh: () -> Unit,
    onMoveUp: () -> Unit,
    onMoveDown: () -> Unit,
    onDelete: () -> Unit,
) {
    RepositoryCard(
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        source.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Normal,
                    )
                    Text(
                        "${source.format.displayName()} · $moduleCount ${stringResource(R.string.module)}",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
                YukiSwitch(checked = source.enabled, onCheckedChange = onEnabledChange)
            }
            Text(
                source.url,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            source.lastSyncAt?.let {
                Text(
                    stringResource(
                        R.string.repository_last_sync,
                        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(it))
                    ),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            source.lastError?.let {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        Icons.Outlined.ErrorOutline,
                        null,
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier.size(18.dp),
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                RepositoryIconButton(onClick = onMoveUp) { Icon(Icons.Outlined.ArrowUpward, null) }
                RepositoryIconButton(onClick = onMoveDown) { Icon(Icons.Outlined.ArrowDownward, null) }
                RepositoryIconButton(onClick = onRefresh) { Icon(Icons.Outlined.Refresh, null) }
                if (!source.builtIn) {
                    RepositoryIconButton(onClick = onDelete) {
                        Icon(Icons.Outlined.Delete, stringResource(R.string.delete))
                    }
                }
            }
        }
    }
}

@Composable
private fun AddRepositorySourceDialog(
    isAdding: Boolean,
    onDismiss: () -> Unit,
    onAdd: (String, String, RepositoryFormat, (String?) -> Unit) -> Unit,
) {
    var name by remember { mutableStateOf("") }
    var url by remember { mutableStateOf("") }
    var format by remember { mutableStateOf(RepositoryFormat.AUTO) }
    var error by remember { mutableStateOf<String?>(null) }

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.repository_add_source)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    stringResource(R.string.repository_unverified_warning),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium,
                )
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    label = { Text(stringResource(R.string.repository_source_name_optional)) },
                    singleLine = true,
                    enabled = !isAdding,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = url,
                    onValueChange = { url = it; error = null },
                    label = { Text(stringResource(R.string.repository_source_url)) },
                    singleLine = true,
                    enabled = !isAdding,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    RepositoryFormat.entries.forEach { option ->
                        RepositoryFilterChip(
                            selected = format == option,
                            onClick = { format = option },
                            label = { Text(option.displayName()) },
                            enabled = !isAdding,
                        )
                    }
                }
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                if (isAdding) RepositoryLinearProgress(Modifier.fillMaxWidth())
            }
        },
        confirmButton = {
            RepositoryTextButton(
                onClick = { onAdd(name, url, format) { error = it } },
                enabled = url.isNotBlank() && !isAdding,
            ) { Text(stringResource(R.string.repository_add)) }
        },
        dismissButton = {
            RepositoryTextButton(onClick = onDismiss, enabled = !isAdding) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun ModuleRepositoryDetailScreen(
    navigator: DestinationsNavigator,
    sourceId: String,
    moduleId: String,
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current
    val downloadErrorText = stringResource(R.string.module_download_error)
    val viewModel = viewModel<ModuleRepositoryViewModel>()
    val sources by viewModel.sources.collectAsState()
    val catalog by viewModel.catalog.collectAsState()
    val bindings by viewModel.bindings.collectAsState()
    val module = catalog.firstOrNull { it.sourceId == sourceId && it.moduleId == moduleId }
        ?: viewModel.module(sourceId, moduleId)
    val source = sources.firstOrNull { it.id == sourceId }
    val installed = viewModel.installedModules[moduleId].takeUnless { module?.isScript == true }
    val binding = bindings[moduleId].takeUnless { module?.isScript == true }
    val compatibility = module?.let(viewModel::compatibility)
    val missingDependencies = module?.dependencies?.filterNot {
        viewModel.installedModules.containsKey(it)
    }.orEmpty()
    var selectedVersion by remember { mutableStateOf<RepositoryModuleVersion?>(null) }
    var downloadingVersion by remember { mutableStateOf<RepositoryModuleVersion?>(null) }
    var downloadProgress by remember { mutableStateOf(DownloadProgress()) }
    var downloadHandle by remember { mutableStateOf<DownloadHandle?>(null) }
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    LaunchedEffect(sourceId, moduleId) {
        viewModel.refreshInstalledModules()
        viewModel.loadModuleDetails(sourceId, moduleId)
    }

    Scaffold(
        topBar = {
            RepositoryTopAppBar(
                title = {
                    Text(
                        module?.name ?: moduleId,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        fontWeight = FontWeight.Normal,
                    )
                },
                onBack = navigator::popBackStack,
                scrollBehavior = scrollBehavior,
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        ),
    ) { innerPadding ->
        if (module == null) {
            Column(Modifier.fillMaxSize().padding(innerPadding), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(stringResource(R.string.repository_module_missing))
                RepositoryTextButton(onClick = { viewModel.refreshSource(sourceId) }) {
                    Text(stringResource(R.string.repository_retry))
                }
            }
            return@Scaffold
        }
        val moduleCompatibility = checkNotNull(compatibility)
        val alternatives = catalog.filter { it.moduleId == moduleId && it.sourceId != sourceId }
        val currentArtifact = module.declaredArtifact()
        val orderedVersions = buildList {
            if (currentArtifact != null) add(currentArtifact)
            addAll(
                module.versions.withIndex()
                    .filterNot { it.value == currentArtifact }
                    .sortedWith(
                        compareByDescending<IndexedValue<RepositoryModuleVersion>> {
                            it.value.timestamp ?: Long.MIN_VALUE
                        }.thenBy(IndexedValue<RepositoryModuleVersion>::index)
                    )
                    .map(IndexedValue<RepositoryModuleVersion>::value)
            )
        }

        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                RepositoryCard(
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(
                            module.name,
                            style = MaterialTheme.typography.headlineSmall,
                            fontWeight = FontWeight.Normal,
                        )
                        Text("ID: ${module.moduleId}", style = MaterialTheme.typography.bodySmall)
                        if (module.author.isNotBlank()) Text(module.author)
                        module.stars?.let { stars ->
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                Icon(Icons.Outlined.Star, null, modifier = Modifier.size(16.dp))
                                Text(stars.toString(), style = MaterialTheme.typography.bodySmall)
                            }
                        }
                        if (module.description.isNotBlank()) {
                            Text(module.description, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        HorizontalDivider()
                        Text(
                            source?.name ?: stringResource(R.string.repository_removed_source),
                            color = MaterialTheme.colorScheme.primary,
                        )
                        installed?.let {
                            Text(stringResource(R.string.repository_installed_version, it.version))
                        }
                        if (binding != null) {
                            val boundName = sources.firstOrNull { it.id == binding.sourceId }?.name
                                ?: stringResource(R.string.repository_removed_source)
                            Text(stringResource(R.string.repository_bound_to, boundName))
                            if (binding.sourceId == sourceId) {
                                RepositoryOutlinedButton(onClick = {
                                    viewModel.setPinned(
                                        moduleId,
                                        if (binding.pinnedVersionCode == null) installed?.versionCode else null,
                                    )
                                }) {
                                    Icon(
                                        if (binding.pinnedVersionCode == null) Icons.Outlined.Lock else Icons.Outlined.LockOpen,
                                        null,
                                    )
                                    Spacer(Modifier.width(8.dp))
                                    Text(
                                        if (binding.pinnedVersionCode == null) {
                                            stringResource(R.string.repository_pin_version)
                                        } else {
                                            stringResource(R.string.repository_unpin_version)
                                        }
                                    )
                                }
                            }
                        }
                        val boundSourceExists = binding?.let { bound ->
                            sources.any { it.id == bound.sourceId }
                        } ?: false
                        val canAdoptSource = installed != null &&
                            binding?.sourceId != sourceId &&
                            !boundSourceExists &&
                            module.versions.any { it.versionCode == installed.versionCode }
                        if (canAdoptSource) {
                            RepositoryOutlinedButton(
                                onClick = {
                                    viewModel.bindInstalledModule(sourceId, installed)
                                },
                            ) {
                                Icon(Icons.Outlined.Link, null)
                                Spacer(Modifier.width(8.dp))
                                Text(stringResource(R.string.repository_bind_existing))
                            }
                        }
                    }
                }
            }

            if (moduleCompatibility.status == CompatibilityStatus.INCOMPATIBLE) {
                item {
                    RepositoryNotice(
                        title = stringResource(R.string.repository_incompatible),
                        message = moduleCompatibility.reasons.joinToString("\n"),
                        error = true,
                    )
                }
            }
            if (viewModel.isLoadingDetails) {
                item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
            }
            viewModel.detailError?.let { error ->
                item {
                    Text(error, color = MaterialTheme.colorScheme.error)
                    RepositoryTextButton(onClick = { viewModel.loadModuleDetails(sourceId, moduleId) }) {
                        Text(stringResource(R.string.repository_retry))
                    }
                }
            }
            if (!module.hasConsistentLatestArtifact &&
                (source?.format != RepositoryFormat.KERNEL_SU_V1 || module.detailsLoaded)
            ) {
                item {
                    RepositoryNotice(
                        title = stringResource(R.string.repository_inconsistent_index),
                        message = stringResource(
                            R.string.repository_inconsistent_index_description,
                            module.declaredVersion,
                            module.declaredVersionCode,
                        ),
                        error = true,
                    )
                }
            }
            if (missingDependencies.isNotEmpty()) {
                item {
                    RepositoryNotice(
                        title = stringResource(R.string.repository_missing_dependencies),
                        message = missingDependencies.joinToString(", "),
                        error = true,
                    )
                }
            }
            if (module.dependencies.isNotEmpty()) {
                item {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.repository_dependencies), style = MaterialTheme.typography.titleMedium)
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            module.dependencies.forEach { RepositoryTag(it) }
                        }
                    }
                }
            }
            if (alternatives.isNotEmpty()) {
                item {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.repository_other_sources), style = MaterialTheme.typography.titleMedium)
                        alternatives.forEach { alternative ->
                            RepositoryOutlinedButton(
                                onClick = {
                                    navigator.navigate(
                                        ModuleRepositoryDetailScreenDestination(
                                            alternative.sourceId,
                                            alternative.moduleId,
                                        )
                                    )
                                },
                                modifier = Modifier.fillMaxWidth(),
                            ) {
                                Text(
                                    sources.firstOrNull { it.id == alternative.sourceId }?.name
                                        ?: stringResource(R.string.repository_removed_source),
                                    modifier = Modifier.weight(1f),
                                )
                                Text(alternative.declaredVersion)
                            }
                        }
                    }
                }
            }
            if (listOf(module.homepage, module.support, module.sourceUrl, module.readme).any { !it.isNullOrBlank() }) {
                item {
                    Row(
                        modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        module.homepage?.let { RepositoryLinkButton(stringResource(R.string.repository_homepage), it, uriHandler::openUri) }
                        module.sourceUrl?.let { RepositoryLinkButton(stringResource(R.string.repository_source_code), it, uriHandler::openUri) }
                        module.support?.let { RepositoryLinkButton(stringResource(R.string.repository_support), it, uriHandler::openUri) }
                        module.readme?.let { RepositoryLinkButton("README", it, uriHandler::openUri) }
                    }
                }
            }
            item {
                Text(stringResource(R.string.repository_versions), style = MaterialTheme.typography.titleLarge)
            }
            items(orderedVersions, key = { "${it.versionCode}:${it.downloadUrl}" }) { version ->
                VersionCard(
                    version = version,
                    isDeclared = version.versionCode == module.declaredVersionCode &&
                        version.version == module.declaredVersion,
                    isInstalled = installed?.versionCode == version.versionCode,
                    isScript = module.isScript,
                    onInstall = { selectedVersion = version },
                )
            }
        }
    }

    selectedVersion?.let { version ->
        val switchingSource = binding != null && binding.sourceId != sourceId
        YukiAlertDialog(
            onDismissRequest = { selectedVersion = null },
            title = { Text(stringResource(if (module?.isScript == true) R.string.script_run_title else R.string.module_install)) },
            icon = { Icon(Icons.Outlined.Warning, null) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(if (module?.isScript == true) "${module.name} · ${version.version}" else stringResource(R.string.repository_install_version_confirm, version.version, version.versionCode))
                    if (source?.builtIn != true) {
                        Text(
                            stringResource(R.string.repository_unverified_warning),
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                    if (switchingSource) {
                        Text(stringResource(R.string.repository_switch_source_warning))
                    }
                    if (missingDependencies.isNotEmpty()) {
                        Text(
                            "${stringResource(R.string.repository_missing_dependencies)}: ${missingDependencies.joinToString()}",
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                    if (compatibility?.status == CompatibilityStatus.INCOMPATIBLE) {
                        Text(
                            compatibility.reasons.joinToString("\n"),
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                }
            },
            confirmButton = {
                RepositoryTextButton(onClick = {
                    val safeName = "${moduleId}_${version.versionCode}.zip"
                        .replace(Regex("[^A-Za-z0-9._-]"), "_")
                    selectedVersion = null
                    downloadingVersion = version
                    downloadProgress = DownloadProgress()
                    downloadHandle = download(
                        context = context,
                        url = version.downloadUrl,
                        fileName = safeName,
                        description = module?.name ?: moduleId,
                        expected = version.expectedFile(),
                        onDownloaded = { uri ->
                            downloadingVersion = null
                            downloadHandle = null
                            if (module?.isScript == true) {
                                navigator.navigate(
                                    ScriptRunScreenDestination(
                                        ScriptRunArg(
                                            uri, registryId = moduleId, displayName = module.name,
                                        )
                                    )
                                )
                            } else navigator.navigate(
                                FlashScreenDestination(
                                    FlashIt.FlashModule(
                                        uri = uri,
                                        repositorySourceId = sourceId,
                                        repositoryModuleId = moduleId,
                                        repositoryVersionCode = version.versionCode,
                                        repositoryVersionName = version.version,
                                    )
                                )
                            )
                        },
                        onProgress = { progress -> downloadProgress = progress },
                        onError = { error ->
                            downloadingVersion = null
                            downloadHandle = null
                            Toast.makeText(
                                context,
                                "$downloadErrorText: $error",
                                Toast.LENGTH_LONG,
                            ).show()
                        },
                    )
                }) { Text(stringResource(if (module?.isScript == true) R.string.script_run else R.string.install)) }
            },
            dismissButton = {
                RepositoryTextButton(onClick = { selectedVersion = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }

    downloadingVersion?.let { version ->
        DownloadProgressDialog(
            title = stringResource(R.string.module_downloading, module?.name ?: moduleId),
            message = stringResource(
                R.string.repository_install_version_confirm,
                version.version,
                version.versionCode,
            ),
            progress = downloadProgress,
            onCancel = {
                downloadHandle?.cancel()
                downloadHandle = null
                downloadingVersion = null
            },
        )
    }
}

@Composable
private fun RepositoryNotice(title: String, message: String, error: Boolean) {
    RepositoryCard(
        containerColor = if (error) MaterialTheme.colorScheme.errorContainer
        else MaterialTheme.colorScheme.secondaryContainer,
        contentColor = if (error) MaterialTheme.colorScheme.onErrorContainer
        else MaterialTheme.colorScheme.onSecondaryContainer,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.Top) {
            Icon(if (error) Icons.Outlined.Warning else Icons.Outlined.CheckCircle, null)
            Spacer(Modifier.width(12.dp))
            Column {
                Text(
                    title,
                    fontWeight = FontWeight.Normal,
                )
                Text(message, style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

@Composable
private fun RepositoryLinkButton(label: String, url: String, open: (String) -> Unit) {
    RepositoryOutlinedButton(onClick = { open(url) }) {
        Icon(Icons.Outlined.Source, null)
        Spacer(Modifier.width(6.dp))
        Text(label)
    }
}

@Composable
private fun VersionCard(
    version: RepositoryModuleVersion,
    isDeclared: Boolean,
    isInstalled: Boolean,
    isScript: Boolean,
    onInstall: () -> Unit,
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current
    RepositoryCard(
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            modifier = Modifier.padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        version.version.ifBlank { version.versionCode.toString() },
                        fontWeight = FontWeight.Normal,
                    )
                    if (isDeclared) RepositoryTag(stringResource(R.string.repository_current_release))
                    if (isInstalled) RepositoryTag(stringResource(R.string.repository_installed))
                }
                Text("versionCode ${version.versionCode}", style = MaterialTheme.typography.bodySmall)
                version.assetName?.takeIf { it.isNotBlank() }?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                version.size?.let {
                    Text(Formatter.formatFileSize(context, it), style = MaterialTheme.typography.bodySmall)
                }
                version.changelogUrl?.takeIf(RegistryUrls::isWebUrl)?.let { url ->
                    RepositoryTextButton(onClick = { uriHandler.openUri(url) }) {
                        Text(stringResource(R.string.module_changelog))
                    }
                }
            }
            RepositoryButton(onClick = onInstall) {
                Text(
                    if (isScript) stringResource(R.string.script_run)
                    else if (isInstalled) stringResource(R.string.repository_reinstall)
                    else stringResource(R.string.install)
                )
            }
        }
    }
}
