package org.rouyashiki.yukisu.ui.util

import androidx.compose.runtime.compositionLocalOf

val LocalSnackbarHost = compositionLocalOf<SnackbarController> {
    error("CompositionLocal LocalSnackbarController not present")
}

class NavigationLeaveGuard {
    private data class Registration(
        val owner: Any,
        val route: String,
        val interceptor: (navigate: () -> Unit, onIntercepted: () -> Unit) -> Unit,
    )

    private var registration: Registration? = null

    fun register(
        owner: Any,
        route: String,
        interceptor: (navigate: () -> Unit, onIntercepted: () -> Unit) -> Unit,
    ) {
        registration = Registration(owner, route, interceptor)
    }

    fun unregister(owner: Any) {
        if (registration?.owner === owner) {
            registration = null
        }
    }

    fun navigateOrIntercept(route: String?, onIntercepted: () -> Unit = {}, navigate: () -> Unit) {
        val current = registration
        if (current != null && current.route == route) {
            current.interceptor(navigate, onIntercepted)
        } else {
            navigate()
        }
    }
}

val LocalNavigationLeaveGuard = compositionLocalOf<NavigationLeaveGuard> {
    error("CompositionLocal LocalNavigationLeaveGuard not present")
}
