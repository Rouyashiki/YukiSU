package com.anatdx.yukisu.ui.motion

import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.material3.MotionScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.LocalContext
import kotlin.math.tanh

enum class MotionPreset(val persistedValue: String) {
    Off("off"),
    Soft("soft"),
    QBouncy("q_bouncy");

    companion object {
        fun fromPersistedValue(value: String?): MotionPreset =
            entries.firstOrNull { it.persistedValue == value } ?: QBouncy
    }
}

enum class MotionRole { Container, LargeContainer, Arrow, Fab, Scale }

@Immutable
data class YukiMotion(val preset: MotionPreset = MotionPreset.QBouncy) : MotionScheme {
    val enabled: Boolean get() = preset != MotionPreset.Off

    fun <T> spatial(
        role: MotionRole = MotionRole.Container,
        expanding: Boolean = true,
        visibilityThreshold: T? = null,
    ): FiniteAnimationSpec<T> {
        if (!enabled) return snap()
        val (damping, stiffness) = parameters(role, expanding)
        return spring(dampingRatio = damping, stiffness = stiffness, visibilityThreshold = visibilityThreshold)
    }

    internal fun parameters(role: MotionRole, expanding: Boolean = true): Pair<Float, Float> {
        if (!expanding && role != MotionRole.Arrow) return when (preset) {
            MotionPreset.Off, MotionPreset.Soft -> 1f to 650f
            MotionPreset.QBouncy -> 0.38f to 620f
        }
        return when (preset) {
            MotionPreset.Off, MotionPreset.Soft -> when (role) {
                MotionRole.Container -> 0.82f to 380f
                MotionRole.LargeContainer -> 0.92f to 420f
                MotionRole.Arrow -> 0.60f to 800f
                MotionRole.Fab -> 0.78f to 430f
                MotionRole.Scale -> 0.72f to 650f
            }
            MotionPreset.QBouncy -> when (role) {
                MotionRole.Container -> 0.38f to 460f
                MotionRole.LargeContainer -> 0.40f to 460f
                MotionRole.Arrow -> 0.32f to 650f
                MotionRole.Fab -> 0.36f to 560f
                MotionRole.Scale -> 0.32f to 700f
            }
        }
    }

    fun <T> effects(): FiniteAnimationSpec<T> =
        if (enabled) spring(dampingRatio = 1f, stiffness = 1600f) else snap()

    val overshootDp: Float get() = when (preset) {
        MotionPreset.Off -> 0f
        MotionPreset.Soft -> 24f
        MotionPreset.QBouncy -> 44f
    }

    override fun <T> defaultSpatialSpec(): FiniteAnimationSpec<T> = spatial()
    override fun <T> fastSpatialSpec(): FiniteAnimationSpec<T> = spatial(MotionRole.Fab)
    override fun <T> slowSpatialSpec(): FiniteAnimationSpec<T> = spatial(MotionRole.LargeContainer)
    override fun <T> defaultEffectsSpec(): FiniteAnimationSpec<T> = effects()
    override fun <T> fastEffectsSpec(): FiniteAnimationSpec<T> =
        if (enabled) spring(dampingRatio = 1f, stiffness = 3800f) else snap()
    override fun <T> slowEffectsSpec(): FiniteAnimationSpec<T> =
        if (enabled) spring(dampingRatio = 1f, stiffness = 800f) else snap()
}

val LocalYukiMotion = staticCompositionLocalOf { YukiMotion() }

@Composable
fun rememberYukiMotion(preset: MotionPreset): YukiMotion {
    val resolver = LocalContext.current.contentResolver
    fun animationsEnabled(): Boolean =
        Settings.Global.getFloat(resolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) > 0f
    var systemEnabled by remember(resolver) { mutableStateOf(animationsEnabled()) }
    DisposableEffect(resolver) {
        val observer = object : ContentObserver(Handler(Looper.getMainLooper())) {
            override fun onChange(selfChange: Boolean) {
                systemEnabled = animationsEnabled()
            }
        }
        resolver.registerContentObserver(
            Settings.Global.getUriFor(Settings.Global.ANIMATOR_DURATION_SCALE), false, observer,
        )
        onDispose { resolver.unregisterContentObserver(observer) }
    }
    return remember(preset, systemEnabled) {
        YukiMotion(if (systemEnabled) preset else MotionPreset.Off)
    }
}

internal fun expansionHeight(progress: Float, height: Int, overshoot: Float): Int {
    if (progress <= 0f || height <= 0) return 0
    if (progress <= 1f) return (height * progress).toInt()
    if (overshoot <= 0f) return height
    return (height + overshoot * tanh((progress - 1f) * height / overshoot)).toInt()
}

internal class ExpansionBoundary {
    var closed: Boolean = false
        private set

    fun hasClosed(progress: Float): Boolean {
        if (progress <= 0f) closed = true
        return closed
    }

    fun height(expanded: Boolean, progress: Float, height: Int, overshoot: Float): Int {
        return if (!expanded && hasClosed(progress)) 0 else expansionHeight(progress, height, overshoot)
    }
}
