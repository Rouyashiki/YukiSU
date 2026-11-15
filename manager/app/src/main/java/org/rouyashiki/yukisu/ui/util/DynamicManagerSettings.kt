package org.rouyashiki.yukisu.ui.util

import android.content.Context
import androidx.core.content.edit

object DynamicManagerSettings {
    private const val PREFS_NAME = "settings"
    const val KEY_ALLOW_ANY_DYNAMIC_MANAGER = "allow_any_dynamic_manager"

    fun allowAnyApp(context: Context): Boolean =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_ALLOW_ANY_DYNAMIC_MANAGER, false)

    fun setAllowAnyApp(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putBoolean(KEY_ALLOW_ANY_DYNAMIC_MANAGER, enabled)
        }
    }
}
