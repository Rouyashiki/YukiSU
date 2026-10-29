package ui.screen.partition

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Block
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Checklist
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DoneAll
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.Upload
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.material3.TriStateCheckbox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.SearchAppBar
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.screen.FlashIt
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.LocalSnackbarHost
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.RamdiskEditorScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Destination<RootGraph>
@Composable
fun PartitionManagerScreen(navigator: DestinationsNavigator) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val snackbarHost = LocalSnackbarHost.current

    var partitionList by remember { mutableStateOf<List<PartitionInfo>>(emptyList()) }
    var allPartitionList by remember { mutableStateOf<List<PartitionInfo>>(emptyList()) }
    var slotInfo by remember { mutableStateOf<SlotInfo?>(null) }
    var selectedSlot by remember { mutableStateOf<String?>(null) }
    var hasLoaded by remember { mutableStateOf(false) }
    var isRefreshing by remember { mutableStateOf(false) }
    var refreshGeneration by remember { mutableIntStateOf(0) }

    var showAllPartitions by rememberSaveable { mutableStateOf(false) }
    var partitionTypeFilter by rememberSaveable { mutableStateOf("all") }
    var searchQuery by rememberSaveable { mutableStateOf("") }
    var multiSelectMode by rememberSaveable { mutableStateOf(false) }
    var selectedPartitions by remember { mutableStateOf<Set<String>>(emptySet()) }
    var selectedPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var avbPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var operationMessage by remember { mutableStateOf<String?>(null) }

    var pendingFlashPartition by remember { mutableStateOf<PartitionInfo?>(null) }
    var stagedFlashImage by remember { mutableStateOf<StagedFlashImage?>(null) }
    var stagedAk3Package by remember { mutableStateOf<StagedAk3Package?>(null) }
    var backupDirectory by remember {
        mutableStateOf(loadPartitionBackupDirectory(context))
    }
    var backupDirectoryDraft by rememberSaveable { mutableStateOf(backupDirectory) }
    var showBackupDirectoryDialog by rememberSaveable { mutableStateOf(false) }

    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    val displayList = if (showAllPartitions) allPartitionList else partitionList
    val filteredList = remember(displayList, partitionTypeFilter, searchQuery) {
        displayList.filter { partition ->
            val typeMatches = when (partitionTypeFilter) {
                "physical" -> !partition.isLogical
                "logical" -> partition.isLogical
                else -> true
            }
            typeMatches && (
                searchQuery.isBlank() ||
                    partition.name.contains(searchQuery.trim(), ignoreCase = true) ||
                    partition.blockDevice.contains(searchQuery.trim(), ignoreCase = true)
                )
        }
    }
    val selectableVisible = filteredList.filterNot(PartitionInfo::excludeFromBatch)
    val isBusy = operationMessage != null

    val refreshPartitions: suspend (String?, Boolean) -> Unit = { requestedSlot, reloadDevice ->
        val generation = refreshGeneration + 1
        refreshGeneration = generation
        isRefreshing = true
        val result = runCatching {
            val deviceSlots = if (reloadDevice) {
                PartitionManagerHelper.getSlotInfo(context)
            } else {
                slotInfo
            }
            val effectiveSlot = if (reloadDevice) {
                requestedSlot ?: deviceSlots?.currentSlot
            } else {
                requestedSlot
            }
            PartitionLoadSnapshot(
                slotInfo = deviceSlots,
                selectedSlot = effectiveSlot,
                commonPartitions = PartitionManagerHelper.getPartitionList(
                    context,
                    effectiveSlot,
                    scanAll = false,
                ),
                allPartitions = PartitionManagerHelper.getPartitionList(
                    context,
                    effectiveSlot,
                    scanAll = true,
                ),
            )
        }
        if (generation == refreshGeneration) {
            result.onSuccess { snapshot ->
                slotInfo = snapshot.slotInfo
                selectedSlot = snapshot.selectedSlot
                partitionList = snapshot.commonPartitions
                allPartitionList = snapshot.allPartitions
                val validNames = snapshot.allPartitions.mapTo(mutableSetOf(), PartitionInfo::name)
                selectedPartitions = selectedPartitions.intersect(validNames)
                hasLoaded = true
            }.onFailure { error ->
                snackbarHost.showSnackbar(
                    resources.getString(
                        R.string.partition_load_failed,
                        error.message ?: resources.getString(R.string.partition_unknown),
                    )
                )
            }
            isRefreshing = false
        }
    }

    val backupDirectoryPickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri != null) {
            val selectedPath = resolveBackupDirectoryPath(uri)
            scope.launch {
                if (selectedPath == null) {
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.partition_backup_directory_picker_unsupported
                        )
                    )
                } else {
                    backupDirectoryDraft = selectedPath
                }
            }
        }
    }

    val filePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        val partition = pendingFlashPartition
        pendingFlashPartition = null
        if (uri != null && partition != null) {
            scope.launch {
                operationMessage = resources.getString(R.string.partition_preparing_image)
                val result = runCatching {
                    withContext(Dispatchers.IO) {
                        stageFlashImage(context, uri, partition, selectedSlot)
                    }
                }
                operationMessage = null
                result.onSuccess { staged ->
                    when {
                        staged.size <= 0L -> {
                            staged.cacheFile.delete()
                            snackbarHost.showSnackbar(
                                resources.getString(R.string.partition_image_empty)
                            )
                        }
                        partition.size > 0L && staged.size > partition.size -> {
                            staged.cacheFile.delete()
                            snackbarHost.showSnackbar(
                                resources.getString(
                                    R.string.partition_image_too_large,
                                    formatSize(staged.size),
                                    formatSize(partition.size),
                                )
                            )
                        }
                        else -> {
                            stagedFlashImage?.cacheFile?.delete()
                            stagedFlashImage = staged
                        }
                    }
                }.onFailure { error ->
                    snackbarHost.showSnackbar(
                        if (error is InsufficientCacheSpaceException) {
                            resources.getString(
                                R.string.partition_insufficient_cache_space,
                                formatSize(error.requiredBytes),
                                formatSize(error.availableBytes),
                            )
                        } else {
                            resources.getString(
                                R.string.partition_cannot_read_file_detail,
                                error.message
                                    ?: resources.getString(R.string.partition_unknown),
                            )
                        }
                    )
                }
            }
        }
    }

    val ak3PickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        if (uri != null) {
            scope.launch {
                operationMessage = resources.getString(R.string.partition_ak3_inspecting)
                var stagedFile: java.io.File? = null
                val result = runCatching {
                    val (displayName, cacheFile) = withContext(Dispatchers.IO) {
                        stageAk3Package(context, uri)
                    }
                    stagedFile = cacheFile
                    val info = PartitionManagerHelper.inspectAk3Package(
                        context = context,
                        zipPath = cacheFile.absolutePath,
                    )
                    StagedAk3Package(
                        displayName = displayName,
                        cacheFile = cacheFile,
                        info = info,
                    )
                }
                operationMessage = null
                result.onSuccess { staged ->
                    stagedAk3Package?.cacheFile?.delete()
                    stagedAk3Package = staged
                }.onFailure { error ->
                    stagedFile?.delete()
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.partition_ak3_invalid,
                            error.message ?: resources.getString(R.string.partition_unknown),
                        )
                    )
                }
            }
        }
    }

    LaunchedEffect(Unit) {
        refreshPartitions(null, true)
    }
    DisposableEffect(Unit) {
        onDispose {
            stagedFlashImage?.cacheFile?.delete()
            stagedAk3Package?.cacheFile?.delete()
        }
    }

    fun startRefresh(slot: String?, reloadDevice: Boolean = false) {
        if (!isBusy) {
            scope.launch { refreshPartitions(slot, reloadDevice) }
        }
    }

    Scaffold(
        topBar = {
            PartitionManagerTopBar(
                onBack = { navigator.popBackStack() },
                onRefresh = { startRefresh(selectedSlot, reloadDevice = true) },
                searchText = searchQuery,
                onSearchTextChange = { searchQuery = it },
                onClearSearch = { searchQuery = "" },
                onOpenBackupDirectory = {
                    backupDirectoryDraft = backupDirectory
                    showBackupDirectoryDialog = true
                },
                actionsEnabled = !isBusy && !isRefreshing,
                scrollBehavior = scrollBehavior,
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    ) { paddingValues ->
        if (!hasLoaded && isRefreshing) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues),
                contentAlignment = Alignment.Center,
            ) {
                CircularProgressIndicator()
            }
        } else {
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues)
                    .nestedScroll(scrollBehavior.nestedScrollConnection),
                contentPadding = PaddingValues(
                    start = 16.dp,
                    top = 12.dp,
                    end = 16.dp,
                    bottom = 32.dp,
                ),
                verticalArrangement = Arrangement.spacedBy(
                    if (isExpressiveUi) 0.dp else 12.dp
                ),
            ) {
                if (isRefreshing) {
                    partitionSection(key = "refresh") {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    }
                }

                operationMessage?.let { message ->
                    partitionSection(key = "operation") {
                        OperationCard(message)
                    }
                }

                slotInfo?.let { slots ->
                    partitionSection(key = "slots") {
                        SlotInfoCard(
                            slotInfo = slots,
                            selectedSlot = selectedSlot,
                            enabled = !isBusy && !isRefreshing,
                            onSlotChange = { newSlot ->
                                selectedPartitions = emptySet()
                                multiSelectMode = false
                                selectedPartition = null
                                selectedSlot = newSlot
                                startRefresh(newSlot)
                            },
                        )
                    }
                    if (
                        slots.isAbDevice &&
                        selectedSlot != null &&
                        selectedSlot != slots.currentSlot
                    ) {
                        partitionSection(key = "map") {
                            InactiveSlotMappingCard(
                                enabled = !isBusy && !isRefreshing,
                                onMap = {
                                    val targetSlot = selectedSlot ?: return@InactiveSlotMappingCard
                                    scope.launch {
                                        operationMessage = resources.getString(
                                            R.string.partition_mapping,
                                            targetSlot,
                                        )
                                        val logs = mutableListOf<String>()
                                        val success = PartitionManagerHelper.mapLogicalPartitions(
                                            context = context,
                                            slot = targetSlot,
                                            onStdout = logs::add,
                                            onStderr = { logs.add("ERROR: $it") },
                                        )
                                        operationMessage = null
                                        if (success) {
                                            snackbarHost.showSnackbar(
                                                resources.getString(
                                                    R.string.partition_map_success
                                                )
                                            )
                                            refreshPartitions(targetSlot, false)
                                        } else {
                                            snackbarHost.showSnackbar(
                                                resources.getString(
                                                    R.string.partition_map_failed,
                                                    logs.lastOrNull()
                                                        ?: resources.getString(
                                                            R.string.partition_unknown
                                                        ),
                                                )
                                            )
                                        }
                                    }
                                },
                            )
                        }
                    }
                }

                partitionSection(key = "overview") {
                    PartitionOverviewCard(
                        partitions = allPartitionList,
                        backupDirectory = backupDirectory,
                    )
                }

                partitionSection(key = "controls") {
                    PartitionControls(
                        filter = partitionTypeFilter,
                        onFilterChange = { partitionTypeFilter = it },
                        showAll = showAllPartitions,
                        onToggleShowAll = { showAllPartitions = !showAllPartitions },
                        selectionMode = multiSelectMode,
                        onToggleSelection = {
                            multiSelectMode = !multiSelectMode
                            if (!multiSelectMode) selectedPartitions = emptySet()
                        },
                        enabled = !isBusy && !isRefreshing,
                    )
                }

                if (multiSelectMode) {
                    partitionSection(key = "selection") {
                        PartitionSelectionCard(
                            selectedCount = selectedPartitions.size,
                            selectableVisible = selectableVisible,
                            selectedPartitions = selectedPartitions,
                            enabled = !isBusy && !isRefreshing,
                            onSelectionChange = { selectedPartitions = it },
                            onSelectBootSet = {
                                selectedPartitions = allPartitionList
                                    .filter {
                                        it.name in BOOT_CRITICAL_PARTITIONS &&
                                            !it.excludeFromBatch
                                    }
                                    .mapTo(mutableSetOf(), PartitionInfo::name)
                            },
                            onBackup = {
                                val names = selectedPartitions
                                scope.launch {
                                    operationMessage = resources.getString(
                                        R.string.partition_batch_backup_start,
                                        names.size,
                                    )
                                    try {
                                        handleBatchBackup(
                                            context = context,
                                            selectedPartitionNames = names,
                                            allPartitions = allPartitionList,
                                            slot = selectedSlot,
                                            backupDirectory = backupDirectory,
                                            snackbarHost = snackbarHost,
                                            onProgress = { current, total, name ->
                                                operationMessage = resources.getString(
                                                    R.string.partition_batch_backup_progress,
                                                    current,
                                                    total,
                                                    name,
                                                )
                                            },
                                        )
                                    } finally {
                                        operationMessage = null
                                    }
                                }
                            },
                        )
                    }
                }

                partitionSection(key = "section-title") {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = 8.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = stringResource(
                                if (showAllPartitions) {
                                    R.string.partition_all
                                } else {
                                    R.string.partition_common
                                }
                            ),
                            style = if (isExpressiveUi) {
                                MaterialTheme.typography.headlineSmall
                            } else {
                                MaterialTheme.typography.titleMedium
                            },
                            fontWeight = if (isExpressiveUi) {
                                FontWeight.Normal
                            } else {
                                FontWeight.Bold
                            },
                            modifier = Modifier.weight(1f),
                        )
                        Text(
                            text = filteredList.size.toString(),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            style = MaterialTheme.typography.labelLarge,
                        )
                    }
                }

                if (filteredList.isEmpty()) {
                    item(key = "empty") {
                        EmptyPartitionCard()
                    }
                } else {
                    itemsIndexed(
                        items = filteredList,
                        key = { _, partition -> partition.name },
                    ) { index, partition ->
                        val motion = LocalYukiMotion.current
                        PartitionCard(
                            modifier = Modifier.animateItem(
                                fadeInSpec = motion.effects(),
                                placementSpec = motion.spatial(),
                                fadeOutSpec = motion.effects(),
                            ),
                            partition = partition,
                            index = index,
                            count = filteredList.size,
                            isSelected = partition.name in selectedPartitions,
                            multiSelectMode = multiSelectMode,
                            enabled = !isBusy && !isRefreshing,
                            onClick = {
                                if (multiSelectMode) {
                                    if (partition.excludeFromBatch) {
                                        scope.launch {
                                            snackbarHost.showSnackbar(
                                                resources.getString(
                                                    R.string.partition_batch_excluded
                                                )
                                            )
                                        }
                                    } else {
                                        selectedPartitions =
                                            selectedPartitions.toggle(partition.name)
                                    }
                                } else {
                                    selectedPartition = partition
                                }
                            },
                            onLongClick = {
                                if (!partition.excludeFromBatch) {
                                    multiSelectMode = true
                                    selectedPartitions =
                                        selectedPartitions.toggle(partition.name)
                                }
                            },
                        )
                    }
                }
            }
        }
    }

    if (showBackupDirectoryDialog) {
        BackupLocationDialog(
            backupDirectory = backupDirectoryDraft,
            enabled = !isBusy,
            onBackupDirectoryChange = { backupDirectoryDraft = it },
            onChooseDirectory = { backupDirectoryPickerLauncher.launch(null) },
            onDismiss = { showBackupDirectoryDialog = false },
            onConfirm = {
                val selectedPath = backupDirectoryDraft.trim()
                backupDirectory = selectedPath
                savePartitionBackupDirectory(context, selectedPath)
                showBackupDirectoryDialog = false
                scope.launch {
                    snackbarHost.showSnackbar(
                        resources.getString(
                            R.string.partition_backup_directory_selected,
                            selectedPath,
                        )
                    )
                }
            },
        )
    }

    selectedPartition?.let { partition ->
        val partitionName = partition.name.removeSuffix("_a").removeSuffix("_b")
        val hasInitBoot = allPartitionList.any {
            it.name.removeSuffix("_a").removeSuffix("_b") == "init_boot"
        }
        PartitionActionDialog(
            partition = partition,
            targetSlot = selectedSlot,
            enabled = !isBusy,
            showRamdiskEditor = when (partitionName) {
                "boot" -> !hasInitBoot
                "init_boot", "vendor_boot", "recovery" -> true
                else -> false
            },
            onDismiss = { selectedPartition = null },
            onBackup = {
                selectedPartition = null
                scope.launch {
                    operationMessage = resources.getString(
                        R.string.partition_backing_up_to,
                        partition.name,
                        backupDirectory,
                    )
                    try {
                        handlePartitionBackup(
                            context = context,
                            partition = partition,
                            slot = selectedSlot,
                            backupDirectory = backupDirectory,
                            snackbarHost = snackbarHost,
                        )
                    } finally {
                        operationMessage = null
                    }
                }
            },
            onFlashAk3 = {
                selectedPartition = null
                ak3PickerLauncher.launch("application/zip")
            },
            onEditRamdisk = {
                selectedPartition = null
                navigator.navigate(
                    RamdiskEditorScreenDestination(
                        partitionName = partition.name,
                        targetSlot = selectedSlot,
                    )
                )
            },
            onManageAvb = { resolvedBlockDevice ->
                selectedPartition = null
                avbPartition = partition.copy(blockDevice = resolvedBlockDevice)
            },
            onFlash = { resolvedBlockDevice ->
                selectedPartition = null
                pendingFlashPartition = partition.copy(blockDevice = resolvedBlockDevice)
                filePickerLauncher.launch("*/*")
            },
        )
    }

    avbPartition?.let { partition ->
        PartitionAvbDialog(
            partition = partition,
            onDismiss = { avbPartition = null },
        )
    }

    stagedFlashImage?.let { staged ->
        FlashPreflightDialog(
            staged = staged,
            enabled = !isBusy,
            onDismiss = {
                staged.cacheFile.delete()
                stagedFlashImage = null
            },
            onConfirm = {
                scope.launch {
                    operationMessage = resources.getString(
                        R.string.partition_flashing,
                        staged.partition.name,
                    )
                    val logs = mutableListOf<String>()
                    val success = try {
                        PartitionManagerHelper.flashPartition(
                            context = context,
                            imagePath = staged.cacheFile.absolutePath,
                            partition = staged.partition.name,
                            slot = staged.slot,
                            onStdout = logs::add,
                            onStderr = { logs.add("ERROR: $it") },
                        )
                    } finally {
                        staged.cacheFile.delete()
                        stagedFlashImage = null
                        operationMessage = null
                    }
                    if (success) {
                        snackbarHost.showSnackbar(
                            resources.getString(R.string.partition_flash_success)
                        )
                    } else {
                        snackbarHost.showSnackbar(
                            if (logs.isEmpty()) {
                                resources.getString(
                                    R.string.partition_flash_failed_check_log
                                )
                            } else {
                                resources.getString(
                                    R.string.partition_flash_failed,
                                    logs.last(),
                                )
                            }
                        )
                    }
                }
            },
        )
    }

    stagedAk3Package?.let { staged ->
        Ak3PreflightDialog(
            staged = staged,
            selectedSlot = selectedSlot,
            enabled = !isBusy,
            onDismiss = {
                staged.cacheFile.delete()
                stagedAk3Package = null
            },
            onConfirm = { useMkbootfs ->
                stagedAk3Package = null
                navigator.navigate(
                    FlashScreenDestination(
                        FlashIt.FlashAk3(
                            zipPath = staged.cacheFile.absolutePath,
                            targetSlot = selectedSlot,
                            useMkbootfs = useMkbootfs,
                        )
                    )
                )
            },
        )
    }
}

private fun LazyListScope.partitionSection(
    key: String,
    content: @Composable () -> Unit,
) {
    item(key = key) {
        val motion = LocalYukiMotion.current
        Box(
            modifier = Modifier
                .animateItem(
                    fadeInSpec = motion.effects(),
                    placementSpec = motion.spatial(),
                    fadeOutSpec = motion.effects(),
                )
                .padding(bottom = if (isExpressiveUi) 8.dp else 0.dp),
        ) {
            content()
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun PartitionManagerTopBar(
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    searchText: String,
    onSearchTextChange: (String) -> Unit,
    onClearSearch: () -> Unit,
    onOpenBackupDirectory: () -> Unit,
    actionsEnabled: Boolean,
    scrollBehavior: TopAppBarScrollBehavior,
) {
    SearchAppBar(
        title = {
            Text(
                text = stringResource(R.string.partition_manager),
                fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
            )
        },
        searchText = searchText,
        onSearchTextChange = onSearchTextChange,
        onClearClick = onClearSearch,
        placeholder = { Text(stringResource(R.string.partition_search_hint)) },
        onBackClick = onBack,
        dropdownContent = {
            IconButton(onClick = onOpenBackupDirectory, enabled = actionsEnabled) {
                YukiIcon(
                    Icons.Filled.Folder,
                    contentDescription = stringResource(R.string.partition_backup_directory),
                )
            }
            IconButton(onClick = onRefresh, enabled = actionsEnabled) {
                YukiIcon(
                    Icons.Filled.Refresh,
                    contentDescription = stringResource(R.string.partition_refresh),
                )
            }
        },
        scrollBehavior = scrollBehavior,
    )
}

@Composable
private fun OperationCard(message: String) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer
        ),
        shape = partitionCardShape(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            CircularProgressIndicator(modifier = Modifier.size(22.dp), strokeWidth = 2.dp)
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun InactiveSlotMappingCard(
    enabled: Boolean,
    onMap: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer
        ),
        shape = partitionCardShape(),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                YukiIcon(
                    Icons.Filled.Info,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.secondary,
                )
                Text(
                    text = stringResource(R.string.partition_map_inactive_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f),
                )
            }
            Button(
                onClick = onMap,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth(),
            ) {
                YukiIcon(Icons.Filled.Refresh, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.partition_map_inactive))
            }
        }
    }
}

@Composable
private fun BackupLocationDialog(
    backupDirectory: String,
    enabled: Boolean,
    onBackupDirectoryChange: (String) -> Unit,
    onChooseDirectory: () -> Unit,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            YukiIcon(
                Icons.Filled.Folder,
                contentDescription = null,
            )
        },
        title = {
            Text(stringResource(R.string.partition_backup_directory))
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = backupDirectory,
                    onValueChange = onBackupDirectoryChange,
                    enabled = enabled,
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    textStyle = MaterialTheme.typography.bodyMedium.copy(
                        fontFamily = FontFamily.Monospace
                    ),
                    supportingText = {
                        Text(stringResource(R.string.partition_backup_directory_desc))
                    },
                )
                OutlinedButton(
                    onClick = onChooseDirectory,
                    enabled = enabled,
                    modifier = Modifier.fillMaxWidth(),
                    shape = if (isExpressiveUi) CircleShape else ButtonDefaults.outlinedShape,
                ) {
                    YukiIcon(Icons.Filled.Folder, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.partition_backup_directory_choose))
                }
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        },
        confirmButton = {
            TextButton(
                onClick = onConfirm,
                enabled = enabled && backupDirectory.isNotBlank(),
            ) {
                Text(stringResource(android.R.string.ok))
            }
        },
    )
}

@Composable
private fun PartitionOverviewCard(
    partitions: List<PartitionInfo>,
    backupDirectory: String,
) {
    val physicalCount = partitions.count { !it.isLogical }
    val logicalCount = partitions.size - physicalCount
    val totalSize = remember(partitions) { totalPartitionSize(partitions) }
    val freeSpace = remember(backupDirectory) { availableStorageBytes(backupDirectory) }
    PartitionSurfaceCard {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                text = stringResource(R.string.partition_overview),
                style = MaterialTheme.typography.titleMedium,
                color = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
                fontWeight = if (isExpressiveUi) FontWeight.Normal else FontWeight.Bold,
            )
            InfoRow(
                stringResource(R.string.partition_count),
                partitions.size.toString(),
            )
            InfoRow(
                stringResource(R.string.partition_type_summary),
                stringResource(
                    R.string.partition_type_summary_value,
                    physicalCount,
                    logicalCount,
                ),
            )
            InfoRow(
                stringResource(R.string.partition_total_size),
                formatSize(totalSize),
            )
            InfoRow(
                stringResource(R.string.partition_free_space),
                formatSize(freeSpace),
            )
        }
    }
}

@Composable
private fun PartitionControls(
    filter: String,
    onFilterChange: (String) -> Unit,
    showAll: Boolean,
    onToggleShowAll: () -> Unit,
    selectionMode: Boolean,
    onToggleSelection: () -> Unit,
    enabled: Boolean,
) {
    PartitionSurfaceCard {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                PartitionFilterChip(
                    selected = filter == "all",
                    onClick = { onFilterChange("all") },
                    label = stringResource(R.string.partition_filter_all),
                    enabled = enabled,
                )
                PartitionFilterChip(
                    selected = filter == "physical",
                    onClick = { onFilterChange("physical") },
                    label = stringResource(R.string.partition_filter_physical),
                    enabled = enabled,
                )
                PartitionFilterChip(
                    selected = filter == "logical",
                    onClick = { onFilterChange("logical") },
                    label = stringResource(R.string.partition_filter_logical),
                    enabled = enabled,
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedButton(
                    onClick = onToggleShowAll,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).yukiExpansionSemantics(showAll),
                ) {
                    YukiExpandArrow(expanded = showAll)
                    Spacer(Modifier.width(6.dp))
                    Text(
                        stringResource(
                            if (showAll) {
                                R.string.partition_collapse
                            } else {
                                R.string.partition_show_all
                            }
                        ),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                OutlinedButton(
                    onClick = onToggleSelection,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) {
                    YukiIcon(
                        if (selectionMode) Icons.Filled.Close else Icons.Filled.Checklist,
                        contentDescription = null,
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        stringResource(
                            if (selectionMode) {
                                R.string.partition_done
                            } else {
                                R.string.partition_select
                            }
                        )
                    )
                }
            }
        }
    }
}

@Composable
private fun PartitionFilterChip(
    selected: Boolean,
    onClick: () -> Unit,
    label: String,
    enabled: Boolean,
) {
    FilterChip(
        selected = selected,
        onClick = onClick,
        enabled = enabled,
        label = { Text(label) },
        leadingIcon = if (selected) {
            {
                YukiIcon(
                    Icons.Filled.Check,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp),
                )
            }
        } else {
            null
        },
        shape = if (isExpressiveUi) CircleShape else FilterChipDefaultsShape,
    )
}

private val FilterChipDefaultsShape: RoundedCornerShape
    get() = RoundedCornerShape(8.dp)

@Composable
private fun PartitionSelectionCard(
    selectedCount: Int,
    selectableVisible: List<PartitionInfo>,
    selectedPartitions: Set<String>,
    enabled: Boolean,
    onSelectionChange: (Set<String>) -> Unit,
    onSelectBootSet: () -> Unit,
    onBackup: () -> Unit,
) {
    val selectedVisible = selectableVisible.count { it.name in selectedPartitions }
    val checkboxState = when {
        selectableVisible.isEmpty() || selectedVisible == 0 -> ToggleableState.Off
        selectedVisible == selectableVisible.size -> ToggleableState.On
        else -> ToggleableState.Indeterminate
    }
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer
        ),
        shape = partitionCardShape(),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TriStateCheckbox(
                    state = checkboxState,
                    enabled = enabled && selectableVisible.isNotEmpty(),
                    onClick = {
                        val visibleNames = selectableVisible.mapTo(
                            mutableSetOf(),
                            PartitionInfo::name,
                        )
                        onSelectionChange(
                            if (checkboxState == ToggleableState.On) {
                                selectedPartitions - visibleNames
                            } else {
                                selectedPartitions + visibleNames
                            }
                        )
                    },
                )
                Text(
                    text = stringResource(
                        R.string.partition_selected_count,
                        selectedCount,
                    ),
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
                TextButton(
                    onClick = { onSelectionChange(emptySet()) },
                    enabled = enabled && selectedCount > 0,
                ) {
                    Text(stringResource(R.string.partition_clear_selection))
                }
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                AssistChip(
                    onClick = {
                        onSelectionChange(
                            selectedPartitions + selectableVisible.map(PartitionInfo::name)
                        )
                    },
                    enabled = enabled && selectableVisible.isNotEmpty(),
                    label = { Text(stringResource(R.string.partition_select_visible)) },
                    leadingIcon = {
                        YukiIcon(
                            Icons.Filled.DoneAll,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                        )
                    },
                )
                AssistChip(
                    onClick = onSelectBootSet,
                    enabled = enabled,
                    label = { Text(stringResource(R.string.partition_select_boot_set)) },
                    leadingIcon = {
                        YukiIcon(
                            Icons.Filled.Storage,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                        )
                    },
                )
            }
            Button(
                onClick = onBackup,
                enabled = enabled && selectedCount > 0,
                modifier = Modifier.fillMaxWidth(),
            ) {
                YukiIcon(Icons.Filled.Download, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.partition_batch_backup))
            }
            Text(
                text = stringResource(R.string.partition_manifest_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        }
    }
}

@Composable
private fun SlotInfoCard(
    slotInfo: SlotInfo,
    selectedSlot: String?,
    enabled: Boolean,
    onSlotChange: (String?) -> Unit,
) {
    PartitionSurfaceCard {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                text = stringResource(R.string.partition_slot_info),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (isExpressiveUi) FontWeight.Normal else FontWeight.Bold,
                color = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
            )
            InfoRow(
                label = stringResource(R.string.partition_device_type),
                value = stringResource(
                    if (slotInfo.isAbDevice) {
                        R.string.partition_ab_device
                    } else {
                        R.string.partition_a_only_device
                    }
                ),
            )
            if (slotInfo.isAbDevice) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    slotInfo.currentSlot?.let { current ->
                        PartitionFilterChip(
                            selected = selectedSlot == current,
                            onClick = { onSlotChange(current) },
                            label = stringResource(
                                R.string.partition_current_slot_value,
                                current,
                            ),
                            enabled = enabled,
                        )
                    }
                    slotInfo.otherSlot?.let { other ->
                        PartitionFilterChip(
                            selected = selectedSlot == other,
                            onClick = { onSlotChange(other) },
                            label = stringResource(
                                R.string.partition_other_slot_value,
                                other,
                            ),
                            enabled = enabled,
                        )
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun PartitionCard(
    partition: PartitionInfo,
    modifier: Modifier = Modifier,
    index: Int,
    count: Int,
    isSelected: Boolean,
    multiSelectMode: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    val motion = LocalYukiMotion.current
    val shape = if (isExpressiveUi) {
        ListItemDefaults.segmentedShapes(index, count).shape
    } else {
        RoundedCornerShape(12.dp)
    }
    Card(
        modifier = modifier
            .fillMaxWidth()
            .then(
                if (isExpressiveUi) {
                    Modifier.padding(vertical = ListItemDefaults.SegmentedGap / 2)
                } else {
                    Modifier
                }
            )
            .clip(shape)
            .combinedClickable(
                enabled = enabled,
                onClick = onClick,
                onLongClick = onLongClick,
            )
            .defaultMinSize(
                minHeight = if (isExpressiveUi) ExpressiveListGroupMinHeight else 0.dp
            ),
        colors = if (isSelected) {
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.primaryContainer
            )
        } else {
            getCardColors(
                if (isExpressiveUi) {
                    MaterialTheme.colorScheme.surfaceContainer
                } else {
                    MaterialTheme.colorScheme.surfaceContainerLow
                }
            )
        },
        elevation = CardDefaults.cardElevation(
            defaultElevation = if (isExpressiveUi) 0.dp else CardConfig.cardElevation
        ),
        border = if (partition.isDangerous) {
            BorderStroke(1.dp, MaterialTheme.colorScheme.error)
        } else {
            null
        },
        shape = shape,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            AnimatedVisibility(
                visible = multiSelectMode,
                enter = expandHorizontally(motion.spatial()) + fadeIn(motion.effects()),
                exit = shrinkHorizontally(motion.spatial(expanding = false)) + fadeOut(motion.effects()),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(
                        checked = isSelected,
                        onCheckedChange = null,
                        enabled = enabled && !partition.excludeFromBatch,
                    )
                    Spacer(Modifier.width(12.dp))
                }
            }
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Text(
                        text = partition.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = if (isExpressiveUi) {
                            FontWeight.Normal
                        } else {
                            FontWeight.Bold
                        },
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    if (partition.isDangerous) {
                        YukiIcon(
                            Icons.Filled.Warning,
                            contentDescription = stringResource(
                                R.string.partition_dangerous_warning
                            ),
                            tint = MaterialTheme.colorScheme.error,
                            modifier = Modifier.size(18.dp),
                        )
                    }
                    if (partition.excludeFromBatch) {
                        YukiIcon(
                            Icons.Filled.Block,
                            contentDescription = stringResource(
                                R.string.partition_batch_excluded
                            ),
                            tint = MaterialTheme.colorScheme.outline,
                            modifier = Modifier.size(17.dp),
                        )
                    }
                }
                Text(
                    text = stringResource(
                        R.string.partition_type_and_size,
                        stringResource(
                            if (partition.isLogical) {
                                R.string.partition_type_logical
                            } else {
                                R.string.partition_type_physical
                            }
                        ),
                        formatSize(partition.size),
                    ),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.width(12.dp))
            YukiIcon(
                imageVector = if (partition.isLogical) {
                    Icons.Filled.Layers
                } else {
                    Icons.Filled.Storage
                },
                contentDescription = null,
                tint = if (isSelected) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
    }
}

@Composable
private fun EmptyPartitionCard() {
    PartitionSurfaceCard {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            YukiIcon(
                Icons.Filled.Search,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(32.dp),
            )
            Text(
                text = stringResource(R.string.partition_empty_result),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.4f),
        )
        SelectionContainer(modifier = Modifier.weight(0.6f)) {
            Text(
                text = value,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                textAlign = TextAlign.End,
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

@Composable
private fun PartitionActionDialog(
    partition: PartitionInfo,
    targetSlot: String?,
    enabled: Boolean,
    showRamdiskEditor: Boolean,
    onDismiss: () -> Unit,
    onBackup: () -> Unit,
    onEditRamdisk: () -> Unit,
    onFlashAk3: () -> Unit,
    onManageAvb: (resolvedBlockDevice: String) -> Unit,
    onFlash: (resolvedBlockDevice: String) -> Unit,
) {
    val context = LocalContext.current
    var blockDevice by remember(partition.name, targetSlot) {
        mutableStateOf(partition.blockDevice)
    }
    var resolvingDevice by remember(partition.name, targetSlot) {
        mutableStateOf(blockDevice.isBlank())
    }
    LaunchedEffect(partition.name, targetSlot) {
        if (blockDevice.isBlank()) {
            blockDevice = runCatching {
                PartitionManagerHelper.getPartitionBlockDevice(
                    context,
                    partition.name,
                    targetSlot,
                )
            }.getOrDefault("")
            resolvingDevice = false
        }
    }

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            YukiIcon(
                if (partition.isLogical) Icons.Filled.Layers else Icons.Filled.Storage,
                contentDescription = null,
            )
        },
        title = { Text(partition.name) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(9.dp)) {
                InfoRow(
                    stringResource(R.string.partition_info_type),
                    stringResource(
                        if (partition.isLogical) {
                            R.string.partition_type_logical
                        } else {
                            R.string.partition_type_physical
                        }
                    ),
                )
                InfoRow(
                    stringResource(R.string.partition_info_size),
                    formatSize(partition.size),
                )
                InfoRow(
                    stringResource(R.string.partition_info_slot),
                    when {
                        resolvingDevice -> stringResource(R.string.partition_resolving)
                        blockDevice.isBlank() -> stringResource(R.string.partition_unknown)
                        else -> partitionSlotSuffix(blockDevice) ?: "/"
                    },
                )
                InfoRow(
                    stringResource(R.string.partition_info_device),
                    when {
                        resolvingDevice -> stringResource(R.string.partition_resolving)
                        blockDevice.isBlank() -> stringResource(R.string.partition_unknown)
                        else -> blockDevice
                    },
                )
                if (partition.isDangerous) {
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        ),
                    ) {
                        Row(
                            modifier = Modifier.padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            YukiIcon(
                                Icons.Filled.Warning,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.error,
                            )
                            Text(
                                text = stringResource(
                                    R.string.partition_dangerous_warning
                                ),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onErrorContainer,
                            )
                        }
                    }
                } else {
                    HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
                }
                TextButton(
                    onClick = onBackup,
                    enabled = enabled,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    YukiIcon(Icons.Filled.Upload, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.partition_backup_to_file))
                }
                if (showRamdiskEditor) {
                    TextButton(
                        onClick = onEditRamdisk,
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        YukiIcon(Icons.Filled.Edit, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.partition_edit_ramdisk))
                    }
                }
                if (isAvbControlPartition(partition.name)) {
                    TextButton(
                        onClick = { onManageAvb(blockDevice) },
                        enabled = enabled && !resolvingDevice && blockDevice.isNotBlank(),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        YukiIcon(Icons.Filled.Edit, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.partition_avb_title))
                    }
                }
                if (partition.name.removeSuffix("_a").removeSuffix("_b") == "boot") {
                    TextButton(
                        onClick = onFlashAk3,
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        YukiIcon(Icons.Filled.Memory, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.partition_flash_ak3))
                    }
                }
                TextButton(
                    onClick = { onFlash(blockDevice) },
                    enabled = enabled && !resolvingDevice && blockDevice.isNotBlank(),
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.textButtonColors(
                        contentColor = MaterialTheme.colorScheme.error
                    ),
                ) {
                    YukiIcon(Icons.Filled.Download, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.partition_flash_image))
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}

@Composable
private fun FlashPreflightDialog(
    staged: StagedFlashImage,
    enabled: Boolean,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            YukiIcon(
                Icons.Filled.Warning,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.error,
            )
        },
        title = { Text(stringResource(R.string.partition_flash_preflight)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    text = stringResource(R.string.partition_flash_preflight_desc),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                InfoRow(
                    stringResource(R.string.partition_image_file),
                    staged.displayName,
                )
                InfoRow(
                    stringResource(R.string.partition_image_size),
                    formatSize(staged.size),
                )
                InfoRow(
                    stringResource(R.string.partition_target),
                    staged.partition.name,
                )
                InfoRow(
                    stringResource(R.string.partition_info_slot),
                    partitionSlotSuffix(staged.partition.blockDevice) ?: "/",
                )
                InfoRow(
                    stringResource(R.string.partition_capacity),
                    formatSize(staged.partition.size),
                )
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer
                    ),
                ) {
                    Text(
                        text = if (staged.partition.isDangerous) {
                            stringResource(
                                R.string.partition_dangerous_flash_warning,
                                staged.partition.name,
                                staged.partition.name,
                            )
                        } else {
                            stringResource(
                                R.string.partition_flash_warning,
                                staged.partition.name,
                            )
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        modifier = Modifier.padding(12.dp),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = onConfirm,
                enabled = enabled,
                colors = ButtonDefaults.textButtonColors(
                    contentColor = MaterialTheme.colorScheme.error
                ),
            ) {
                Text(stringResource(R.string.partition_confirm_flash))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = enabled) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}

@Composable
private fun Ak3PreflightDialog(
    staged: StagedAk3Package,
    selectedSlot: String?,
    enabled: Boolean,
    onDismiss: () -> Unit,
    onConfirm: (useMkbootfs: Boolean) -> Unit,
) {
    var useMkbootfs by rememberSaveable(staged.cacheFile.absolutePath) {
        mutableStateOf(false)
    }
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            YukiIcon(
                Icons.Filled.Warning,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.error,
            )
        },
        title = { Text(stringResource(R.string.partition_ak3_preflight)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                InfoRow(
                    stringResource(R.string.partition_ak3_package),
                    staged.displayName,
                )
                InfoRow(
                    stringResource(R.string.partition_image_size),
                    formatSize(staged.size),
                )
                InfoRow(
                    stringResource(R.string.partition_ak3_kernel),
                    staged.info.kernelName.ifBlank {
                        stringResource(R.string.partition_unknown)
                    },
                )
                InfoRow(
                    stringResource(R.string.partition_ak3_devices),
                    staged.info.devices.takeIf(List<String>::isNotEmpty)
                        ?.joinToString()
                        ?: stringResource(R.string.partition_ak3_not_declared),
                )
                InfoRow(
                    stringResource(R.string.partition_info_slot),
                    selectedSlot ?: "/",
                )
                staged.info.packageSlotPolicy?.let { policy ->
                    InfoRow(
                        stringResource(R.string.partition_ak3_package_slot_policy),
                        policy,
                    )
                }
                Ak3MkbootfsOption(
                    checked = useMkbootfs,
                    enabled = enabled,
                    onCheckedChange = { useMkbootfs = it },
                )
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer
                    ),
                ) {
                    Text(
                        text = stringResource(R.string.partition_ak3_trust_warning),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        modifier = Modifier.padding(12.dp),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(useMkbootfs) },
                enabled = enabled,
                colors = ButtonDefaults.textButtonColors(
                    contentColor = MaterialTheme.colorScheme.error
                ),
            ) {
                Text(stringResource(R.string.partition_ak3_flash_now))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = enabled) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}

@Composable
private fun Ak3MkbootfsOption(
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    val title = stringResource(R.string.partition_ak3_use_mkbootfs)
    val summary = stringResource(R.string.partition_ak3_use_mkbootfs_summary)

    if (isExpressiveUi) {
        Card(
            onClick = { onCheckedChange(!checked) },
            enabled = enabled,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.large,
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainer),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(3.dp),
                ) {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Normal,
                    )
                    Text(
                        text = summary,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                YukiSwitch(
                    checked = checked,
                    onCheckedChange = onCheckedChange,
                    enabled = enabled,
                )
            }
        }
    } else {
        HorizontalDivider(modifier = Modifier.padding(vertical = 2.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            YukiSwitch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled,
            )
        }
    }
}

@Composable
private fun PartitionSurfaceCard(
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = CardDefaults.cardElevation(
            defaultElevation = if (isExpressiveUi) 0.dp else CardConfig.cardElevation
        ),
        shape = partitionCardShape(),
    ) {
        content()
    }
}

@Composable
private fun partitionCardShape() = if (isExpressiveUi) {
    MaterialTheme.shapes.extraLarge
} else {
    RoundedCornerShape(12.dp)
}

private fun Set<String>.toggle(value: String): Set<String> {
    return if (value in this) this - value else this + value
}
