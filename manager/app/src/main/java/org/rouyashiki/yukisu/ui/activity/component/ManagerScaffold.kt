package org.rouyashiki.yukisu.ui.activity.component

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import org.rouyashiki.yukisu.ui.activity.util.AnimatedBottomBar
import org.rouyashiki.yukisu.ui.theme.LocalCompactWindow
import org.rouyashiki.yukisu.ui.theme.ManagerUi

@Composable
fun ManagerScaffold(
    navController: NavHostController,
    showNavigation: Boolean,
    active: Boolean,
    containerColor: Color,
    snackbarHost: @Composable () -> Unit,
    content: @Composable (PaddingValues) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val windowHeight = with(LocalDensity.current) {
            LocalWindowInfo.current.containerSize.height.toDp()
        }
        val compactHeight = windowHeight < 480.dp
        val useRail = maxWidth >= 600.dp
        val bottomInsets = if (active && useRail) {
            WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom)
        } else {
            WindowInsets(0, 0, 0, 0)
        }
        CompositionLocalProvider(LocalCompactWindow provides compactHeight) {
            Row(Modifier.fillMaxSize()) {
                if (active && showNavigation && useRail) {
                    BottomBar(navController, vertical = true)
                }
                Scaffold(
                    modifier = Modifier.weight(1f).windowInsetsPadding(bottomInsets),
                    containerColor = containerColor,
                    snackbarHost = snackbarHost,
                    bottomBar = {
                        if (active && !useRail) {
                            AnimatedBottomBar.AnimatedBottomBarWrapper(
                                showBottomBar = showNavigation,
                                content = { BottomBar(navController) },
                            )
                        }
                    },
                    contentWindowInsets = WindowInsets(0, 0, 0, 0),
                ) { padding ->
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
                        Box(
                            Modifier
                                .then(if (active) Modifier.widthIn(
                                    max = if (showNavigation) ManagerUi.ListMaxWidth else ManagerUi.ContentMaxWidth,
                                ) else Modifier)
                                .fillMaxSize(),
                        ) {
                            content(padding)
                        }
                    }
                }
            }
        }
    }
}
