package org.rouyashiki.yukisu.ui.component

import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.animation.rememberSplineBasedDecay
import org.rouyashiki.yukisu.ui.motion.LocalYukiMotion
import org.rouyashiki.yukisu.ui.theme.CardStyleProvider.getTopAppBarColors
import org.rouyashiki.yukisu.ui.theme.LocalCompactWindow

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun rememberYukiTopAppBarScrollBehavior(
    state: TopAppBarState = rememberTopAppBarState(),
): TopAppBarScrollBehavior {
    val motion = LocalYukiMotion.current
    return if (!LocalCompactWindow.current) {
        TopAppBarDefaults.exitUntilCollapsedScrollBehavior(
            state = state,
            snapAnimationSpec = motion.spatial(),
            flingAnimationSpec = if (motion.enabled) rememberSplineBasedDecay<Float>() else null,
        )
    } else {
        TopAppBarDefaults.pinnedScrollBehavior(state)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun YukiLargeTopAppBar(
    title: @Composable () -> Unit,
    navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {},
    colors: TopAppBarColors = getTopAppBarColors(),
    windowInsets: WindowInsets = TopAppBarDefaults.windowInsets,
    scrollBehavior: TopAppBarScrollBehavior? = null,
) {
    if (LocalCompactWindow.current) {
        TopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            actions = actions,
            colors = colors,
            windowInsets = windowInsets,
        )
    } else {
        LargeFlexibleTopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            actions = actions,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    }
}
