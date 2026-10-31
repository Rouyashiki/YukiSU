package com.anatdx.yukisu.ui.activity.component

import android.annotation.SuppressLint
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.unit.dp
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavHostController
import androidx.navigation.compose.currentBackStackEntryAsState
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.spec.RouteOrDirection
import com.ramcosta.composedestinations.utils.isRouteOnBackStackAsState
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import com.anatdx.yukisu.ui.MainActivity
import com.anatdx.yukisu.ui.activity.util.*
import com.anatdx.yukisu.ui.screen.BottomBarDestination
import com.anatdx.yukisu.ui.theme.CardStyleProvider.getBarColor
import com.anatdx.yukisu.ui.util.*

@SuppressLint("ContextCastToActivity")
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BottomBar(navController: NavHostController, vertical: Boolean = false) {
    val navigator = navController.rememberDestinationsNavigator()
    val navigationLeaveGuard = LocalNavigationLeaveGuard.current
    val currentRoute = navController.currentBackStackEntryAsState().value?.destination?.route
    val isFullFeatured by AppData.DataRefreshManager.isFullFeatured.collectAsState()
    val cardColor = getBarColor(MaterialTheme.colorScheme.surfaceContainer)
    val activity = LocalContext.current as MainActivity
    val settings by activity.settingsStateFlow.collectAsState()

    val isHideOtherInfo = settings.isHideOtherInfo

    // 收集计数数据
    val superuserCount by AppData.DataRefreshManager.superuserCount.collectAsState()
    val moduleCount by AppData.DataRefreshManager.moduleCount.collectAsState()
    val pluginCount by AppData.DataRefreshManager.pluginCount.collectAsState()

    fun badgeCountOf(destination: BottomBarDestination) = when (destination) {
        BottomBarDestination.SuperUser -> superuserCount
        BottomBarDestination.Module -> moduleCount
        BottomBarDestination.Plugin -> pluginCount
        else -> 0
    }

    val destinations = BottomBarDestination.entries.filter {
        isFullFeatured || !it.rootRequired
    }
    val containerColor = cardColor

    fun navigate(destination: BottomBarDestination, selected: Boolean) {
        navigationLeaveGuard.navigateOrIntercept(currentRoute) {
            if (selected) navigator.popBackStack(destination.direction, false)
            navigator.navigate(destination.direction) {
                popUpTo(NavGraphs.root) { saveState = true }
                launchSingleTop = true
                restoreState = true
            }
        }
    }

    if (vertical) {
        NavigationRail(
            modifier = Modifier.fillMaxHeight().width(96.dp).verticalScroll(rememberScrollState()),
            containerColor = containerColor,
            windowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Vertical + WindowInsetsSides.Start),
        ) {
            destinations.forEach { destination ->
                val selected by navController.isRouteOnBackStackAsState(destination.direction)
                NavigationRailItem(
                    selected = selected,
                    onClick = { navigate(destination, selected) },
                    icon = { DestinationIcon(destination, badgeCountOf(destination), !isHideOtherInfo) },
                    label = { Text(stringResource(destination.label), style = MaterialTheme.typography.labelMedium) },
                )
            }
        }
    } else {
        ShortNavigationBar(
            containerColor = containerColor,
            windowInsets = WindowInsets.navigationBars.only(
                WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
            ),
            arrangement = ShortNavigationBarArrangement.EqualWeight
        ) {
            destinations.forEach { destination ->
                val selected by navController.isRouteOnBackStackAsState(destination.direction)
                val badgeCount = badgeCountOf(destination)
                ShortNavigationBarItem(
                    selected = selected,
                    onClick = { navigate(destination, selected) },
                    icon = {
                        DestinationIcon(destination, badgeCount, !isHideOtherInfo)
                    },
                    label = {
                        Text(
                            stringResource(destination.label),
                            style = MaterialTheme.typography.labelMedium
                        )
                    }
                )
            }
        }
    }
}

@Composable
private fun DestinationIcon(
    destination: BottomBarDestination,
    badgeCount: Int,
    showBadge: Boolean
) {
    val icon: @Composable () -> Unit = {
        Icon(
            painter = painterResource(destination.icon),
            contentDescription = null
        )
    }

    if (badgeCount <= 0 || !showBadge) {
        icon()
        return
    }

    BadgedBox(
        badge = {
            Badge(containerColor = MaterialTheme.colorScheme.secondary) {
                Text(badgeCount.toString(), style = MaterialTheme.typography.labelSmall)
            }
        }
    ) {
        icon()
    }
}
