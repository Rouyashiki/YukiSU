package com.anatdx.yukisu.ui.motion

import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.SnapSpec
import androidx.compose.animation.core.TargetBasedAnimation
import androidx.compose.animation.core.VectorConverter
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class YukiMotionTest {
    @Test
    fun defaultAndPersistedSelectionsKeepQBouncyAsTheFallback() {
        assertEquals(MotionPreset.QBouncy, YukiMotion().preset)
        assertEquals(MotionPreset.QBouncy, MotionPreset.fromPersistedValue(null))
        assertEquals(MotionPreset.QBouncy, MotionPreset.fromPersistedValue(""))
        assertEquals(MotionPreset.QBouncy, MotionPreset.fromPersistedValue("unknown-future-preset"))
        assertEquals(MotionPreset.QBouncy, MotionPreset.fromPersistedValue("duang"))

        val selections = mapOf(
            "off" to MotionPreset.Off,
            "soft" to MotionPreset.Soft,
            "q_bouncy" to MotionPreset.QBouncy,
        )
        assertEquals(selections.size, MotionPreset.entries.size)
        selections.forEach { (stored, preset) ->
            assertEquals(preset, MotionPreset.fromPersistedValue(stored))
            assertEquals(stored, preset.persistedValue)
        }
    }

    @Test
    fun offSnapsEveryRoleAndMaterialMotionChannelImmediately() {
        val motion = YukiMotion(MotionPreset.Off)
        assertFalse(motion.enabled)
        val specifications = MotionRole.entries.flatMap { role ->
            listOf(motion.spatial<Float>(role), motion.spatial<Float>(role, expanding = false))
        } + listOf<FiniteAnimationSpec<Float>>(
            motion.effects(),
            motion.defaultSpatialSpec(),
            motion.fastSpatialSpec(),
            motion.slowSpatialSpec(),
            motion.defaultEffectsSpec(),
            motion.fastEffectsSpec(),
            motion.slowEffectsSpec(),
        )
        specifications.forEach { specification ->
            assertTrue(specification is SnapSpec<*>)
            val animation = animation(specification, initial = 0.25f, target = 0.75f)
            assertEquals(0L, animation.durationNanos)
            assertEquals(0.75f, animation.getValueFromNanos(0L))
        }
    }

    @Test
    fun everySpatialRoleHasDistinctIncreasingSpringOvershoot() {
        MotionRole.entries.forEach { role ->
            val curves = listOf(MotionPreset.Soft, MotionPreset.QBouncy)
                .map { preset -> sample(YukiMotion(preset).spatial(role)) }
            val peaks = curves.map { samples -> samples.maxOf { it.value } }

            assertTrue(peaks[0] <= 1.12f, "$role soft overshoot: ${peaks[0]}")
            assertTrue(peaks[1] >= 1.20f, "$role Q bounce must be visible: ${peaks[1]}")
            assertTrue(peaks[0] < peaks[1], "$role peaks: $peaks")
            curves.forEach { samples ->
                assertEquals(0f, samples.first().value)
                assertEquals(1f, samples.last().value)
                assertTrue(samples.all { it.value.isFinite() && it.velocity.isFinite() })
            }
        }
    }

    @Test
    fun bouncyContainersReboundAcrossTheTargetBeforeSettling() {
        listOf(MotionPreset.QBouncy).forEach { preset ->
            val samples = sample(YukiMotion(preset).spatial(MotionRole.Container))
            val firstOvershoot = samples.indexOfFirst { it.value > 1.05f }
            val rebound = samples.indexOfFirstAfter(firstOvershoot) { it.value < 0.97f }
            val secondOvershoot = samples.indexOfFirstAfter(rebound) { it.value > 1.005f }
            assertTrue(firstOvershoot >= 0, "$preset has an opening overshoot")
            assertTrue(rebound > firstOvershoot, "$preset rebounds below its destination")
            assertTrue(secondOvershoot > rebound, "$preset continues oscillating before settling")
            assertEquals(1f, samples.last().value)
        }
    }

    @Test
    fun effectsStayInsideTheirVisualRangeForEveryEnabledPreset() {
        MotionPreset.entries.filter { it != MotionPreset.Off }.forEach { preset ->
            val motion = YukiMotion(preset)
            listOf(
                motion.effects<Float>(),
                motion.defaultEffectsSpec(),
                motion.fastEffectsSpec(),
                motion.slowEffectsSpec(),
            ).forEach { specification ->
                val values = sample(specification).map { it.value }
                assertTrue(values.all { it in 0f..1f }, "$preset effects must not overshoot opacity")
                assertTrue(values.zipWithNext().all { (previous, next) -> next >= previous })
                assertEquals(1f, values.last())
            }
        }
    }

    @Test
    fun closingPanelStaysGoneAfterTheSpringFirstCrossesZero() {
        listOf(MotionPreset.QBouncy).forEach { preset ->
            val motion = YukiMotion(preset)
            val samples = sample(motion.spatial(expanding = false), initial = 1f, target = 0f)
            val firstCrossing = samples.indexOfFirst { it.value <= 0f }
            assertTrue(firstCrossing > 0, "$preset must reach the closed boundary")
            assertTrue(
                samples.drop(firstCrossing + 1).any { it.value > 0.01f },
                "$preset raw spring must rebound to exercise the closed-boundary guard",
            )

            val boundary = ExpansionBoundary()
            val heights = samples.map { boundary.height(false, it.value, 480, motion.overshootDp) }
            assertEquals(480, heights.first())
            assertTrue(heights.take(firstCrossing).any { it in 1..479 })
            assertTrue(heights.drop(firstCrossing).all { it == 0 }, "$preset panel reappeared")
            assertTrue(boundary.closed)
        }
    }

    @Test
    fun largePanelsLimitOvershootWhileTheSpringContinuesRebounding() {
        listOf(MotionPreset.QBouncy).forEach { preset ->
            val motion = YukiMotion(preset)
            val samples = sample(motion.spatial(MotionRole.LargeContainer))
            val contentHeight = 1600
            val limit = motion.overshootDp * 1.2f
            val boundary = ExpansionBoundary()
            val heights = samples.map { boundary.height(true, it.value, contentHeight, limit) }

            assertTrue(samples.maxOf { it.value * contentHeight } > contentHeight + 3f * limit)
            assertTrue(heights.max() > contentHeight, "$preset bounded bounce must remain visible")
            assertTrue(heights.all { it >= 0 && it <= contentHeight + limit })
            assertTrue(
                samples.any { it.value * contentHeight > contentHeight + limit && it.velocity > 0.1f },
                "$preset spring must still move while overshoot is constrained",
            )
            val firstPeak = heights.indices.maxBy { heights[it] }
            val rebound = heights.indexOfFirstAfter(firstPeak) { it < contentHeight - 10 }
            val nextOvershoot = heights.indexOfFirstAfter(rebound) { it > contentHeight + 2 }
            assertTrue(rebound > firstPeak, "$preset must return through the destination")
            assertTrue(nextOvershoot > rebound, "$preset spring was truncated at the height limit")
            assertEquals(contentHeight, heights.last())
            assertFalse(boundary.closed)
        }
    }

    private data class Sample(val value: Float, val velocity: Float)

    private fun animation(
        specification: FiniteAnimationSpec<Float>,
        initial: Float,
        target: Float,
    ) = TargetBasedAnimation(
        animationSpec = specification,
        typeConverter = Float.VectorConverter,
        initialValue = initial,
        targetValue = target,
    )

    private fun sample(
        specification: FiniteAnimationSpec<Float>,
        initial: Float = 0f,
        target: Float = 1f,
    ): List<Sample> {
        val animation = animation(specification, initial, target)
        assertTrue(animation.durationNanos in 1L..5_000_000_000L, "Spring must settle within 5 seconds")
        val times = (0L..animation.durationNanos step 2_000_000L).toList() + animation.durationNanos
        return times.distinct().map { time ->
            Sample(animation.getValueFromNanos(time), animation.getVelocityVectorFromNanos(time).value)
        }
    }

    private fun <T> List<T>.indexOfFirstAfter(index: Int, predicate: (T) -> Boolean): Int =
        indices.firstOrNull { it > index && predicate(this[it]) } ?: -1
}
