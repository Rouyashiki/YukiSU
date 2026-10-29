package com.anatdx.yukisu.ui.component

import android.annotation.SuppressLint
import androidx.compose.animation.*
import androidx.compose.animation.core.animateFloat
import androidx.compose.foundation.focusGroup
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionRole
import kotlin.math.tanh
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.unit.dp

@SuppressLint("AutoboxingStateCreation")
@Composable
fun rememberFabVisibilityState(listState: LazyListState): State<Boolean> {
    var previousScrollOffset by remember { mutableStateOf(0) }
    var previousIndex by remember { mutableStateOf(0) }
    val fabVisible = remember { mutableStateOf(true) }

    LaunchedEffect(listState) {
        snapshotFlow { listState.firstVisibleItemIndex to listState.firstVisibleItemScrollOffset }
            .collect { (index, offset) ->
                if (previousIndex == 0 && previousScrollOffset == 0) {
                    fabVisible.value = true
                } else {
                    val isScrollingDown = when {
                        index > previousIndex -> false
                        index < previousIndex -> true
                        else -> offset < previousScrollOffset
                    }

                    fabVisible.value = isScrollingDown
                }

                previousIndex = index
                previousScrollOffset = offset
            }
    }

    return fabVisible
}

@Composable
fun AnimatedFab(
    visible: Boolean,
    content: @Composable () -> Unit
) {
    val motion = LocalYukiMotion.current
    val hiddenOffsetPx = with(LocalDensity.current) { 28.dp.toPx() }
    val boundary = remember(visible) { FabCollapseBoundary() }
    val focusManager = LocalFocusManager.current
    var hasFocus by remember { mutableStateOf(false) }
    LaunchedEffect(visible) {
        if (!visible && hasFocus) focusManager.clearFocus()
    }
    AnimatedVisibility(
        visible = visible,
        enter = EnterTransition.None,
        exit = ExitTransition.None,
    ) {
        val animatedScale by transition.animateFloat(
            transitionSpec = {
                motion.spatial(MotionRole.Scale, expanding = targetState == EnterExitState.Visible)
            },
            label = "FabVisibilityScale",
        ) { if (it == EnterExitState.Visible) 1f else 0f }
        val opacity by transition.animateFloat(
            transitionSpec = { motion.effects() },
            label = "FabVisibilityOpacity",
        ) { if (it == EnterExitState.Visible) 1f else 0f }
        Box(
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
                        while (true) awaitPointerEvent(PointerEventPass.Initial).changes.forEach { it.consume() }
                    }
                }
                .graphicsLayer {
                    val currentScale = boundary.fraction(
                        visible, if (motion.enabled) animatedScale else if (visible) 1f else 0f,
                    )
                    val visualScale = if (currentScale <= 1f) currentScale.coerceAtLeast(0f)
                        else 1f + 0.18f * tanh(((currentScale - 1f) / 0.18f).toDouble()).toFloat()
                    scaleX = visualScale
                    scaleY = visualScale
                    alpha = if (motion.enabled) opacity.coerceIn(0f, 1f) else if (visible) 1f else 0f
                    translationY = (1f - visualScale.coerceAtMost(1f)) * hiddenOffsetPx
                    clip = false
                }
        ) {
            content()
        }
    }
}

internal class FabCollapseBoundary {
    var closed = false
        private set

    fun fraction(expanded: Boolean, progress: Float): Float {
        if (!expanded && progress <= 0f) closed = true
        return if (!expanded && closed) 0f else progress
    }
}
