package com.anatdx.yukisu.ui.component

import androidx.compose.animation.core.TargetBasedAnimation
import androidx.compose.animation.core.VectorConverter
import com.anatdx.yukisu.ui.motion.MotionPreset
import com.anatdx.yukisu.ui.motion.MotionRole
import com.anatdx.yukisu.ui.motion.YukiMotion
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class FabCollapseBoundaryTest {
    @Test
    fun closingFabPositionAndScaleStayHiddenWhileTheirSpringsKeepRebounding() {
        listOf(MotionPreset.QBouncy).forEach { preset ->
            listOf(MotionRole.Fab, MotionRole.Scale).forEach { role ->
                val animation = TargetBasedAnimation(
                    animationSpec = YukiMotion(preset).spatial<Float>(role, expanding = false),
                    typeConverter = Float.VectorConverter,
                    initialValue = 1f,
                    targetValue = 0f,
                )
                val times = (0L..animation.durationNanos step 2_000_000L).toList() + animation.durationNanos
                val samples = times.distinct().map(animation::getValueFromNanos)
                val firstCrossing = samples.indexOfFirst { it <= 0f }
                assertTrue(firstCrossing > 0, "$preset $role must reach the closing boundary")
                assertTrue(
                    samples.drop(firstCrossing + 1).any { it > 0.01f },
                    "$preset $role must physically rebound to exercise the disappearance guard",
                )

                val boundary = FabCollapseBoundary()
                val rendered = samples.map { boundary.fraction(expanded = false, progress = it) }
                assertEquals(1f, rendered.first())
                assertEquals(samples.take(firstCrossing), rendered.take(firstCrossing))
                assertTrue(rendered.drop(firstCrossing).all { it == 0f }, "$preset $role reappeared")
                assertTrue(boundary.closed)
            }
        }
    }

    @Test
    fun exactZeroClosesTheBoundaryBeforeTheNextPositiveRebound() {
        val boundary = FabCollapseBoundary()
        assertEquals(0.2f, boundary.fraction(expanded = false, progress = 0.2f))
        assertFalse(boundary.closed)
        assertEquals(0f, boundary.fraction(expanded = false, progress = 0f))
        assertTrue(boundary.closed)
        assertEquals(0f, boundary.fraction(expanded = false, progress = 0.15f))
    }

    @Test
    fun offSpatialTargetsPassThroughTheRealBoundaryWithoutAnIntermediateFrame() {
        val motion = YukiMotion(MotionPreset.Off)
        listOf(MotionRole.Fab, MotionRole.Scale).forEach { role ->
            listOf(false, true).forEach { expanded ->
                val target = if (expanded) 1f else 0f
                val animation = TargetBasedAnimation(
                    animationSpec = motion.spatial<Float>(role, expanding = expanded),
                    typeConverter = Float.VectorConverter,
                    initialValue = 0.42f,
                    targetValue = target,
                )
                assertEquals(0L, animation.durationNanos)
                assertEquals(
                    target,
                    FabCollapseBoundary().fraction(expanded, animation.getValueFromNanos(0L)),
                )
            }
        }
    }
}
