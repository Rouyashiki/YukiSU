package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.motion.yukiAnimateContentSize
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.*
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import android.os.Process.myUid
import androidx.core.content.edit

private val SPACING_SMALL = 4.dp
private val SPACING_MEDIUM = 8.dp
private val SPACING_LARGE = 16.dp

private const val PAGE_SIZE = 10000
private const val MAX_TOTAL_LOGS = 100000

private const val DEFAULT_LOG_PATH = ""

data class LogEntry(
    val timestamp: String,
    val type: LogType,
    val uid: String,
    val comm: String,
    val details: String,
    val pid: String,
    val rawLine: String,
    val isViewerSelfNoise: Boolean = false,
    val isCurrentManagerCommand: Boolean = false,
)

data class LogPageInfo(
    val currentPage: Int = 0,
    val totalPages: Int = 0,
    val totalLogs: Int = 0,
    val hasMore: Boolean = false,
    val currentFileName: String = ""
)

enum class LogType(val displayName: String, val color: Color) {
    ROOT_EXECVE("ROOT_EXECVE", Color(0xFF3F51B5)),
    SUCOMPAT("SUCOMPAT", Color(0xFF009688)),
    IOCTL_GRANT_ROOT("IOCTL_GRANT_ROOT", Color(0xFF8BC34A)),
    DAEMON_EVENT("DAEMON_EVENT", Color(0xFF795548)),
    DROPPED("DROPPED", Color(0xFFE53935)),
    UNKNOWN("UNKNOWN", Color(0xFF757575))
}

enum class LogExclType(val displayName: String, val color: Color) {
    CURRENT_APP("Current app", Color(0xFF9E9E9E)),
    LOG_VIEWER_SELF_REFRESH("Log viewer self-refresh", Color(0xFF607D8B)),
}

private fun saveExcludedSubTypes(context: Context, types: Set<LogExclType>) {
    val prefs = context.getSharedPreferences("sulog", Context.MODE_PRIVATE)
    val nameSet = types.map { it.name }.toSet()
    prefs.edit {
        putStringSet("excluded_subtypes", nameSet)
        putInt("excluded_subtypes_version", 1)
    }
}

private fun loadExcludedSubTypes(context: Context): Set<LogExclType> {
    val prefs = context.getSharedPreferences("sulog", Context.MODE_PRIVATE)
    val nameSet = prefs.getStringSet("excluded_subtypes", emptySet()) ?: emptySet()
    val loaded = nameSet.mapNotNull { name ->
        LogExclType.entries.firstOrNull { it.name == name }
    }.toSet()
    val prefsVersion = prefs.getInt("excluded_subtypes_version", 0)
    return if (prefsVersion < 1) {
        loaded + LogExclType.LOG_VIEWER_SELF_REFRESH
    } else {
        loaded
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun LogViewerScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val snackBarHost = LocalSnackbarHost.current
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var logEntries by remember { mutableStateOf<List<LogEntry>>(emptyList()) }
    var isLoading by remember { mutableStateOf(false) }
    var filterType by rememberSaveable { mutableStateOf<LogType?>(null) }
    var searchQuery by rememberSaveable { mutableStateOf("") }
    var showSearchBar by rememberSaveable { mutableStateOf(false) }
    var pageInfo by remember { mutableStateOf(LogPageInfo()) }
    var lastLogFileHash by remember { mutableStateOf("") }
    var currentLogPath by remember { mutableStateOf(DEFAULT_LOG_PATH) }
    var activeLogPath by remember { mutableStateOf(DEFAULT_LOG_PATH) }
    var logSources by remember { mutableStateOf<List<SulogLogSource>>(emptyList()) }
    val currentUid = remember { myUid().toString() }

    val initialExcluded = remember {
        loadExcludedSubTypes(context)
    }

    var excludedSubTypes by rememberSaveable { mutableStateOf(initialExcluded) }

    LaunchedEffect(excludedSubTypes) {
        saveExcludedSubTypes(context, excludedSubTypes)
    }

    val filteredEntries = remember(
        logEntries, filterType, searchQuery, excludedSubTypes
    ) {
        logEntries.filter { entry ->
            val matchesSearch = searchQuery.isEmpty() ||
                    entry.comm.contains(searchQuery, ignoreCase = true) ||
                    entry.details.contains(searchQuery, ignoreCase = true) ||
                    entry.uid.contains(searchQuery, ignoreCase = true) ||
                    entry.rawLine.contains(searchQuery, ignoreCase = true)

            // 排除本应用
            if (
                LogExclType.CURRENT_APP in excludedSubTypes &&
                (entry.uid == currentUid || entry.isCurrentManagerCommand)
            ) {
                return@filter false
            }

            if (LogExclType.LOG_VIEWER_SELF_REFRESH in excludedSubTypes && entry.isViewerSelfNoise) {
                return@filter false
            }

            // 普通类型筛选
            val matchesFilter = filterType == null || entry.type == filterType
            matchesFilter && matchesSearch
        }
    }
    val currentCleanAction = remember(currentLogPath, activeLogPath) {
        if (currentLogPath.isBlank()) {
            SulogLogSourceCleanAction.Clear
        } else {
            resolveSulogSourceCleanAction(currentLogPath, activeLogPath)
        }
    }
    val currentManageTitle = stringResource(
        if (currentCleanAction == SulogLogSourceCleanAction.Clear) {
            R.string.log_viewer_clear_logs
        } else {
            R.string.log_viewer_delete_log
        }
    )
    val currentManageConfirmMessage = stringResource(
        if (currentCleanAction == SulogLogSourceCleanAction.Clear) {
            R.string.log_viewer_clear_logs_confirm
        } else {
            R.string.log_viewer_delete_log_confirm
        }
    )
    val currentManageSuccessMessage = stringResource(
        if (currentCleanAction == SulogLogSourceCleanAction.Clear) {
            R.string.log_viewer_logs_cleared
        } else {
            R.string.log_viewer_log_deleted
        }
    )

    val loadingDialog = rememberLoadingDialog()
    val confirmDialog = rememberConfirmDialog()

    val loadPage: (Int, Boolean) -> Unit = { page, forceRefresh ->
        scope.launch {
            if (isLoading) return@launch

            isLoading = true
            try {
                loadLogsWithPagination(
                    page,
                    forceRefresh,
                    lastLogFileHash,
                    currentLogPath,
                    activeLogPath
                ) { entries, newPageInfo, newHash, newPath, newSources, newActivePath ->
                    logEntries = if (page == 0 || forceRefresh) {
                        entries
                    } else {
                        logEntries + entries
                    }
                    pageInfo = newPageInfo
                    lastLogFileHash = newHash
                    currentLogPath = newPath
                    logSources = newSources
                    activeLogPath = newActivePath
                }
            } finally {
                isLoading = false
            }
        }
    }

    val onManualRefresh: () -> Unit = {
        loadPage(0, true)
    }

    val loadNextPage: () -> Unit = {
        if (pageInfo.hasMore && !isLoading) {
            loadPage(pageInfo.currentPage + 1, false)
        }
    }

    LaunchedEffect(Unit) {
        while (true) {
            delay(5_000)
            if (!isLoading) {
                scope.launch {
                    val hasNewLogs = checkForNewLogs(lastLogFileHash, currentLogPath, activeLogPath)
                    if (hasNewLogs) {
                        loadPage(0, true)
                    }
                }
            }
        }
    }

    LaunchedEffect(Unit) {
        loadPage(0, true)
    }

    Scaffold(
        topBar = {
            LogViewerTopBar(
                scrollBehavior = scrollBehavior,
                onBackClick = { navigator.navigateUp() },
                showSearchBar = showSearchBar,
                searchQuery = searchQuery,
                onSearchQueryChange = { searchQuery = it },
                onSearchToggle = { showSearchBar = !showSearchBar },
                onRefresh = onManualRefresh,
                canManageLogs = currentLogPath.isNotBlank(),
                manageLogsTitle = currentManageTitle,
                onClearLogs = {
                    scope.launch {
                        if (currentLogPath.isBlank()) return@launch
                        val result = confirmDialog.awaitConfirm(
                            title = currentManageTitle,
                            content = currentManageConfirmMessage
                        )
                        if (result == ConfirmResult.Confirmed) {
                            loadingDialog.withLoading {
                                when (currentCleanAction) {
                                    SulogLogSourceCleanAction.Clear -> clearLogs(currentLogPath)
                                    SulogLogSourceCleanAction.Delete -> deleteLogs(currentLogPath)
                                }
                                loadPage(0, true)
                            }
                            snackBarHost.showSnackbar(currentManageSuccessMessage)
                        }
                    }
                }
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .padding(paddingValues)
                .nestedScroll(scrollBehavior.nestedScrollConnection)
        ) {
            LogControlPanel(
                filterType = filterType,
                onFilterTypeSelected = { filterType = it },
                logCount = filteredEntries.size,
                totalCount = logEntries.size,
                pageInfo = pageInfo,
                logSources = logSources,
                currentLogPath = currentLogPath,
                onLogSourceSelected = { path ->
                    currentLogPath = path
                    loadPage(0, true)
                },
                excludedSubTypes = excludedSubTypes,
                onExcludeToggle = { excl ->
                    excludedSubTypes = if (excl in excludedSubTypes)
                        excludedSubTypes - excl
                    else
                        excludedSubTypes + excl
                }
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
            ) {
                if (isLoading && logEntries.isEmpty()) {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                } else if (filteredEntries.isEmpty()) {
                    EmptyLogState(
                        hasLogs = logEntries.isNotEmpty(),
                        onRefresh = onManualRefresh
                    )
                } else {
                    LogList(
                        entries = filteredEntries,
                        pageInfo = pageInfo,
                        isLoading = isLoading,
                        onLoadMore = loadNextPage,
                        modifier = Modifier.fillMaxSize()
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogControlPanel(
    filterType: LogType?,
    onFilterTypeSelected: (LogType?) -> Unit,
    logCount: Int,
    totalCount: Int,
    pageInfo: LogPageInfo,
    logSources: List<SulogLogSource>,
    currentLogPath: String,
    onLogSourceSelected: (String) -> Unit,
    excludedSubTypes: Set<LogExclType>,
    onExcludeToggle: (LogExclType) -> Unit
) {
    var fileSelectorExpanded by rememberSaveable { mutableStateOf(false) }
    var showFilterSheet by rememberSaveable { mutableStateOf(false) }
    var controlsExpanded by rememberSaveable { mutableStateOf(false) }
    val fileSelectorVisible = fileSelectorExpanded && controlsExpanded && logSources.isNotEmpty()
    LaunchedEffect(controlsExpanded, logSources.isEmpty()) {
        if (!controlsExpanded || logSources.isEmpty()) fileSelectorExpanded = false
    }
    val selectedSource = remember(logSources, currentLogPath) {
        resolveSelectedSulogSource(logSources, currentLogPath, currentLogPath)
    }
    val activeFilterCount = (if (filterType != null) 1 else 0) + excludedSubTypes.size
    val excludedLabels = excludedSubTypes.map { excl ->
        when (excl) {
            LogExclType.CURRENT_APP -> stringResource(R.string.log_viewer_exclude_current_app)
            LogExclType.LOG_VIEWER_SELF_REFRESH -> stringResource(R.string.log_viewer_exclude_viewer_refresh)
        }
    }
    val selectedSourceLabel = selectedSource.displayName.ifBlank {
        stringResource(R.string.log_viewer_no_logs)
    }
    val summaryParts = buildList {
        add(stringResource(R.string.log_viewer_showing_entries, logCount, totalCount))
        if (pageInfo.totalPages > 0) {
            add(
                stringResource(
                    R.string.log_viewer_page_info_compact,
                    pageInfo.currentPage + 1,
                    pageInfo.totalPages
                )
            )
        }
        if (logSources.isNotEmpty()) {
            add(stringResource(R.string.log_viewer_file_count, logSources.size))
        }
        if (activeFilterCount > 0) {
            add(stringResource(R.string.log_viewer_filters_with_count, activeFilterCount))
        }
    }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation()
    ) {
        Column(
            modifier = Modifier.padding(SPACING_LARGE),
            verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.Top,
                horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
            ) {
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .heightIn(min = 48.dp)
                        .yukiExpansionSemantics(controlsExpanded)
                        .clickable { controlsExpanded = !controlsExpanded },
                    verticalArrangement = Arrangement.spacedBy(SPACING_SMALL)
                ) {
                    Text(
                        text = selectedSourceLabel,
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.primary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    Text(
                        text = summaryParts.joinToString(" · "),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis
                    )
                }

                Row(
                    horizontalArrangement = Arrangement.spacedBy(SPACING_SMALL),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    FilledTonalIconButton(onClick = { showFilterSheet = true }) {
                        BadgedBox(
                            badge = {
                                if (activeFilterCount > 0) {
                                    Badge {
                                        Text(if (activeFilterCount > 9) "9+" else activeFilterCount.toString())
                                    }
                                }
                            }
                        ) {
                            Icon(
                                imageVector = Icons.Filled.Tune,
                                contentDescription = stringResource(R.string.log_viewer_filters)
                            )
                        }
                    }

                    FilledTonalIconButton(
                        onClick = { controlsExpanded = !controlsExpanded },
                        modifier = Modifier.yukiExpansionSemantics(controlsExpanded),
                    ) {
                        YukiExpandArrow(
                            expanded = controlsExpanded,
                            contentDescription = stringResource(
                                if (controlsExpanded) {
                                    R.string.collapse_menu
                                } else {
                                    R.string.expand_menu
                                }
                            )
                        )
                    }
                }
            }

            YukiExpandableContent(
                visible = controlsExpanded,
            ) {
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
                ) {
                    ExposedDropdownMenuBox(
                        modifier = Modifier.fillMaxWidth(),
                        expanded = fileSelectorVisible,
                        onExpandedChange = {
                            if (controlsExpanded && logSources.isNotEmpty()) {
                                fileSelectorExpanded = it
                            }
                        }
                    ) {
                        OutlinedTextField(
                            modifier = Modifier
                                .fillMaxWidth()
                                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
                            readOnly = true,
                            singleLine = true,
                            value = selectedSourceLabel,
                            onValueChange = {},
                            enabled = logSources.isNotEmpty(),
                            label = { Text(stringResource(R.string.log_viewer_select_file)) },
                            trailingIcon = {
                                YukiExpandArrow(expanded = fileSelectorVisible)
                            }
                        )
                        ExposedDropdownMenu(
                            expanded = fileSelectorVisible,
                            onDismissRequest = { fileSelectorExpanded = false },
                            modifier = Modifier.clickHapticFeedback(),
                        ) {
                            logSources.forEachIndexed { index, source ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            if (index == 0) {
                                                "${source.displayName} (${stringResource(R.string.log_viewer_latest_file)})"
                                            } else {
                                                source.displayName
                                            }
                                        )
                                    },
                                    onClick = {
                                        fileSelectorExpanded = false
                                        onLogSourceSelected(source.path)
                                    }
                                )
                            }
                        }
                    }

                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM),
                        verticalArrangement = Arrangement.spacedBy(SPACING_SMALL)
                    ) {
                        AssistChip(
                            onClick = { showFilterSheet = true },
                            label = {
                                Text(
                                    stringResource(
                                        R.string.log_viewer_filter_summary,
                                        filterType?.displayName
                                            ?: stringResource(R.string.log_viewer_all_types)
                                    )
                                )
                            },
                            leadingIcon = {
                                Icon(
                                    imageVector = Icons.Filled.FilterList,
                                    contentDescription = null,
                                    modifier = Modifier.size(18.dp)
                                )
                            }
                        )

                        if (excludedLabels.isNotEmpty()) {
                            AssistChip(
                                onClick = { showFilterSheet = true },
                                label = {
                                    Text(
                                        stringResource(
                                            R.string.log_viewer_exclude_summary,
                                            excludedLabels.joinToString()
                                        )
                                    )
                                },
                                leadingIcon = {
                                    Icon(
                                        imageVector = Icons.Filled.Block,
                                        contentDescription = null,
                                        modifier = Modifier.size(18.dp)
                                    )
                                }
                            )
                        }

                        AssistChip(
                            onClick = {},
                            label = {
                                Text(stringResource(R.string.log_viewer_showing_entries, logCount, totalCount))
                            }
                        )

                        if (pageInfo.totalPages > 0) {
                            AssistChip(
                                onClick = {},
                                label = {
                                    Text(
                                        stringResource(
                                            R.string.log_viewer_page_info_compact,
                                            pageInfo.currentPage + 1,
                                            pageInfo.totalPages
                                        )
                                    )
                                }
                            )
                        }

                        if (logSources.isNotEmpty()) {
                            AssistChip(
                                onClick = {},
                                label = {
                                    Text(stringResource(R.string.log_viewer_file_count, logSources.size))
                                }
                            )
                        }
                    }
                }
            }

            if (pageInfo.totalLogs >= MAX_TOTAL_LOGS) {
                Text(
                    text = stringResource(R.string.log_viewer_too_many_logs, MAX_TOTAL_LOGS),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )
            }
        }
    }

    if (showFilterSheet) {
        LogFilterBottomSheet(
            filterType = filterType,
            onFilterTypeSelected = onFilterTypeSelected,
            excludedSubTypes = excludedSubTypes,
            onExcludeToggle = onExcludeToggle,
            onDismiss = { showFilterSheet = false }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogFilterBottomSheet(
    filterType: LogType?,
    onFilterTypeSelected: (LogType?) -> Unit,
    excludedSubTypes: Set<LogExclType>,
    onExcludeToggle: (LogExclType) -> Unit,
    onDismiss: () -> Unit
) {
    YukiModalBottomSheet(
        onDismissRequest = onDismiss,
        modifier = Modifier.clickHapticFeedback(),
        containerColor = MaterialTheme.colorScheme.surfaceContainerHigh
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = SPACING_LARGE),
            verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
        ) {
            Text(
                text = stringResource(R.string.log_viewer_filter_options),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.primary
            )

            Text(
                text = stringResource(R.string.log_viewer_filter_type),
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary
            )
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM),
                verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
            ) {
                FilterChip(
                    onClick = { onFilterTypeSelected(null) },
                    label = { Text(stringResource(R.string.log_viewer_all_types)) },
                    selected = filterType == null
                )
                LogType.entries.forEach { type ->
                    FilterChip(
                        onClick = { onFilterTypeSelected(if (filterType == type) null else type) },
                        label = { Text(type.displayName) },
                        selected = filterType == type,
                        leadingIcon = {
                            Box(
                                modifier = Modifier
                                    .size(8.dp)
                                    .background(type.color, RoundedCornerShape(4.dp))
                            )
                        }
                    )
                }
            }

            Text(
                text = stringResource(R.string.log_viewer_exclude_subtypes),
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary
            )
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM),
                verticalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
            ) {
                LogExclType.entries.forEach { excl ->
                    val label = when (excl) {
                        LogExclType.CURRENT_APP -> stringResource(R.string.log_viewer_exclude_current_app)
                        LogExclType.LOG_VIEWER_SELF_REFRESH -> stringResource(R.string.log_viewer_exclude_viewer_refresh)
                    }

                    FilterChip(
                        onClick = { onExcludeToggle(excl) },
                        label = { Text(label) },
                        selected = excl in excludedSubTypes,
                        leadingIcon = {
                            Box(
                                modifier = Modifier
                                    .size(8.dp)
                                    .background(excl.color, RoundedCornerShape(4.dp))
                            )
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(SPACING_LARGE))
        }
    }
}

@Composable
private fun LogList(
    entries: List<LogEntry>,
    pageInfo: LogPageInfo,
    isLoading: Boolean,
    onLoadMore: () -> Unit,
    modifier: Modifier = Modifier
) {
    val listState = rememberLazyListState()

    LazyColumn(
        state = listState,
        modifier = modifier,
        contentPadding = PaddingValues(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
        verticalArrangement = Arrangement.spacedBy(SPACING_SMALL)
    ) {
        items(entries) { entry ->
            LogEntryCard(entry = entry)
        }

        if (pageInfo.hasMore) {
            item {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(SPACING_LARGE),
                    contentAlignment = Alignment.Center
                ) {
                    if (isLoading) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(24.dp)
                        )
                    } else {
                        Button(
                            onClick = onLoadMore,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(
                                imageVector = Icons.Filled.ExpandMore,
                                contentDescription = null,
                                modifier = Modifier.size(18.dp)
                            )
                            Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                            Text(stringResource(R.string.log_viewer_load_more))
                        }
                    }
                }
            }
        } else if (entries.isNotEmpty()) {
            item {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(SPACING_LARGE),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = stringResource(R.string.log_viewer_all_logs_loaded),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
private fun LogEntryCard(entry: LogEntry) {
    var expanded by remember { mutableStateOf(false) }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .yukiAnimateContentSize()
            .yukiExpansionSemantics(expanded)
            .clickable { expanded = !expanded },
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp)
    ) {
        Column(
            modifier = Modifier.padding(SPACING_MEDIUM)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(SPACING_MEDIUM)
                ) {
                    Box(
                        modifier = Modifier
                            .size(12.dp)
                            .background(entry.type.color, RoundedCornerShape(6.dp))
                    )
                    Text(
                        text = entry.type.displayName,
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.Bold
                    )
                }
                Text(
                    text = entry.timestamp,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Spacer(modifier = Modifier.height(SPACING_SMALL))

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = "UID: ${entry.uid}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    text = "PID: ${entry.pid}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Text(
                text = entry.comm,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                maxLines = if (expanded) Int.MAX_VALUE else 1,
                overflow = TextOverflow.Ellipsis
            )

            if (entry.details.isNotEmpty()) {
                Spacer(modifier = Modifier.height(SPACING_SMALL))
                Text(
                    text = entry.details,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = if (expanded) Int.MAX_VALUE else 2,
                    overflow = TextOverflow.Ellipsis
                )
            }

            if (expanded) {
                Column {
                    Spacer(modifier = Modifier.height(SPACING_MEDIUM))
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    Spacer(modifier = Modifier.height(SPACING_MEDIUM))
                    Text(
                        text = stringResource(R.string.log_viewer_raw_log),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary
                    )
                    Spacer(modifier = Modifier.height(SPACING_SMALL))
                    Text(
                        text = entry.rawLine,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
private fun EmptyLogState(
    hasLogs: Boolean,
    onRefresh: () -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(SPACING_LARGE)
        ) {
            Icon(
                imageVector = if (hasLogs) Icons.Filled.FilterList else Icons.Filled.Description,
                contentDescription = null,
                modifier = Modifier.size(64.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Text(
                text = stringResource(
                    if (hasLogs) R.string.log_viewer_no_matching_logs
                    else R.string.log_viewer_no_logs
                ),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Button(onClick = onRefresh) {
                Icon(
                    imageVector = Icons.Filled.Refresh,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                Text(stringResource(R.string.log_viewer_refresh))
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogViewerTopBar(
    scrollBehavior: TopAppBarScrollBehavior? = null,
    onBackClick: () -> Unit,
    showSearchBar: Boolean,
    searchQuery: String,
    onSearchQueryChange: (String) -> Unit,
    onSearchToggle: () -> Unit,
    onRefresh: () -> Unit,
    canManageLogs: Boolean,
    manageLogsTitle: String,
    onClearLogs: () -> Unit
) {
    val colorScheme = MaterialTheme.colorScheme
    val cardColor = if (CardConfig.isCustomBackgroundEnabled) {
        colorScheme.surfaceContainerLow
    } else {
        colorScheme.background
    }

    Column {
        TopAppBar(
            title = {
                Text(
                    text = stringResource(R.string.log_viewer_title),
                    style = MaterialTheme.typography.titleLarge
                )
            },
            navigationIcon = {
                IconButton(onClick = onBackClick) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                        contentDescription = stringResource(R.string.log_viewer_back)
                    )
                }
            },
            actions = {
                IconButton(onClick = onSearchToggle) {
                    Icon(
                        imageVector = if (showSearchBar) Icons.Filled.SearchOff else Icons.Filled.Search,
                        contentDescription = stringResource(R.string.log_viewer_search)
                    )
                }
                IconButton(onClick = onRefresh) {
                    Icon(
                        imageVector = Icons.Filled.Refresh,
                        contentDescription = stringResource(R.string.log_viewer_refresh)
                    )
                }
                IconButton(onClick = onClearLogs, enabled = canManageLogs) {
                    Icon(
                        imageVector = Icons.Filled.DeleteSweep,
                        contentDescription = manageLogsTitle
                    )
                }
            },
            colors = getTopAppBarColors(
                containerColor = cardColor,
                scrolledContainerColor = cardColor
            ),
            windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
            scrollBehavior = scrollBehavior
        )

        YukiExpandableContent(
            visible = showSearchBar,
        ) {
            OutlinedTextField(
                value = searchQuery,
                onValueChange = onSearchQueryChange,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = SPACING_LARGE, vertical = SPACING_MEDIUM),
                placeholder = { Text(stringResource(R.string.log_viewer_search_placeholder)) },
                leadingIcon = {
                    Icon(
                        imageVector = Icons.Filled.Search,
                        contentDescription = null
                    )
                },
                trailingIcon = {
                    if (searchQuery.isNotEmpty()) {
                        IconButton(onClick = { onSearchQueryChange("") }) {
                            Icon(
                                imageVector = Icons.Filled.Clear,
                                contentDescription = stringResource(R.string.log_viewer_clear_search)
                            )
                        }
                    }
                },
                singleLine = true
            )
        }
    }
}

private suspend fun checkForNewLogs(
    lastHash: String,
    preferredPath: String,
    previousActivePath: String,
): Boolean {
    return withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val sources = listSulogSources(shell)
            val activePath = readActiveSulogPath(shell)
            val source = resolveSelectedSulogSource(sources, preferredPath, activePath, previousActivePath)
            val currentHash = buildVisibleSulogSourceSignature(shell, sources, source, activePath)

            currentHash != lastHash
        } catch (_: Exception) {
            false
        }
    }
}

private suspend fun loadLogsWithPagination(
    page: Int,
    forceRefresh: Boolean,
    lastHash: String,
    preferredPath: String,
    previousActivePath: String,
    onLoaded: (List<LogEntry>, LogPageInfo, String, String, List<SulogLogSource>, String) -> Unit
) {
    withContext(Dispatchers.IO) {
        try {
            val shell = getRootShell()
            val sources = listSulogSources(shell)
            val activePath = readActiveSulogPath(shell)
            val source = resolveSelectedSulogSource(sources, preferredPath, activePath, previousActivePath)
            val currentHash = buildVisibleSulogSourceSignature(shell, sources, source, activePath)
            val statPart = readSulogSourceStat(shell, source.path)

            if (source.path.isBlank() || statPart == "0 0") {
                withContext(Dispatchers.Main) {
                    onLoaded(emptyList(), LogPageInfo(), currentHash, DEFAULT_LOG_PATH, sources, activePath)
                }
                return@withContext
            }

            val quotedPath = shellArg(source.path)

            if (page == 0 && !forceRefresh && currentHash == lastHash && statPart != "0 0") {
                withContext(Dispatchers.Main) {
                    onLoaded(
                        emptyList(),
                        LogPageInfo(currentFileName = source.name),
                        currentHash,
                        source.path,
                        sources,
                        activePath
                    )
                }
                return@withContext
            }

            val totalLinesResult = runCmd(shell, "wc -l < $quotedPath 2>/dev/null || echo '0'")
            val totalLines = totalLinesResult.trim().toIntOrNull() ?: 0

            if (totalLines == 0) {
                withContext(Dispatchers.Main) {
                    onLoaded(
                        emptyList(),
                        LogPageInfo(currentFileName = source.name),
                        currentHash,
                        source.path,
                        sources,
                        activePath
                    )
                }
                return@withContext
            }

            val effectiveTotal = minOf(totalLines, MAX_TOTAL_LOGS)
            val totalPages = (effectiveTotal + PAGE_SIZE - 1) / PAGE_SIZE
            val retainedStartLine = totalLines - effectiveTotal + 1

            val endOffsetExclusive = effectiveTotal - page * PAGE_SIZE
            val startOffsetInclusive = maxOf(0, endOffsetExclusive - PAGE_SIZE)
            val startLine = retainedStartLine + startOffsetInclusive
            val endLine = retainedStartLine + endOffsetExclusive - 1

            if (startLine > endLine || startLine > totalLines) {
                withContext(Dispatchers.Main) {
                    onLoaded(
                        emptyList(),
                        LogPageInfo(
                            currentPage = page,
                            totalPages = totalPages,
                            totalLogs = effectiveTotal,
                            hasMore = false,
                            currentFileName = source.name
                        ),
                        currentHash,
                        source.path,
                        sources,
                        activePath
                    )
                }
                return@withContext
            }

            val result = runCmd(shell, "sed -n '${startLine},${endLine}p' $quotedPath 2>/dev/null || echo ''")
            val entries = parseLogEntries(
                logContent = result,
                useCurrentClockFallback = source.path == activePath
            )

            val hasMore = page < totalPages - 1
            val pageInfo = LogPageInfo(
                currentPage = page,
                totalPages = totalPages,
                totalLogs = effectiveTotal,
                hasMore = hasMore,
                currentFileName = source.name
            )

            withContext(Dispatchers.Main) {
                onLoaded(entries, pageInfo, currentHash, source.path, sources, activePath)
            }
        } catch (_: Exception) {
            withContext(Dispatchers.Main) {
                onLoaded(emptyList(), LogPageInfo(), lastHash, DEFAULT_LOG_PATH, emptyList(), DEFAULT_LOG_PATH)
            }
        }
    }
}

private suspend fun clearLogs(path: String) {
    withContext(Dispatchers.IO) {
        try {
            if (path.isBlank()) return@withContext
            val shell = getRootShell()
            runCmd(shell, ": > ${shellArg(path)}")
        } catch (_: Exception) {
        }
    }
}

private suspend fun deleteLogs(path: String) {
    withContext(Dispatchers.IO) {
        try {
            if (path.isBlank()) return@withContext
            val shell = getRootShell()
            runCmd(shell, "rm -f ${shellArg(path)}")
        } catch (_: Exception) {
        }
    }
}

private fun parseLogEntries(logContent: String, useCurrentClockFallback: Boolean): List<LogEntry> {
    return parseSulogEntries(logContent, useCurrentClockFallback).reversed()
}
