package org.rouyashiki.yukisu.ui.activity.util

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext

internal const val PREDICTIVE_BACK_PREFERENCE = "predictive_back"

@Composable
internal fun rememberPredictiveBackEnabled(): State<Boolean> {
    val context = LocalContext.current
    val prefs = remember(context) { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    val enabled = remember(prefs) { mutableStateOf(prefs.getBoolean(PREDICTIVE_BACK_PREFERENCE, false)) }
    DisposableEffect(prefs) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == null || key == PREDICTIVE_BACK_PREFERENCE) {
                enabled.value = prefs.getBoolean(PREDICTIVE_BACK_PREFERENCE, false)
            }
        }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        enabled.value = prefs.getBoolean(PREDICTIVE_BACK_PREFERENCE, false)
        onDispose { prefs.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    return enabled
}
