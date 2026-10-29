package com.anatdx.yukisu.ui.activity.util

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.TransformOrigin
import androidx.navigationevent.NavigationEvent
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.PageTransitionStyle
import com.anatdx.yukisu.ui.motion.YukiMotion
import kotlinx.coroutines.Job
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.launch

internal data class NavigationOrigin(val x: Float = 0.5f, val y: Float = 0.5f) {
    fun transformOrigin() = TransformOrigin(x, y)
}

internal class NavigationOriginTracker {
    private data class Tap(val origin: NavigationOrigin, val timeMillis: Long)
    private var pendingTap: Tap? = null
    private var transitionKey: Pair<String, String>? = null
    private var transitionOrigin = NavigationOrigin()

    fun recordTap(x: Float, y: Float, width: Int, height: Int, timeMillis: Long) {
        pendingTap = if (width > 0 && height > 0 && x.isFinite() && y.isFinite()) {
            Tap(NavigationOrigin((x / width).coerceIn(0f, 1f), (y / height).coerceIn(0f, 1f)), timeMillis)
        } else null
    }

    fun originFor(fromId: String, toId: String, timeMillis: Long): NavigationOrigin {
        val key = fromId to toId
        if (transitionKey != key) {
            transitionKey = key
            transitionOrigin = pendingTap?.takeIf { timeMillis - it.timeMillis in 0L..750L }?.origin
                ?: NavigationOrigin()
            pendingTap = null
        }
        return transitionOrigin
    }
}

internal class PageMotion(
    private val motion: YukiMotion,
    private val style: PageTransitionStyle = PageTransitionStyle.Reveal,
) {
    val openingScale = if (motion.preset == MotionPreset.Soft) 0.84f else 0.72f
    val closingScale = 0.92f
    val enabled get() = motion.enabled
    val usesForwardSlide = motion.enabled && style == PageTransitionStyle.Slide
    val slideOpeningScale = if (motion.preset == MotionPreset.Soft) 0.97f else 0.92f

    fun slideOffsetSpec(): FiniteAnimationSpec<Float> = if (motion.enabled) {
        spring(dampingRatio = 1f, stiffness = 500f, visibilityThreshold = 0.001f)
    } else snap()

    fun slideScaleSpec(): FiniteAnimationSpec<Float> = if (motion.enabled) {
        spring(
            dampingRatio = if (motion.preset == MotionPreset.Soft) 0.8f else 0.38f,
            stiffness = 400f,
            visibilityThreshold = 0.001f,
        )
    } else snap()

    fun <T> enterSpec(): FiniteAnimationSpec<T> =
        if (motion.enabled) tween(360, easing = FastOutSlowInEasing) else snap()

    fun <T> returnSpec(): FiniteAnimationSpec<T> =
        if (motion.enabled) tween(220, easing = FastOutSlowInEasing) else snap()

    fun <T> returnFadeSpec(): FiniteAnimationSpec<T> =
        if (motion.enabled) tween(140, delayMillis = 80, easing = FastOutSlowInEasing) else snap()

    fun popEnter(): EnterTransition = if (!motion.enabled || style == PageTransitionStyle.Reveal) {
        EnterTransition.None
    } else slideInHorizontally(returnSpec()) { -it / 4 }

    fun popExit(): ExitTransition = when {
        !motion.enabled -> ExitTransition.None
        style == PageTransitionStyle.Slide -> slideOutHorizontally(returnSpec()) { it }
        else -> scaleOut(returnSpec(), targetScale = closingScale, transformOrigin = TransformOrigin.Center) +
            fadeOut(returnFadeSpec())
    }
}

internal data class ForwardPageTransform(
    val scale: Float = 1f,
    val offset: Float = 0f,
    val offsetY: Float = 0f,
    val alpha: Float = 1f,
)

internal class ForwardPageMotion(
    private val motion: PageMotion,
    private val origin: NavigationOrigin,
    private val bottomBar: Boolean,
) {
    private val slide = motion.usesForwardSlide && !bottomBar
    private val pivot = if (slide) NavigationOrigin(0f, 0.5f) else origin
    private val offset = Animatable(if (slide) 1f else 0f)
    private val scale = Animatable(when {
        bottomBar -> 1f
        slide -> motion.slideOpeningScale
        else -> motion.openingScale
    })
    private val alpha = Animatable(if (slide) 1f else if (bottomBar) 0f else 0.35f)
    private var job: Job? = null
    private var frozen by mutableStateOf<ForwardPageTransform?>(null)
    var finished by mutableStateOf(false)
        private set

    fun transform(): ForwardPageTransform {
        if (finished) return ForwardPageTransform()
        frozen?.let { return it }
        val currentScale = scale.value
        // Preserve the pivot's geometry when predictive back takes over around the page center.
        return ForwardPageTransform(
            scale = currentScale,
            offset = offset.value + (pivot.x - 0.5f) * (1f - currentScale),
            offsetY = (pivot.y - 0.5f) * (1f - currentScale),
            alpha = alpha.value,
        )
    }

    suspend fun animate() {
        if (finished || frozen != null) return
        job = currentCoroutineContext()[Job]
        try {
            coroutineScope {
                launch { offset.animateTo(0f, if (slide) motion.slideOffsetSpec() else snap()) }
                launch { scale.animateTo(1f, if (slide) motion.slideScaleSpec() else motion.enterSpec()) }
                launch { alpha.animateTo(1f, if (bottomBar) motion.enterSpec() else tween(140)) }
            }
            finished = true
        } finally {
            job = null
        }
    }

    fun handOff(): ForwardPageTransform {
        val current = transform()
        finished = true
        job?.cancel()
        return current
    }

    fun freeze() {
        frozen = transform()
        job?.cancel()
    }
}

internal enum class BackGesturePhase { Dragging, Cancelling, Completing }

internal data class BackGestureSession(
    val outgoingId: String,
    val incomingId: String,
    val direction: Int,
    val phase: BackGesturePhase = BackGesturePhase.Dragging,
    val releaseProgress: Float = 0f,
    val entryTransform: ForwardPageTransform = ForwardPageTransform(),
) {
    fun matches(fromId: String, toId: String) = outgoingId == fromId && incomingId == toId
    fun contains(entryId: String) = entryId == outgoingId || entryId == incomingId
}

internal class BackGestureState(
    private val currentEntryId: () -> String?,
    private val previousEntryId: () -> String?,
) : BackGestureObserver {
    var session by mutableStateOf<BackGestureSession?>(null)
        private set
    private var progress = 0f
    private val entries = mutableMapOf<String, ForwardPageMotion>()
    private val progressKeys = mutableMapOf<String, Pair<String, String>>()

    fun progressKey(entryId: String): Pair<String, String>? =
        if (entryId == currentEntryId() || session?.contains(entryId) == true) progressKeys[entryId] else null

    fun registerEntry(entryId: String, motion: ForwardPageMotion) { entries[entryId] = motion }

    fun unregisterEntry(entryId: String, motion: ForwardPageMotion) {
        if (entries[entryId] === motion) entries.remove(entryId)
    }

    fun freezeEntry(entryId: String) { entries[entryId]?.freeze() }

    override fun onStarted(event: NavigationEvent) {
        val current = currentEntryId() ?: return
        val previous = previousEntryId() ?: return
        val progressKey = current to previous
        progressKeys[current] = progressKey
        progressKeys[previous] = progressKey
        val entryTransform = entries[current]?.handOff() ?: ForwardPageTransform()
        entries[previous]?.handOff()
        progress = event.progress.coerceIn(0f, 1f)
        session = BackGestureSession(current, previous, when (event.swipeEdge) {
            NavigationEvent.EDGE_LEFT -> 1
            NavigationEvent.EDGE_RIGHT -> -1
            else -> 0
        }, entryTransform = entryTransform)
    }

    override fun onProgressed(event: NavigationEvent) {
        progress = event.progress.coerceIn(0f, 1f)
    }

    override fun onCompleted() {
        session = session?.copy(phase = BackGesturePhase.Completing, releaseProgress = progress)
    }

    override fun onCancelled() {
        session = session?.copy(phase = BackGesturePhase.Cancelling, releaseProgress = progress)
    }

    fun onPageSettled(entryId: String) {
        val active = session ?: return
        if (active.phase == BackGesturePhase.Dragging) return
        val current = currentEntryId()
        if (entryId == current && active.contains(entryId)) session = null
    }

    fun retainVisibleEntries(entryIds: Set<String>) {
        progressKeys.keys.removeAll { it !in entryIds && it != currentEntryId() }
        val active = session ?: return
        val current = currentEntryId()
        if ((current != null && !active.contains(current)) ||
            (active.outgoingId !in entryIds && active.incomingId !in entryIds)
        ) session = null
    }

    fun clear() {
        session = null
        progressKeys.clear()
    }
}

internal fun <T> predictiveBackSpec(): FiniteAnimationSpec<T> = tween(320, easing = LinearEasing)

internal data class PredictivePageTransform(
    val scale: Float = 1f,
    val offset: Float = 0f,
    val offsetY: Float = 0f,
    val alpha: Float = 1f,
    val dim: Float = 0f,
)

internal fun predictivePageTransform(
    session: BackGestureSession,
    outgoing: Boolean,
    progress: Float,
): PredictivePageTransform {
    val fraction = progress.coerceIn(0f, 1f)
    if (!outgoing) return PredictivePageTransform(
        offset = -session.direction * 0.025f * (1f - fraction),
        dim = 0.24f * (1f - fraction),
    )
    fun drag(value: Float): PredictivePageTransform {
        val entry = session.entryTransform
        return PredictivePageTransform(
            scale = (entry.scale + (1f - entry.scale) * value) * (1f - 0.2f * value),
            offset = entry.offset * (1f - value) + session.direction * 0.075f * value,
            offsetY = entry.offsetY * (1f - value),
            alpha = entry.alpha + (1f - entry.alpha) * (value * 4f).coerceAtMost(1f),
        )
    }
    if (session.phase == BackGesturePhase.Cancelling && session.releaseProgress > 0f) {
        val released = drag(session.releaseProgress)
        val remaining = (fraction / session.releaseProgress).coerceIn(0f, 1f)
        return PredictivePageTransform(
            scale = 1f + (released.scale - 1f) * remaining,
            offset = released.offset * remaining,
            offsetY = released.offsetY * remaining,
            alpha = 1f + (released.alpha - 1f) * remaining,
        )
    }
    val opacity = if (session.phase == BackGesturePhase.Completing) {
        val remaining = 1f - session.releaseProgress
        if (remaining <= 0.001f) 0f else ((1f - fraction) / remaining).coerceIn(0f, 1f)
    } else 1f
    val current = drag(fraction)
    return current.copy(alpha = current.alpha * opacity)
}
