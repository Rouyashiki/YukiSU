package org.rouyashiki.yukisu.ui.activity.util

import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalView
import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventHandler
import androidx.navigationevent.NavigationEventInfo
import androidx.navigationevent.compose.LocalNavigationEventDispatcherOwner
import androidx.navigationevent.compose.rememberNavigationEventDispatcherOwner
import androidx.navigationevent.findViewTreeNavigationEventDispatcherOwner

@Composable
internal fun ModalNavigationEventScope(content: @Composable () -> Unit) {
    // A child dispatcher would still share handlers with the page behind the modal.
    val owner = rememberNavigationEventDispatcherOwner(parent = null)
    CompositionLocalProvider(LocalNavigationEventDispatcherOwner provides owner, content = content)
}

internal fun bridgeWindowBackEvents(
    window: NavigationEventDispatcher,
    inherited: NavigationEventDispatcher,
): () -> Unit {
    if (window === inherited) return {}

    val input = ForwardedBackInput()
    val handler = object : NavigationEventHandler<NavigationEventInfo>(
        NavigationEventInfo.None,
        isBackEnabled = true,
    ) {
        override fun onBackStarted(event: NavigationEvent) = input.start(event)
        override fun onBackProgressed(event: NavigationEvent) = input.progress(event)
        override fun onBackCancelled() = input.cancel()
        override fun onBackCompleted() = input.complete()
    }
    inherited.addInput(input)
    window.addHandler(handler)

    return {
        handler.remove()
        input.cancel()
        try {
            inherited.removeInput(input)
        } catch (_: IllegalStateException) {
            // The navigation hierarchy can be disposed before its dialog content.
        }
    }
}

@Composable
internal fun WindowNavigationEventBridge() {
    val inherited = LocalNavigationEventDispatcherOwner.current?.navigationEventDispatcher ?: return
    val view = LocalView.current
    val window = remember(view) {
        view.findViewTreeNavigationEventDispatcherOwner()?.navigationEventDispatcher
    } ?: return

    DisposableEffect(window, inherited) {
        val disconnect = bridgeWindowBackEvents(window, inherited)
        onDispose { disconnect() }
    }
}
