package org.rouyashiki.yukisu.ui.theme

import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.unit.dp

object ManagerUi {
    val CardPadding = 16.dp
    val GroupSpacing = 24.dp
    val SectionTitlePadding = 8.dp
    val ListMaxWidth = 840.dp
    val ContentMaxWidth = 1040.dp
}

val LocalCompactWindow = staticCompositionLocalOf { false }
