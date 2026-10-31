package com.anatdx.yukisu.ui.kasumi

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getBarColor
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingsControlGroup

@Composable
internal fun kasumiCardShape(): Shape =
    MaterialTheme.shapes.large

@Composable
internal fun KasumiControlGroup(
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Only,
    content: @Composable ColumnScope.() -> Unit,
) {
    SettingsControlGroup(groupPosition = position, content = content)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun KasumiTopBar(
    onBack: () -> Unit,
    onRefresh: () -> Unit,
) {
    val title: @Composable () -> Unit = {
        Text(
            stringResource(R.string.kasumi_title),
            fontWeight = FontWeight.Normal,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
    val navigation: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
        }
    }
    val refresh: @Composable RowScope.() -> Unit = {
        IconButton(onClick = onRefresh) {
            YukiIcon(Icons.Filled.Refresh, stringResource(R.string.kasumi_rules_refresh))
        }
    }
    TopAppBar(
        title = title,
        navigationIcon = navigation,
        actions = refresh,
        colors = getTopAppBarColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
            scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
        windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
    )
}

@Composable
internal fun KasumiLogActions(content: @Composable RowScope.() -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun KasumiTabs(selected: KasumiTab, onSelect: (KasumiTab) -> Unit) {
    PrimaryScrollableTabRow(
        selectedTabIndex = selected.ordinal,
        edgePadding = 16.dp,
        containerColor = getBarColor(MaterialTheme.colorScheme.surfaceContainerLow),
    ) {
        KasumiTab.entries.forEach { tab ->
            Tab(selected = selected == tab, onClick = { onSelect(tab) },
                text = { Text(stringResource(tab.displayNameRes)) })
        }
    }
}
