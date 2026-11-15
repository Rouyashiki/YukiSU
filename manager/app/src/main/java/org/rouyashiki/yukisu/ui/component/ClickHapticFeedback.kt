package org.rouyashiki.yukisu.ui.component

import android.view.HapticFeedbackConstants
import android.view.View
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.changedToUpIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView

/**
 * Adds a light haptic response after a descendant consumes a short tap.
 *
 * Listening during the final pointer pass lets this modifier distinguish an
 * actual control interaction from a tap on empty space. Dragging and long
 * presses are deliberately excluded so scrolling and gesture-specific haptics
 * keep their existing behaviour.
 */
@Composable
fun Modifier.clickHapticFeedback(enabled: Boolean = true): Modifier {
    if (!enabled) return this

    val feedbackView = LocalView.current
    return pointerInput(feedbackView) {
        awaitEachGesture {
            val down = awaitFirstDown(
                requireUnconsumed = false,
                pass = PointerEventPass.Final,
            )
            val pointerId = down.id
            val startPosition = down.position
            var interactionConsumed = down.isConsumed
            var movedPastTouchSlop = false
            var released = false
            var releaseUptimeMillis = down.uptimeMillis

            while (true) {
                val event = awaitPointerEvent(PointerEventPass.Final)
                val change = event.changes.firstOrNull { it.id == pointerId } ?: break

                interactionConsumed = interactionConsumed || change.isConsumed
                if (!movedPastTouchSlop) {
                    movedPastTouchSlop = Offset(
                        x = change.position.x - startPosition.x,
                        y = change.position.y - startPosition.y,
                    ).getDistance() > viewConfiguration.touchSlop
                }

                if (change.changedToUpIgnoreConsumed()) {
                    released = true
                    releaseUptimeMillis = change.uptimeMillis
                    break
                }
                if (!change.pressed) break
            }

            val wasShortTap = releaseUptimeMillis - down.uptimeMillis <
                viewConfiguration.longPressTimeoutMillis
            if (released && interactionConsumed && !movedPastTouchSlop && wasShortTap) {
                feedbackView.performClickHapticFeedback()
            }
        }
    }
}

/** Performs the same lightweight feedback for callbacks hosted outside our Compose roots. */
fun View.performClickHapticFeedback() {
    performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
}
