package com.anatdx.yukisu.ui.activity.util

import android.os.SystemClock
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.EnterExitState
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.snap
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.changedToUpIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalGraphicsContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.NavHostController
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.navigationevent.compose.LocalNavigationEventDispatcherOwner
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.theme.BackgroundLayer
import com.anatdx.yukisu.ui.theme.ThemeConfig
import com.ramcosta.composedestinations.animations.NavHostAnimatedDestinationStyle
import com.ramcosta.composedestinations.manualcomposablecalls.ManualComposableCallsBuilder
import com.ramcosta.composedestinations.manualcomposablecalls.composable
import com.ramcosta.composedestinations.rememberNavHostEngine
import com.ramcosta.composedestinations.spec.DestinationStyle
import com.ramcosta.composedestinations.spec.Direction
import com.ramcosta.composedestinations.spec.NavGraphSpec
import com.ramcosta.composedestinations.spec.NavHostEngine
import com.ramcosta.composedestinations.spec.TypedDestinationSpec

internal val LocalPredictiveBackEnabled = staticCompositionLocalOf { false }
private val LocalBackGestureState = staticCompositionLocalOf<BackGestureState?> { null }
private val LocalForwardEntries = staticCompositionLocalOf<MutableMap<String, ForwardPageMotion>?> { null }
private val LocalNavigationBackdrop = staticCompositionLocalOf<NavigationBackdrop?> { null }

@Composable
fun rememberPredictiveBackNavHostEngine(bottomBarRoutes: Set<String>, predictiveBackEnabled: Boolean): NavHostEngine {
    val delegate = rememberNavHostEngine()
    val enabled = rememberUpdatedState(predictiveBackEnabled)
    return remember(delegate, bottomBarRoutes) { PredictiveBackNavHostEngine(delegate, bottomBarRoutes, enabled) }
}

private class PredictiveBackNavHostEngine(
    delegate: NavHostEngine,
    private val bottomBarRoutes: Set<String>,
    private val predictiveBackEnabled: State<Boolean>,
) : NavHostEngine by delegate {
    @Composable
    override fun NavHost(
        modifier: Modifier,
        route: String,
        start: Direction,
        defaultTransitions: NavHostAnimatedDestinationStyle,
        navController: NavHostController,
        builder: NavGraphBuilder.() -> Unit,
    ) {
        YukiNavigationHost(
            navController = navController,
            startDestination = start.route,
            modifier = modifier,
            route = route,
            bottomBarRoutes = bottomBarRoutes,
            predictiveBackEnabled = predictiveBackEnabled.value,
            builder = builder,
        )
    }
}

@Composable
internal fun YukiNavigationHost(
    navController: NavHostController,
    startDestination: String,
    modifier: Modifier = Modifier,
    route: String? = null,
    bottomBarRoutes: Set<String> = emptySet(),
    predictiveBackEnabled: Boolean,
    pageTransitionStyle: PageTransitionStyle = ThemeConfig.pageTransitionStyle,
    builder: NavGraphBuilder.() -> Unit,
) {
    val motion by rememberUpdatedState(PageMotion(LocalYukiMotion.current, pageTransitionStyle))
    val origins = remember { NavigationOriginTracker() }
    val forwardEntries = remember(navController) { mutableMapOf<String, ForwardPageMotion>() }
    val graphics = LocalGraphicsContext.current
    val backdrop = remember(graphics, navController) { NavigationBackdrop(graphics) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val gestures = remember(navController) {
        BackGestureState(
            currentEntryId = { navController.currentBackStackEntry?.id },
            previousEntryId = { navController.previousBackStackEntry?.id },
        )
    }
    val dispatcherOwner = rememberGuardedNavigationDispatcher(navController, predictiveBackEnabled, gestures)
    LaunchedEffect(navController, gestures) {
        navController.visibleEntries.collect { entries ->
            val ids = entries.mapTo(mutableSetOf()) { it.id }
            gestures.retainVisibleEntries(ids)
            forwardEntries.keys.retainAll(ids)
        }
    }
    DisposableEffect(gestures) { onDispose { gestures.clear() } }
    DisposableEffect(backdrop, lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) backdrop.stop()
        }
        lifecycle.addObserver(observer)
        onDispose {
            lifecycle.removeObserver(observer)
            backdrop.dispose()
        }
    }
    LaunchedEffect(backdrop, gestures) {
        snapshotFlow { backdrop.entryFinished && gestures.session == null }.collect { covered ->
            if (covered) backdrop.finishIfCovered(gestureActive = false)
        }
    }
    DisposableEffect(navController, gestures, backdrop) {
        var currentEntry = navController.currentBackStackEntry?.id
        val listener = NavController.OnDestinationChangedListener { controller, _, _ ->
            val nextEntry = controller.currentBackStackEntry?.id
            backdrop.onDestinationChanged(nextEntry)
            if (currentEntry != nextEntry) {
                currentEntry?.let(gestures::freezeEntry)
                currentEntry = nextEntry
            }
        }
        navController.addOnDestinationChangedListener(listener)
        onDispose { navController.removeOnDestinationChangedListener(listener) }
    }

    CompositionLocalProvider(
        LocalPredictiveBackEnabled provides predictiveBackEnabled,
        LocalBackGestureState provides gestures,
        LocalForwardEntries provides forwardEntries,
        LocalNavigationBackdrop provides backdrop,
        LocalNavigationEventDispatcherOwner provides dispatcherOwner,
    ) {
        // Forward entry motion hands ownership to this seekable host before any back gesture starts.
        androidx.navigation.compose.NavHost(
            navController = navController,
            startDestination = startDestination,
            modifier = modifier.fillMaxSize().observeNavigationTaps(origins)
                .onSizeChanged(backdrop::resize)
                .drawWithContent {
                    backdrop.draw(this)
                    drawContent()
                },
            route = route,
            contentAlignment = Alignment.Center,
            enterTransition = {
                if (gestures.session?.matches(initialState.id, targetState.id) == true) EnterTransition.None
                else {
                    val bottomBar = targetState.destination.route in bottomBarRoutes
                    val origin = origins.originFor(initialState.id, targetState.id, SystemClock.uptimeMillis())
                    if (initialState.id != targetState.id && motion.enabled) {
                        val entryMotion = forwardEntries.getOrPut(targetState.id) {
                            ForwardPageMotion(motion, origin, bottomBar)
                        }
                        backdrop.begin(initialState.id, targetState.id, entryMotion)
                    }
                    EnterTransition.None
                }
            },
            exitTransition = {
                if (gestures.session?.matches(initialState.id, targetState.id) == true) ExitTransition.None
                else ExitTransition.KeepUntilTransitionsFinished
            },
            popEnterTransition = {
                forwardEntries.remove(targetState.id)
                origins.originFor(initialState.id, targetState.id, SystemClock.uptimeMillis())
                if (gestures.session?.matches(initialState.id, targetState.id) == true) EnterTransition.None
                else motion.popEnter()
            },
            popExitTransition = {
                if (gestures.session?.matches(initialState.id, targetState.id) == true) ExitTransition.None
                else motion.popExit()
            },
            predictivePopEnterTransition = {
                forwardEntries.remove(targetState.id)
                EnterTransition.None
            },
            predictivePopExitTransition = { ExitTransition.None },
            builder = builder,
        )
    }
}

private fun Modifier.observeNavigationTaps(origins: NavigationOriginTracker): Modifier = pointerInput(origins) {
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
        var tap = true
        do {
            val event = awaitPointerEvent(PointerEventPass.Initial)
            val change = event.changes.firstOrNull { it.id == down.id } ?: break
            if (event.changes.any { it.id != down.id && it.pressed } ||
                (change.position - down.position).getDistance() > viewConfiguration.touchSlop
            ) tap = false
            if (change.changedToUpIgnoreConsumed() && tap) {
                origins.recordTap(change.position.x, change.position.y, size.width, size.height, change.uptimeMillis)
            }
        } while (event.changes.any { it.pressed })
    }
}

fun ManualComposableCallsBuilder.predictiveBackSurfaces(
    navGraph: NavGraphSpec,
    page: @Composable (route: String?, contentModifier: Modifier, content: @Composable () -> Unit) -> Unit,
) {
    navGraph.destinations.forEach { destination ->
        if (destination.style is DestinationStyle.Default || destination.style is DestinationStyle.Animated) {
            predictiveBackSurface(destination, page)
        }
    }
    navGraph.nestedNavGraphs.forEach { predictiveBackSurfaces(it, page) }
}

private fun <T> ManualComposableCallsBuilder.predictiveBackSurface(
    destination: TypedDestinationSpec<T>,
    page: @Composable (route: String?, contentModifier: Modifier, content: @Composable () -> Unit) -> Unit,
) {
    composable(destination) {
        YukiNavigationPage(navBackStackEntry.id) {
            page(navBackStackEntry.destination.route, Modifier) { with(destination) { Content() } }
        }
    }
}

@Composable
internal fun AnimatedVisibilityScope.YukiNavigationPage(entryId: String, content: @Composable () -> Unit) {
    val gestures = LocalBackGestureState.current
    val session = gestures?.session?.takeIf { it.contains(entryId) }
    val outgoing = session?.outgoingId == entryId
    val forwardEntries = LocalForwardEntries.current
    val backdrop = LocalNavigationBackdrop.current
    val pageLayer = remember(backdrop, entryId) { backdrop?.createPage(entryId) }
    val entryMotion = remember(entryId) { forwardEntries?.remove(entryId) }
    DisposableEffect(gestures, entryId, entryMotion) {
        if (entryMotion != null) gestures?.registerEntry(entryId, entryMotion)
        onDispose { if (entryMotion != null) gestures?.unregisterEntry(entryId, entryMotion) }
    }
    val motionEnabled = LocalYukiMotion.current.enabled
    LaunchedEffect(entryMotion, motionEnabled) {
        if (motionEnabled) entryMotion?.animate() else entryMotion?.handOff()
    }
    val progress = key(gestures?.progressKey(entryId)) {
        transition.animateFloat(
            transitionSpec = { if (session != null) predictiveBackSpec() else snap() },
            label = "predictive page progress",
        ) { state ->
            if (session == null) 0f else if (outgoing) {
                if (state == EnterExitState.PostExit) 1f else 0f
            } else {
                if (state == EnterExitState.PreEnter) 0f else 1f
            }
        }
    }
    LaunchedEffect(session, transition.currentState, transition.targetState, transition.isRunning) {
        if (transition.targetState == EnterExitState.Visible && !transition.isRunning &&
            transition.currentState != EnterExitState.PreEnter
        ) {
            gestures?.onPageSettled(entryId)
            backdrop?.onPageSettled(entryId)
        }
    }
    val view = LocalView.current
    var screenShape by remember { mutableStateOf(ScreenShape()) }
    fun transform(): PredictivePageTransform {
        if (session != null) return predictivePageTransform(session, outgoing, progress.value)
        val entry = entryMotion?.transform() ?: ForwardPageTransform()
        return PredictivePageTransform(
            scale = entry.scale,
            offset = entry.offset,
            offsetY = entry.offsetY,
            alpha = entry.alpha,
        )
    }
    Box(
        Modifier.fillMaxSize()
            .onGloballyPositioned { screenShape = ScreenShape.fromInsets(view.rootWindowInsets) }
            .graphicsLayer {
                val current = transform()
                if (pageLayer?.frozen == false) pageLayer.pose = current
                scaleX = current.scale
                scaleY = current.scale
                translationX = size.width * current.offset
                translationY = size.height * current.offsetY
                alpha = current.alpha
                shape = screenShape
                clip = session != null || entryMotion?.finished == false ||
                    transition.isRunning || transition.currentState != transition.targetState
                shadowElevation = if (session != null && outgoing) 16.dp.toPx() * progress.value else 0f
            }
            .drawWithContent {
                if (backdrop != null && pageLayer != null) {
                    backdrop.record(pageLayer, this, screenShape.createOutline(size, layoutDirection, this))
                } else drawContent()
                val dim = if (session != null && !outgoing) {
                    predictivePageTransform(session, outgoing = false, progress.value).dim
                } else 0f
                if (dim > 0f) drawRect(Color.Black, alpha = dim)
            },
    ) {
        if (LocalPredictiveBackEnabled.current) BackgroundLayer()
        content()
    }
}
