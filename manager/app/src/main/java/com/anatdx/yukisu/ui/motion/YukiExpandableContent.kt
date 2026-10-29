package com.anatdx.yukisu.ui.motion

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.AnimationVector1D
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.focusGroup
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.LocalContentColor
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.constrainHeight
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiIcon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

internal class HeightAnimation(private val initialHeight: Float? = null) {
    private var animation: Animatable<Float, AnimationVector1D>? = null
    private var target = Float.NaN
    private var preset: MotionPreset? = null
    private var generation = 0
    private var renderedHeight: Float? = null
    private var pendingStart by mutableStateOf<Float?>(null)
    private var boundary = ExpansionBoundary()
    var active by mutableStateOf(false)
        private set
    var shrinking = false
        private set

    fun measure(
        height: Int,
        animate: Boolean,
        motion: YukiMotion,
        role: MotionRole,
        scope: CoroutineScope,
    ): Float {
        val state = animation ?: Animatable(initialHeight ?: height.toFloat()).also { animation = it }
        val newTarget = height.toFloat()
        if (newTarget != target || preset != motion.preset) {
            val start = renderedHeight ?: state.value
            val velocity = state.velocity
            shrinking = newTarget < start
            boundary = ExpansionBoundary()
            target = newTarget
            preset = motion.preset
            val revision = ++generation
            active = animate && motion.enabled && start != newTarget
            val shouldAnimate = active
            val expanding = !shrinking
            pendingStart = start
            scope.launch {
                if (generation != revision) return@launch
                if (shouldAnimate) {
                    // Retarget from the visible boundary while retaining the spring's momentum.
                    if (start != state.value) state.snapTo(start)
                    pendingStart = null
                    state.animateTo(
                        newTarget,
                        motion.spatial(role, expanding = expanding, visibilityThreshold = 0.5f),
                        initialVelocity = velocity,
                    )
                } else {
                    state.snapTo(newTarget)
                    pendingStart = null
                }
                if (generation == revision) active = false
            }
        }
        return if (active && motion.enabled) {
            // Keep layout subscribed to frames even before the animation coroutine starts.
            val animatedHeight = state.value
            pendingStart ?: animatedHeight
        } else newTarget
    }

    fun boundedHeight(value: Float, height: Int, overshoot: Float): Int {
        val rendered = when {
            shrinking && boundary.hasClosed(value - height) -> height
            shrinking -> value.toInt().coerceAtLeast(height)
            else -> expansionHeight(if (height > 0) value / height else 0f, height, overshoot)
        }
        renderedHeight = rendered.toFloat()
        return rendered
    }
}

@Composable
fun YukiExpandableContent(
    visible: Boolean,
    modifier: Modifier = Modifier,
    large: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    val motion = LocalYukiMotion.current
    val scope = rememberCoroutineScope()
    val animation = remember { HeightAnimation(if (visible) null else 0f) }
    var previousVisible by remember { mutableStateOf(visible) }
    val role = if (large) MotionRole.LargeContainer else MotionRole.Container
    val focusManager = LocalFocusManager.current
    var hasFocus by remember { mutableStateOf(false) }
    LaunchedEffect(visible) {
        if (!visible && hasFocus) focusManager.clearFocus()
    }

    if (visible || previousVisible || animation.active) {
        val overshoot = with(LocalDensity.current) {
            (motion.overshootDp * if (large) 1.2f else 1f).dp.toPx()
        }
        Layout(
            modifier = modifier.graphicsLayer { clip = animation.active || !visible },
            content = {
                Column(
                    modifier = Modifier
                        .onFocusChanged { hasFocus = it.hasFocus }
                        .focusProperties {
                            onEnter = { if (!visible) cancelFocusChange() }
                        }
                        .focusGroup()
                        .onPreviewKeyEvent { !visible }
                        .then(if (visible) Modifier else Modifier.clearAndSetSemantics {})
                        .pointerInput(visible) {
                            if (!visible) awaitPointerEventScope {
                                while (true) {
                                    awaitPointerEvent(PointerEventPass.Initial).changes.forEach { it.consume() }
                                }
                            }
                        },
                    content = content,
                )
            },
        ) { measurables, constraints ->
            val placeable = measurables.single().measure(constraints.copy(minHeight = 0))
            val targetHeight = if (visible) placeable.height else 0
            val height = if (isLookingAhead) {
                targetHeight
            } else {
                // A settled parent follows its children's measured animation without adding another spring.
                val rawHeight = animation.measure(
                    targetHeight,
                    animate = visible != previousVisible || animation.active,
                    motion, role, scope,
                )
                previousVisible = visible
                animation.boundedHeight(rawHeight, targetHeight, overshoot)
            }
            layout(placeable.width, constraints.constrainHeight(height)) {
                placeable.placeRelative(0, 0)
            }
        }
    }
}

@Composable
fun YukiExpandArrow(
    expanded: Boolean,
    modifier: Modifier = Modifier,
    tint: Color = LocalContentColor.current,
    contentDescription: String? = null,
) {
    val motion = LocalYukiMotion.current
    val angle by animateFloatAsState(
        if (expanded) 180f else 0f,
        animationSpec = motion.spatial(MotionRole.Arrow, expanding = expanded),
        label = "disclosure arrow",
    )
    YukiIcon(
        Icons.Filled.ExpandMore,
        contentDescription,
        modifier.graphicsLayer { rotationZ = if (motion.enabled) angle else if (expanded) 180f else 0f },
        tint,
    )
}

fun Modifier.yukiExpansionSemantics(expanded: Boolean): Modifier = composed {
    val description = stringResource(if (expanded) R.string.motion_expanded else R.string.motion_collapsed)
    semantics { stateDescription = description }
}

fun Modifier.yukiAnimateContentSize(): Modifier = composed {
    val motion = LocalYukiMotion.current
    val scope = rememberCoroutineScope()
    val animation = remember { HeightAnimation() }
    val overshoot = with(LocalDensity.current) { motion.overshootDp.dp.toPx() }
    graphicsLayer { clip = animation.active }.layout { measurable, constraints ->
        val placeable = measurable.measure(constraints.copy(minHeight = 0))
        val height = if (isLookingAhead) {
            placeable.height
        } else {
            val rawHeight = animation.measure(placeable.height, true, motion, MotionRole.LargeContainer, scope)
            animation.boundedHeight(rawHeight, placeable.height, overshoot)
        }
        layout(placeable.width, constraints.constrainHeight(height)) { placeable.placeRelative(0, 0) }
    }
}
