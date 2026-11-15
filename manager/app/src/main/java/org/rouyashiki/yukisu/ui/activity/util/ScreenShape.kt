package org.rouyashiki.yukisu.ui.activity.util

import android.view.RoundedCorner
import android.view.WindowInsets
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.LayoutDirection

internal data class ScreenShape(
    val topLeft: Float = 0f,
    val topRight: Float = 0f,
    val bottomRight: Float = 0f,
    val bottomLeft: Float = 0f,
) : Shape {
    override fun createOutline(size: Size, layoutDirection: LayoutDirection, density: Density): Outline {
        // Insets radii are pixels. DisplayShape may still contain a different resolution's path.
        return Outline.Rounded(
            RoundRect(
                left = 0f,
                top = 0f,
                right = size.width,
                bottom = size.height,
                topLeftCornerRadius = CornerRadius(topLeft),
                topRightCornerRadius = CornerRadius(topRight),
                bottomRightCornerRadius = CornerRadius(bottomRight),
                bottomLeftCornerRadius = CornerRadius(bottomLeft),
            ),
        )
    }

    companion object {
        fun fromInsets(insets: WindowInsets?): ScreenShape {
            fun radius(position: Int) = insets?.getRoundedCorner(position)?.radius?.toFloat() ?: 0f
            return ScreenShape(
                topLeft = radius(RoundedCorner.POSITION_TOP_LEFT),
                topRight = radius(RoundedCorner.POSITION_TOP_RIGHT),
                bottomRight = radius(RoundedCorner.POSITION_BOTTOM_RIGHT),
                bottomLeft = radius(RoundedCorner.POSITION_BOTTOM_LEFT),
            )
        }
    }
}
