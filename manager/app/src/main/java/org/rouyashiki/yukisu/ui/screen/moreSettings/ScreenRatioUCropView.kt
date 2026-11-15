package org.rouyashiki.yukisu.ui.screen.moreSettings

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import com.yalantis.ucrop.view.UCropView

/**
 * Routes touches that start on the crop frame directly to the overlay. Everything
 * else keeps uCrop's normal image pan/pinch/rotate handling.
 */
class ScreenRatioUCropView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : UCropView(context, attrs, defStyleAttr) {

    private var frameGestureInProgress = false

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        val overlay = overlayView as? ScreenRatioOverlayView
            ?: return super.dispatchTouchEvent(event)
        val childX = event.x + scrollX - overlay.left
        val childY = event.y + scrollY - overlay.top

        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            frameGestureInProgress = overlay.isCropHandle(childX, childY)
        }

        if (!frameGestureInProgress) {
            return super.dispatchTouchEvent(event)
        }

        val overlayEvent = MotionEvent.obtain(event).apply {
            offsetLocation(
                (scrollX - overlay.left).toFloat(),
                (scrollY - overlay.top).toFloat(),
            )
        }
        val handled = overlay.onTouchEvent(overlayEvent)
        overlayEvent.recycle()

        if (event.actionMasked == MotionEvent.ACTION_UP ||
            event.actionMasked == MotionEvent.ACTION_CANCEL
        ) {
            frameGestureInProgress = false
        }
        return handled
    }
}
