package com.anatdx.yukisu.ui.activity.util

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavBackStackEntryInfo
import androidx.navigationevent.NavigationEvent
import androidx.navigationevent.NavigationEventDispatcher
import androidx.navigationevent.NavigationEventDispatcherOwner
import androidx.navigationevent.NavigationEventHandler
import androidx.navigationevent.NavigationEventHistory
import androidx.navigationevent.NavigationEventInfo
import androidx.navigationevent.NavigationEventInput
import androidx.navigationevent.compose.LocalNavigationEventDispatcherOwner
import androidx.navigationevent.compose.rememberNavigationEventDispatcherOwner
import com.anatdx.yukisu.ui.util.LocalNavigationLeaveGuard
import com.anatdx.yukisu.ui.util.NavigationLeaveGuard

internal data class BackTarget(val id: String, val route: String?)

internal interface BackGestureObserver {
    fun onStarted(event: NavigationEvent)
    fun onProgressed(event: NavigationEvent)
    fun onCompleted()
    fun onCancelled()
}

internal class ForwardedBackInput(private val gestureObserver: BackGestureObserver? = null) : NavigationEventInput() {
    private var attached = false
    private var started = false
    private var pageGesture = false
    var onEnabledChanged: (Boolean) -> Unit = {}
    var onHistoryUpdated: (NavigationEventHistory) -> Unit = {}

    override fun onAdded(dispatcher: NavigationEventDispatcher) { attached = true }
    override fun onRemoved() {
        if (pageGesture) gestureObserver?.onCancelled()
        pageGesture = false
        attached = false
        started = false
        onEnabledChanged(false)
    }
    override fun onHasEnabledHandlersChanged(hasEnabledHandlers: Boolean) = onEnabledChanged(hasEnabledHandlers)
    override fun onHistoryChanged(history: NavigationEventHistory) = onHistoryUpdated(history)

    fun start(event: NavigationEvent, pageGesture: Boolean = false) {
        if (!attached) return
        this.pageGesture = pageGesture
        if (pageGesture) gestureObserver?.onStarted(event)
        dispatchOnBackStarted(event)
        started = true
    }

    fun progress(event: NavigationEvent) {
        if (attached && started) {
            if (pageGesture) gestureObserver?.onProgressed(event)
            dispatchOnBackProgressed(event)
        }
    }

    fun complete() {
        if (!attached) return
        if (pageGesture) gestureObserver?.onCompleted()
        dispatchOnBackCompleted()
        started = false
        pageGesture = false
    }

    fun cancel() {
        if (attached && started) {
            if (pageGesture) gestureObserver?.onCancelled()
            dispatchOnBackCancelled()
        }
        started = false
        pageGesture = false
    }
}

internal class GuardedBackHandler(
    private val input: ForwardedBackInput,
    private val leaveGuard: NavigationLeaveGuard,
    private val predictiveBackEnabled: () -> Boolean = { false },
    private val currentTarget: () -> BackTarget?,
) : NavigationEventHandler<NavigationEventInfo>(NavigationEventInfo.None, isBackEnabled = false) {
    private var revision = 0
    private var pendingTarget: BackTarget? = null
    private var gestureTarget: BackTarget? = null
    private var ignoredGesture = false
    private var hostStarted = false
    private var childEnabled = false

    init {
        input.onEnabledChanged = {
            childEnabled = it
            isBackEnabled = hostStarted && childEnabled
        }
        input.onHistoryUpdated = { history ->
            val index = history.currentIndex
            val entries = history.mergedHistory
            setInfo(
                currentInfo = entries.getOrNull(index) ?: NavigationEventInfo.None,
                backInfo = if (index >= 0) entries.take(index) else emptyList(),
                forwardInfo = if (index >= 0) entries.drop(index + 1) else emptyList(),
            )
        }
    }

    override fun onBackStarted(event: NavigationEvent) {
        ignoredGesture = pendingTarget != null
        if (ignoredGesture) return
        cancelPending()
        gestureTarget = currentTarget()
        if (currentInfo !is NavBackStackEntryInfo || predictiveBackEnabled()) {
            input.start(event, pageGesture = currentInfo is NavBackStackEntryInfo)
        }
    }

    override fun onBackProgressed(event: NavigationEvent) {
        onTargetChanged()
        if (!ignoredGesture && pendingTarget == null) input.progress(event)
    }

    override fun onBackCancelled() {
        if (ignoredGesture) {
            ignoredGesture = false
        } else if (pendingTarget == null) {
            cancelPending()
        }
    }

    override fun onBackCompleted() {
        onTargetChanged()
        if (ignoredGesture) {
            ignoredGesture = false
            return
        }
        if (pendingTarget != null) return
        if (currentInfo !is NavBackStackEntryInfo) {
            gestureTarget = null
            input.complete()
            return
        }
        val target = currentTarget() ?: return cancelPending()
        val requestRevision = ++revision
        pendingTarget = target
        fun finish(allowed: Boolean) {
            if (revision != requestRevision || pendingTarget != target) return
            pendingTarget = null
            gestureTarget = null
            revision++
            if (allowed && currentTarget()?.id == target.id) input.complete() else input.cancel()
        }
        leaveGuard.navigateOrIntercept(target.route, onIntercepted = { finish(false) }) {
            finish(true)
        }
    }

    fun cancelPending() {
        revision++
        pendingTarget = null
        gestureTarget = null
        input.cancel()
    }

    fun onTargetChanged() {
        val target = pendingTarget ?: gestureTarget
        if (target != null && target.id != currentTarget()?.id) {
            val ignoreTerminal = pendingTarget == null && gestureTarget != null
            cancelPending()
            if (ignoreTerminal) ignoredGesture = true
        }
    }

    fun setHostStarted(started: Boolean) {
        hostStarted = started
        if (!started) cancelPending()
        isBackEnabled = hostStarted && childEnabled
    }
}

@Composable
internal fun rememberGuardedNavigationDispatcher(
    navController: NavHostController,
    predictiveBackEnabled: Boolean,
    gestureObserver: BackGestureObserver? = null,
): NavigationEventDispatcherOwner {
    val parent = checkNotNull(LocalNavigationEventDispatcherOwner.current).navigationEventDispatcher
    val owner = rememberNavigationEventDispatcherOwner(parent = null)
    val dispatcher = owner.navigationEventDispatcher
    val leaveGuard = LocalNavigationLeaveGuard.current
    val input = remember(dispatcher, gestureObserver) { ForwardedBackInput(gestureObserver) }
    val enabled = rememberUpdatedState(predictiveBackEnabled)
    val handler = remember(input, leaveGuard, navController) {
        GuardedBackHandler(input, leaveGuard, predictiveBackEnabled = { enabled.value }) {
            navController.currentBackStackEntry?.let { BackTarget(it.id, it.destination.route) }
        }
    }

    LifecycleStartEffect(handler) {
        handler.setHostStarted(true)
        onStopOrDispose {
            handler.setHostStarted(false)
        }
    }
    DisposableEffect(parent, dispatcher, handler, input) {
        dispatcher.addInput(input)
        parent.addHandler(handler)
        onDispose {
            handler.cancelPending()
            handler.remove()
            dispatcher.removeInput(input)
        }
    }
    LaunchedEffect(navController, handler) {
        navController.currentBackStackEntryFlow.collect { handler.onTargetChanged() }
    }
    return owner
}
