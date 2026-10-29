package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import com.anatdx.yukisu.ui.component.rememberYukiTopAppBarScrollBehavior
import com.anatdx.yukisu.ui.component.YukiLargeTopAppBar
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CornerSize
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.automirrored.outlined.OpenInNew
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.outlined.Code
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.Forum
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.OpenSourceLicenseScreenDestination
import com.ramcosta.composedestinations.generated.destinations.OpenSourceLicensesScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray

@Destination<RootGraph>
@Composable
fun AboutScreen(navigator: DestinationsNavigator) {
    val uriHandler = LocalUriHandler.current
    AboutScaffold(stringResource(R.string.about), navigator) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize().padding(padding),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            item {
                Column(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Image(
                        painter = painterResource(R.drawable.ic_about_yukisu),
                        contentDescription = null,
                        modifier = Modifier.size(144.dp),
                        colorFilter = ColorFilter.tint(MaterialTheme.colorScheme.primary),
                    )
                    Text(
                        stringResource(R.string.app_name),
                        style = MaterialTheme.typography.headlineMedium,
                        textAlign = TextAlign.Center,
                    )
                    Text(
                        BuildConfig.VERSION_NAME,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = TextAlign.Center,
                    )
                }
            }
            item {
                AboutListGroup {
                    AboutListItem(
                        title = stringResource(R.string.open_source_project),
                        summary = "GitHub",
                        icon = Icons.Outlined.Code,
                        index = 0, count = 3,
                        external = true,
                        onClick = { uriHandler.openUri("https://github.com/Rouyashiki/YukiSU") },
                    )
                    AboutListItem(
                        title = stringResource(R.string.about_telegram_group),
                        summary = "Telegram",
                        icon = Icons.Outlined.Forum,
                        index = 1, count = 3,
                        external = true,
                        onClick = { uriHandler.openUri("https://t.me/manosaba") },
                    )
                    AboutListItem(
                        title = stringResource(R.string.open_source_licenses),
                        summary = stringResource(R.string.open_source_licenses_summary),
                        icon = Icons.Outlined.Description,
                        index = 2, count = 3,
                        onClick = { navigator.navigate(OpenSourceLicensesScreenDestination) },
                    )
                }
            }
        }
    }
}

@Composable
private fun AboutListGroup(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    if (isExpressiveUi) {
        Column(modifier.fillMaxWidth(), content = content)
    } else {
        Card(
            modifier = modifier.fillMaxWidth(),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation(),
        ) {
            Column(Modifier.padding(vertical = 8.dp), content = content)
        }
    }
}

@Composable
private fun AboutListItem(
    title: String,
    summary: String,
    index: Int,
    count: Int,
    icon: ImageVector? = null,
    external: Boolean = false,
    onClick: () -> Unit,
) {
    val expressive = isExpressiveUi
    ListItem(
        content = { Text(title, style = MaterialTheme.typography.titleMedium) },
        supportingContent = { Text(summary, color = MaterialTheme.colorScheme.onSurfaceVariant) },
        leadingContent = icon?.let { { Icon(it, null, tint = MaterialTheme.colorScheme.onSurfaceVariant) } },
        trailingContent = {
            Icon(
                if (external) Icons.AutoMirrored.Outlined.OpenInNew else Icons.AutoMirrored.Filled.NavigateNext,
                null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
        colors = ListItemDefaults.colors(
            containerColor = if (expressive) {
                MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
            } else {
                Color.Transparent
            },
        ),
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (expressive) {
                    Modifier
                        .padding(horizontal = 6.dp, vertical = ListItemDefaults.SegmentedGap / 2)
                        .clip(ListItemDefaults.segmentedShapes(index, count).shape)
                } else {
                    Modifier
                }
            )
            .clickable(onClick = onClick)
            .defaultMinSize(minHeight = ExpressiveListGroupMinHeight),
    )
}

private data class LicenseText(val title: String, val path: String)

private data class LoadedLicenseText(val title: String, val paragraphs: List<String>)

private data class OpenSourceComponent(
    val id: String,
    val name: String,
    val license: String,
    val version: String,
    val source: String,
    val artifacts: List<String>,
    val texts: List<LicenseText>,
)

private fun parseComponents(json: String): List<OpenSourceComponent> {
    val array = JSONArray(json)
    return List(array.length()) { index ->
        val entry = array.getJSONObject(index)
        val artifacts = entry.getJSONArray("artifacts")
        val texts = entry.getJSONArray("texts")
        OpenSourceComponent(
            id = entry.getString("id"), name = entry.getString("name"),
            license = entry.getString("license"), version = entry.getString("version"),
            source = entry.getString("source"),
            artifacts = List(artifacts.length()) { artifacts.getString(it) },
            texts = List(texts.length()) {
                val text = texts.getJSONObject(it)
                LicenseText(text.getString("title"), text.getString("path"))
            },
        )
    }
}

@Composable
private fun <T> LicenseAsset(
    key: String = "catalog",
    load: suspend (android.content.res.AssetManager) -> T,
    content: @Composable (T) -> Unit,
) {
    val assets = LocalContext.current.assets
    var attempt by remember { mutableIntStateOf(0) }
    val result by produceState<Result<T>?>(null, assets, attempt, key) {
        value = null
        value = try {
            Result.success(withContext(Dispatchers.IO) { load(assets) })
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            Result.failure(IllegalStateException("Cannot read bundled licenses"))
        }
    }
    val current = result
    when {
        current == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            CircularProgressIndicator()
        }
        current.isFailure -> Column(Modifier.padding(16.dp)) {
            Text(stringResource(R.string.open_source_load_error))
            TextButton(onClick = { attempt++ }) { Text(stringResource(R.string.open_source_retry)) }
        }
        else -> content(current.getOrThrow())
    }
}

@Destination<RootGraph>
@Composable
fun OpenSourceLicensesScreen(navigator: DestinationsNavigator) {
    var query by rememberSaveable { mutableStateOf("") }
    AboutScaffold(stringResource(R.string.open_source_licenses), navigator) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            OutlinedTextField(
                value = query, onValueChange = { query = it }, singleLine = true,
                label = { Text(stringResource(R.string.open_source_search)) },
                leadingIcon = { Icon(Icons.Default.Search, null) },
                shape = if (isExpressiveUi) MaterialTheme.shapes.extraLarge else MaterialTheme.shapes.small,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
            )
            LicenseAsset(
                load = { assets -> assets.open("open_source/components.json").bufferedReader().use { parseComponents(it.readText()) } },
            ) { components ->
                val filtered = remember(components, query) {
                    val term = query.trim()
                    components.filter { component ->
                        listOf(component.name, component.license, component.version).plus(component.artifacts)
                            .any { it.contains(term, ignoreCase = true) }
                    }
                }
                if (filtered.isEmpty()) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text(
                            stringResource(R.string.open_source_no_results),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    AboutListGroup(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                        LazyColumn {
                            itemsIndexed(filtered, key = { _, component -> component.id }) { index, component ->
                                AboutListItem(
                                    title = component.name,
                                    summary = listOf(component.version, component.license)
                                        .filter { it.isNotEmpty() }.joinToString(" · "),
                                    index = index,
                                    count = filtered.size,
                                    onClick = {
                                        navigator.navigate(OpenSourceLicenseScreenDestination(component.id))
                                    },
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Destination<RootGraph>
@Composable
fun OpenSourceLicenseScreen(componentId: String, navigator: DestinationsNavigator) {
    val uriHandler = LocalUriHandler.current
    var showArtifacts by rememberSaveable(componentId) { mutableStateOf(false) }
    AboutScaffold(stringResource(R.string.open_source_licenses), navigator) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            LicenseAsset(
                key = componentId,
                load = { assets ->
                    val component = assets.open("open_source/components.json").bufferedReader().use {
                        parseComponents(it.readText()).first { component -> component.id == componentId }
                    }
                    component to component.texts.map { text ->
                        LoadedLicenseText(
                            text.title,
                            assets.open(text.path).bufferedReader().use { it.readText() }
                                .split(Regex("""\n\s*\n""")),
                        )
                    }
                },
            ) { (component, texts) ->
                SelectionContainer {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(16.dp),
                    ) {
                        item {
                            Card(
                                modifier = Modifier.fillMaxWidth(),
                                shape = MaterialTheme.shapes.large,
                                colors = getCardColors(
                                    if (isExpressiveUi) MaterialTheme.colorScheme.surfaceContainer
                                    else MaterialTheme.colorScheme.surfaceContainerLow
                                ),
                            ) {
                                Column(
                                    Modifier.padding(20.dp),
                                    verticalArrangement = Arrangement.spacedBy(12.dp),
                                ) {
                                    Text(component.name, style = MaterialTheme.typography.headlineSmall)
                                    if (component.version.isNotEmpty()) {
                                        Text(
                                            component.version,
                                            style = MaterialTheme.typography.bodyMedium,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                    }
                                    Text(component.license, style = MaterialTheme.typography.bodyMedium)
                                    FilledTonalButton(onClick = { uriHandler.openUri(component.source) }) {
                                        Icon(Icons.AutoMirrored.Outlined.OpenInNew, null, Modifier.size(ButtonDefaults.IconSize))
                                        Spacer(Modifier.width(ButtonDefaults.IconSpacing))
                                        Text(stringResource(R.string.open_source_project))
                                    }
                                    if (component.artifacts.isNotEmpty()) {
                                        TextButton(
                                            onClick = { showArtifacts = !showArtifacts },
                                            modifier = Modifier.yukiExpansionSemantics(showArtifacts),
                                        ) {
                                            Text(stringResource(R.string.open_source_artifacts, component.artifacts.size))
                                            Spacer(Modifier.width(ButtonDefaults.IconSpacing))
                                            YukiExpandArrow(expanded = showArtifacts)
                                        }
                                        YukiExpandableContent(visible = showArtifacts, large = true) {
                                            Text(
                                                component.artifacts.joinToString("\n"),
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            )
                                        }
                                    }
                                }
                            }
                        }
                        texts.forEach { (title, paragraphs) ->
                            item { Spacer(Modifier.height(20.dp)) }
                            itemsIndexed(paragraphs) { index, paragraph ->
                                val shape = MaterialTheme.shapes.large.copy(
                                    topStart = if (index == 0) MaterialTheme.shapes.large.topStart else CornerSize(0.dp),
                                    topEnd = if (index == 0) MaterialTheme.shapes.large.topEnd else CornerSize(0.dp),
                                    bottomStart = if (index == paragraphs.lastIndex) MaterialTheme.shapes.large.bottomStart else CornerSize(0.dp),
                                    bottomEnd = if (index == paragraphs.lastIndex) MaterialTheme.shapes.large.bottomEnd else CornerSize(0.dp),
                                )
                                Column(
                                    Modifier.fillMaxWidth()
                                        .clip(shape)
                                        .background(
                                            (if (isExpressiveUi) MaterialTheme.colorScheme.surfaceContainer
                                            else MaterialTheme.colorScheme.surfaceContainerLow)
                                                .copy(alpha = CardConfig.cardAlpha)
                                        )
                                        .padding(
                                            start = 20.dp, end = 20.dp,
                                            top = if (index == 0) 20.dp else 0.dp,
                                            bottom = if (index == paragraphs.lastIndex) 20.dp else 12.dp,
                                        ),
                                ) {
                                    if (index == 0) {
                                        Text(
                                            title,
                                            style = MaterialTheme.typography.titleMedium,
                                            color = MaterialTheme.colorScheme.primary,
                                        )
                                        Spacer(Modifier.height(16.dp))
                                    }
                                    Text(paragraph, style = MaterialTheme.typography.bodyMedium)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AboutScaffold(
    title: String,
    navigator: DestinationsNavigator,
    content: @Composable (PaddingValues) -> Unit,
) {
    val scrollBehavior = rememberYukiTopAppBarScrollBehavior()
    val background = if (CardConfig.isCustomBackgroundEnabled) {
        MaterialTheme.colorScheme.surfaceContainerLow
    } else {
        MaterialTheme.colorScheme.background
    }
    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            val navigationIcon: @Composable () -> Unit = {
                IconButton(onClick = { navigator.popBackStack() }) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                }
            }
            val colors = getTopAppBarColors(
                containerColor = background,
                scrolledContainerColor = background,
            )
            if (isExpressiveUi) {
                YukiLargeTopAppBar(
                    title = { Text(title) },
                    navigationIcon = navigationIcon,
                    colors = colors,
                    scrollBehavior = scrollBehavior,
                )
            } else {
                TopAppBar(
                    title = { Text(title) },
                    navigationIcon = navigationIcon,
                    colors = colors,
                    scrollBehavior = scrollBehavior,
                )
            }
        },
        content = content,
    )
}
