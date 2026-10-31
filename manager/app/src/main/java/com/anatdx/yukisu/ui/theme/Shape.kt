package com.anatdx.yukisu.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.ui.unit.dp

val ExpressiveListGroupMinHeight = 64.dp
val ExpressiveListGroupMinRadius = 7.dp

val ExpressiveShapes = Shapes(
    extraSmall = RoundedCornerShape(ExpressiveListGroupMinRadius),
    small = RoundedCornerShape(14.dp),
    medium = RoundedCornerShape(20.dp),
    large = RoundedCornerShape(28.dp),
    extraLarge = RoundedCornerShape(36.dp),
    largeIncreased = RoundedCornerShape(32.dp),
    extraLargeIncreased = RoundedCornerShape(44.dp),
    extraExtraLarge = RoundedCornerShape(56.dp)
)
