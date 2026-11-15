package org.rouyashiki.yukisu.ui.screen.moreSettings

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.ValueAnimator
import android.content.Context
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.animation.AccelerateDecelerateInterpolator
import org.rouyashiki.yukisu.R
import com.yalantis.ucrop.util.RectUtils
import com.yalantis.ucrop.view.GestureCropImageView
import com.yalantis.ucrop.view.OverlayView
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * uCrop overlay that keeps the crop frame at the device-screen aspect ratio.
 * Dragging is limited to the frame edges/corners so touches inside the frame
 * continue to pan, scale, and rotate the image. On release, the selected area
 * expands back to the largest frame and the image follows the same transform.
 */
class ScreenRatioOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : OverlayView(context, attrs, defStyleAttr) {

    private enum class DragHandle {
        Left,
        Top,
        Right,
        Bottom,
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft,
    }

    private val expandedCropRect = RectF()
    private val dragStartRect = RectF()
    private val animationStartRect = RectF()
    private val animationTargetRect = RectF()
    private val touchThreshold = 48f * resources.displayMetrics.density
    private val minimumEdge = 72f * resources.displayMetrics.density

    private var dragHandle: DragHandle? = null
    private var settleAnimator: ValueAnimator? = null

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        if (changed && !getCropViewRect().isEmpty) {
            expandedCropRect.set(getCropViewRect())
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (getFreestyleCropMode() == FREESTYLE_CROP_MODE_DISABLE) return false
        if (settleAnimator?.isRunning == true) return true

        return when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val handle = findDragHandle(event.x, event.y) ?: return false
                if (expandedCropRect.isEmpty) {
                    expandedCropRect.set(getCropViewRect())
                }
                dragHandle = handle
                dragStartRect.set(getCropViewRect())
                parent?.requestDisallowInterceptTouchEvent(true)
                true
            }

            MotionEvent.ACTION_MOVE -> {
                val handle = dragHandle ?: return false
                if (event.pointerCount == 1) {
                    resizeCropRect(handle, event.x, event.y)
                }
                true
            }

            MotionEvent.ACTION_UP -> {
                if (dragHandle == null) return false
                dragHandle = null
                parent?.requestDisallowInterceptTouchEvent(false)
                performClick()
                true
            }

            MotionEvent.ACTION_CANCEL -> {
                if (dragHandle == null) return false
                dragHandle = null
                parent?.requestDisallowInterceptTouchEvent(false)
                settleCropSelection()
                true
            }

            else -> dragHandle != null
        }
    }

    override fun performClick(): Boolean {
        super.performClick()
        settleCropSelection()
        return true
    }

    /**
     * UCropView normally asks its children to handle a DOWN event in drawing order. In
     * practice GestureCropImageView can win that dispatch before the overlay gets a
     * chance to claim a crop handle. The parent crop view uses this hit test to route
     * the complete edge/corner gesture here explicitly.
     */
    internal fun isCropHandle(x: Float, y: Float): Boolean =
        getFreestyleCropMode() != FREESTYLE_CROP_MODE_DISABLE &&
            settleAnimator?.isRunning != true &&
            findDragHandle(x, y) != null

    override fun onDetachedFromWindow() {
        settleAnimator?.removeAllListeners()
        settleAnimator?.cancel()
        settleAnimator = null
        super.onDetachedFromWindow()
    }

    private fun findDragHandle(x: Float, y: Float): DragHandle? {
        val rect = getCropViewRect()
        val withinHorizontalRange = x >= rect.left - touchThreshold && x <= rect.right + touchThreshold
        val withinVerticalRange = y >= rect.top - touchThreshold && y <= rect.bottom + touchThreshold
        val nearLeft = abs(x - rect.left) <= touchThreshold && withinVerticalRange
        val nearRight = abs(x - rect.right) <= touchThreshold && withinVerticalRange
        val nearTop = abs(y - rect.top) <= touchThreshold && withinHorizontalRange
        val nearBottom = abs(y - rect.bottom) <= touchThreshold && withinHorizontalRange

        return when {
            nearLeft && nearTop -> DragHandle.TopLeft
            nearRight && nearTop -> DragHandle.TopRight
            nearRight && nearBottom -> DragHandle.BottomRight
            nearLeft && nearBottom -> DragHandle.BottomLeft
            nearLeft -> DragHandle.Left
            nearTop -> DragHandle.Top
            nearRight -> DragHandle.Right
            nearBottom -> DragHandle.Bottom
            else -> null
        }
    }

    private fun resizeCropRect(handle: DragHandle, touchX: Float, touchY: Float) {
        if (expandedCropRect.isEmpty) return

        val aspectRatio = expandedCropRect.width() / expandedCropRect.height()
        val contentLeft = paddingLeft.toFloat()
        val contentTop = paddingTop.toFloat()
        val contentRight = width - paddingRight.toFloat()
        val contentBottom = height - paddingBottom.toFloat()
        val minWidth = max(minimumEdge, minimumEdge * aspectRatio)
        val result = when (handle) {
            DragHandle.TopLeft -> resizeFromCorner(
                anchorX = dragStartRect.right,
                anchorY = dragStartRect.bottom,
                horizontalDirection = -1f,
                verticalDirection = -1f,
                touchX = touchX,
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.TopRight -> resizeFromCorner(
                anchorX = dragStartRect.left,
                anchorY = dragStartRect.bottom,
                horizontalDirection = 1f,
                verticalDirection = -1f,
                touchX = touchX,
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.BottomRight -> resizeFromCorner(
                anchorX = dragStartRect.left,
                anchorY = dragStartRect.top,
                horizontalDirection = 1f,
                verticalDirection = 1f,
                touchX = touchX,
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.BottomLeft -> resizeFromCorner(
                anchorX = dragStartRect.right,
                anchorY = dragStartRect.top,
                horizontalDirection = -1f,
                verticalDirection = 1f,
                touchX = touchX,
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.Left -> resizeFromHorizontalEdge(
                anchorX = dragStartRect.right,
                horizontalDirection = -1f,
                centerY = dragStartRect.centerY(),
                touchX = touchX,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.Right -> resizeFromHorizontalEdge(
                anchorX = dragStartRect.left,
                horizontalDirection = 1f,
                centerY = dragStartRect.centerY(),
                touchX = touchX,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.Top -> resizeFromVerticalEdge(
                anchorY = dragStartRect.bottom,
                verticalDirection = -1f,
                centerX = dragStartRect.centerX(),
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )

            DragHandle.Bottom -> resizeFromVerticalEdge(
                anchorY = dragStartRect.top,
                verticalDirection = 1f,
                centerX = dragStartRect.centerX(),
                touchY = touchY,
                aspectRatio = aspectRatio,
                minWidth = minWidth,
                contentLeft = contentLeft,
                contentTop = contentTop,
                contentRight = contentRight,
                contentBottom = contentBottom,
            )
        }

        getCropViewRect().set(result)
        refreshCropGrid()
    }

    private fun resizeFromCorner(
        anchorX: Float,
        anchorY: Float,
        horizontalDirection: Float,
        verticalDirection: Float,
        touchX: Float,
        touchY: Float,
        aspectRatio: Float,
        minWidth: Float,
        contentLeft: Float,
        contentTop: Float,
        contentRight: Float,
        contentBottom: Float,
    ): RectF {
        val projectedWidth = (
            horizontalDirection * (touchX - anchorX) +
                verticalDirection * (touchY - anchorY) / aspectRatio
            ) / (1f + 1f / (aspectRatio * aspectRatio))
        val horizontalLimit = if (horizontalDirection > 0f) {
            contentRight - anchorX
        } else {
            anchorX - contentLeft
        }
        val verticalLimit = if (verticalDirection > 0f) {
            (contentBottom - anchorY) * aspectRatio
        } else {
            (anchorY - contentTop) * aspectRatio
        }
        val maxWidth = min(horizontalLimit, verticalLimit).coerceAtLeast(1f)
        val cropWidth = projectedWidth.coerceIn(min(minWidth, maxWidth), maxWidth)
        val cropHeight = cropWidth / aspectRatio
        val oppositeX = anchorX + horizontalDirection * cropWidth
        val oppositeY = anchorY + verticalDirection * cropHeight
        return RectF(
            min(anchorX, oppositeX),
            min(anchorY, oppositeY),
            max(anchorX, oppositeX),
            max(anchorY, oppositeY),
        )
    }

    private fun resizeFromHorizontalEdge(
        anchorX: Float,
        horizontalDirection: Float,
        centerY: Float,
        touchX: Float,
        aspectRatio: Float,
        minWidth: Float,
        contentLeft: Float,
        contentTop: Float,
        contentRight: Float,
        contentBottom: Float,
    ): RectF {
        val horizontalLimit = if (horizontalDirection > 0f) {
            contentRight - anchorX
        } else {
            anchorX - contentLeft
        }
        val verticalLimit = 2f * min(centerY - contentTop, contentBottom - centerY) * aspectRatio
        val maxWidth = min(horizontalLimit, verticalLimit).coerceAtLeast(1f)
        val cropWidth = (horizontalDirection * (touchX - anchorX))
            .coerceIn(min(minWidth, maxWidth), maxWidth)
        val cropHeight = cropWidth / aspectRatio
        val oppositeX = anchorX + horizontalDirection * cropWidth
        return RectF(
            min(anchorX, oppositeX),
            centerY - cropHeight / 2f,
            max(anchorX, oppositeX),
            centerY + cropHeight / 2f,
        )
    }

    private fun resizeFromVerticalEdge(
        anchorY: Float,
        verticalDirection: Float,
        centerX: Float,
        touchY: Float,
        aspectRatio: Float,
        minWidth: Float,
        contentLeft: Float,
        contentTop: Float,
        contentRight: Float,
        contentBottom: Float,
    ): RectF {
        val verticalLimit = if (verticalDirection > 0f) {
            contentBottom - anchorY
        } else {
            anchorY - contentTop
        }
        val horizontalLimit = 2f * min(centerX - contentLeft, contentRight - centerX) / aspectRatio
        val maxHeight = min(verticalLimit, horizontalLimit).coerceAtLeast(1f)
        val minHeight = minWidth / aspectRatio
        val cropHeight = (verticalDirection * (touchY - anchorY))
            .coerceIn(min(minHeight, maxHeight), maxHeight)
        val cropWidth = cropHeight * aspectRatio
        val oppositeY = anchorY + verticalDirection * cropHeight
        return RectF(
            centerX - cropWidth / 2f,
            min(anchorY, oppositeY),
            centerX + cropWidth / 2f,
            max(anchorY, oppositeY),
        )
    }

    private fun settleCropSelection() {
        if (expandedCropRect.isEmpty || getCropViewRect().isEmpty) return

        animationStartRect.set(getCropViewRect())
        animationTargetRect.set(expandedCropRect)
        if (rectsNearlyEqual(animationStartRect, animationTargetRect)) {
            getOverlayViewChangeListener()?.onCropRectUpdated(RectF(animationTargetRect))
            return
        }

        val cropImageView = rootView.findViewById<GestureCropImageView>(R.id.image_view_crop)
        val targetScale = animationTargetRect.width() / animationStartRect.width()
        var previousScale = 1f
        var previousCenterX = animationStartRect.centerX()
        var previousCenterY = animationStartRect.centerY()

        settleAnimator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = SETTLE_ANIMATION_DURATION_MS
            interpolator = AccelerateDecelerateInterpolator()
            addUpdateListener { animator ->
                val progress = animator.animatedValue as Float
                val currentScale = lerp(1f, targetScale, progress)
                val currentCenterX = lerp(
                    animationStartRect.centerX(),
                    animationTargetRect.centerX(),
                    progress,
                )
                val currentCenterY = lerp(
                    animationStartRect.centerY(),
                    animationTargetRect.centerY(),
                    progress,
                )

                cropImageView?.postScale(
                    currentScale / previousScale,
                    previousCenterX,
                    previousCenterY,
                )
                cropImageView?.postTranslate(
                    currentCenterX - previousCenterX,
                    currentCenterY - previousCenterY,
                )
                previousScale = currentScale
                previousCenterX = currentCenterX
                previousCenterY = currentCenterY

                getCropViewRect().set(
                    lerp(animationStartRect.left, animationTargetRect.left, progress),
                    lerp(animationStartRect.top, animationTargetRect.top, progress),
                    lerp(animationStartRect.right, animationTargetRect.right, progress),
                    lerp(animationStartRect.bottom, animationTargetRect.bottom, progress),
                )
                refreshCropGrid()
            }
            addListener(object : AnimatorListenerAdapter() {
                override fun onAnimationEnd(animation: Animator) {
                    getCropViewRect().set(animationTargetRect)
                    refreshCropGrid()
                    getOverlayViewChangeListener()?.onCropRectUpdated(RectF(animationTargetRect))
                    settleAnimator = null
                }
            })
            start()
        }
    }

    private fun refreshCropGrid() {
        val cropRect = getCropViewRect()
        mCropGridCorners = RectUtils.getCornersFromRect(cropRect)
        mCropGridCenter = RectUtils.getCenterFromRect(cropRect)
        setCropGridRowCount(2)
        setCropGridColumnCount(2)
        postInvalidateOnAnimation()
    }

    private fun rectsNearlyEqual(first: RectF, second: RectF): Boolean {
        return abs(first.left - second.left) < 0.5f &&
            abs(first.top - second.top) < 0.5f &&
            abs(first.right - second.right) < 0.5f &&
            abs(first.bottom - second.bottom) < 0.5f
    }

    private fun lerp(start: Float, end: Float, progress: Float): Float {
        return start + (end - start) * progress
    }

    private companion object {
        const val SETTLE_ANIMATION_DURATION_MS = 280L
    }
}
