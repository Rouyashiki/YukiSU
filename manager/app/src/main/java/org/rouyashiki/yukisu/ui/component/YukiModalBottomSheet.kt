package org.rouyashiki.yukisu.ui.component

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.material3.BottomSheetDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SheetState
import androidx.compose.material3.SheetValue
import androidx.compose.material3.rememberBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import org.rouyashiki.yukisu.ui.activity.util.ModalNavigationEventScope
import org.rouyashiki.yukisu.ui.activity.util.WindowNavigationEventBridge

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun YukiModalBottomSheet(
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    sheetState: SheetState = rememberBottomSheetState(initialValue = SheetValue.Hidden),
    containerColor: Color = BottomSheetDefaults.ContainerColor,
    dragHandle: @Composable (() -> Unit)? = { BottomSheetDefaults.DragHandle() },
    content: @Composable ColumnScope.() -> Unit,
) {
    ModalNavigationEventScope {
        ModalBottomSheet(
            onDismissRequest = onDismissRequest,
            modifier = modifier,
            sheetState = sheetState,
            containerColor = containerColor,
            dragHandle = dragHandle,
        ) {
            WindowNavigationEventBridge()
            content()
        }
    }
}
