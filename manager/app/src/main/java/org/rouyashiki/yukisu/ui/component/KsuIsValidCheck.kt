package org.rouyashiki.yukisu.ui.component

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import org.rouyashiki.yukisu.Natives

@Composable
fun KsuIsValid(
    content: @Composable () -> Unit
) {
    // Cache the JNI roundtrip; manager status and KSU version do not change
    // for the lifetime of this composition.
    val ksuVersion = remember {
        if (Natives.isManager) Natives.version else null
    }
    if (ksuVersion != null) {
        content()
    }
}