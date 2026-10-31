package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import android.content.ClipData
import android.content.ClipboardManager
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Archive
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.getSystemService
import androidx.lifecycle.compose.dropUnlessResumed
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.TemplateEditorScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.result.ResultRecipient
import com.ramcosta.composedestinations.result.getOr
import com.anatdx.yukisu.R
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiPullToRefreshBox
import com.anatdx.yukisu.ui.component.clickHapticFeedback
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig.cardAlpha
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.viewmodel.TemplateViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

/**
 * @author weishu
 * @date 2023/10/20.
 */

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun AppProfileTemplateScreen(
    navigator: DestinationsNavigator,
    resultRecipient: ResultRecipient<TemplateEditorScreenDestination, Boolean>
) {
    val viewModel = viewModel<TemplateViewModel>()
    val scope = rememberCoroutineScope()
    val resources = LocalResources.current
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()

    LaunchedEffect(Unit) {
        if (viewModel.templateList.isEmpty()) {
            viewModel.fetchTemplates()
        }
    }

    // handle result from TemplateEditorScreen, refresh if needed
    resultRecipient.onNavResult { result ->
        if (result.getOr { false }) {
            scope.launch { viewModel.fetchTemplates() }
        }
    }

    Scaffold(
        topBar = {
            val context = LocalContext.current
            val clipboardManager = context.getSystemService<ClipboardManager>()
            val showToast = fun(msg: String) {
                scope.launch(Dispatchers.Main) {
                    Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
                }
            }
            TopBar(
                onBack = dropUnlessResumed { navigator.popBackStack() },
                onSync = {
                    scope.launch { viewModel.fetchTemplates(true) }
                },
                onImport = {
                    scope.launch {
                        val clipboardText = clipboardManager?.primaryClip?.getItemAt(0)?.text?.toString()
                        if (clipboardText.isNullOrEmpty()) {
                            showToast(resources.getString(R.string.app_profile_template_import_empty))
                            return@launch
                        }
                        viewModel.importTemplates(
                            clipboardText,
                            {
                                showToast(resources.getString(R.string.app_profile_template_import_success))
                                viewModel.fetchTemplates(false)
                            },
                            showToast
                        )
                    }
                },
                onExport = {
                    scope.launch {
                        viewModel.exportTemplates(
                            {
                                showToast(resources.getString(R.string.app_profile_template_export_empty))
                            }
                        ) { text ->
                            clipboardManager?.setPrimaryClip(ClipData.newPlainText("", text))
                        }
                    }
                },
                scrollBehavior = scrollBehavior
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = {
                    navigator.navigate(
                        TemplateEditorScreenDestination(
                            TemplateViewModel.TemplateInfo(
                                flags = if (Natives.isDefaultNoNewPrivsEnabled()) Natives.FLAG_KSU_NO_NEW_PRIVS else 0L,
                            ),
                            false
                        )
                    )
                },
                icon = { YukiIcon(Icons.Filled.Add, null) },
                text = { Text(stringResource(id = R.string.app_profile_template_create)) },
                contentColor = MaterialTheme.colorScheme.onSecondaryContainer
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
    ) { innerPadding ->
        YukiPullToRefreshBox(
            modifier = Modifier.padding(innerPadding),
            isRefreshing = viewModel.isRefreshing,
            onRefresh = {
                scope.launch { viewModel.fetchTemplates(sync = true) }
            }
        ) {
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .nestedScroll(scrollBehavior.nestedScrollConnection),
                contentPadding = remember {
                    PaddingValues(bottom = 16.dp + 56.dp + 16.dp /* Scaffold Fab Spacing + Fab container height */)
                }
            ) {
                itemsIndexed(
                    items = viewModel.templateList,
                    key = { _, template -> template.id },
                ) { index, template ->
                    TemplateItem(
                        navigator = navigator,
                        template = template,
                        index = index,
                        count = viewModel.templateList.size,
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TemplateItem(
    navigator: DestinationsNavigator,
    template: TemplateViewModel.TemplateInfo,
    index: Int,
    count: Int,
) {
    val expressiveShape = if (count == 1) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(index, count).shape
    }
    ListItem(
        modifier = Modifier
            .then(
                Modifier
                    .padding(
                        horizontal = 16.dp,
                        vertical = ListItemDefaults.SegmentedGap / 2,
                    )
                    .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                    .clip(expressiveShape)
                    .background(
                        MaterialTheme.colorScheme.surfaceContainer.copy(alpha = cardAlpha)
                    )
            )
            .clickable {
                navigator.navigate(TemplateEditorScreenDestination(template, !template.local))
            },
        colors = ListItemDefaults.colors(
            containerColor = Color.Transparent,
        ),
        content = {
            Text(
                text = template.name,
                fontWeight = FontWeight.Normal,
            )
        },
        supportingContent = {
            Column {
                Text(
                    text = "${template.id}${if (template.author.isEmpty()) "" else "@${template.author}"}",
                    style = MaterialTheme.typography.bodySmall,
                    fontSize = MaterialTheme.typography.bodySmall.fontSize,
                )
                Text(template.description)
                FlowRow {
                    LabelText(label = "UID: ${template.uid}")
                    LabelText(label = "GID: ${template.gid}")
                    LabelText(label = template.context)
                    if (template.local) {
                        LabelText(label = "local")
                    } else {
                        LabelText(label = "remote")
                    }
                }
            }
        }
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    onBack: () -> Unit,
    onSync: () -> Unit = {},
    onImport: () -> Unit = {},
    onExport: () -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val colorScheme = MaterialTheme.colorScheme
    val cardColor = colorScheme.surfaceContainerLow
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.settings_profile_template),
            fontWeight = FontWeight.Normal,
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
        }
    }
    val actions: @Composable RowScope.() -> Unit = {
        IconButton(onClick = onSync) {
            YukiIcon(
                Icons.Filled.Sync,
                contentDescription = stringResource(id = R.string.app_profile_template_sync)
            )
        }

        var showDropdown by remember { mutableStateOf(false) }
        IconButton(onClick = { showDropdown = true }) {
            YukiIcon(
                imageVector = Icons.Filled.Archive,
                contentDescription = stringResource(id = R.string.app_profile_import_export)
            )

            DropdownMenu(expanded = showDropdown, onDismissRequest = {
                showDropdown = false
            }, modifier = Modifier.clickHapticFeedback()) {
                DropdownMenuItem(text = {
                    Text(stringResource(id = R.string.app_profile_import_from_clipboard))
                }, onClick = {
                    onImport()
                    showDropdown = false
                })
                DropdownMenuItem(text = {
                    Text(stringResource(id = R.string.app_profile_export_to_clipboard))
                }, onClick = {
                    onExport()
                    showDropdown = false
                })
            }
        }
    }
    val colors = getTopAppBarColors(
        containerColor = cardColor,
        scrolledContainerColor = cardColor
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
        scrollBehavior = scrollBehavior,
    )
}

@Composable
fun LabelText(label: String) {
    Box(
        modifier = Modifier
            .padding(top = 4.dp, end = 4.dp)
            .background(
                MaterialTheme.colorScheme.secondaryContainer,
                shape = MaterialTheme.shapes.small
            )
    ) {
        Text(
            text = label,
            modifier = Modifier.padding(vertical = 2.dp, horizontal = 5.dp),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
