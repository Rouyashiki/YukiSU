package org.rouyashiki.yukisu.magica

import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.edit
import org.rouyashiki.yukisu.boot.BootCompletedReceiver as BaseBootCompletedReceiver

object MagicaHelper {
    private const val TAG = "YukiSUMagica"
    private const val SETTINGS_PREFS = "settings"
    private const val AUTO_JAILBREAK_KEY = "auto_jailbreak"

    @JvmStatic
    fun isAutoJailbreakEnabled(context: Context): Boolean {
        return context.applicationContext
            .getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE)
            .getBoolean(AUTO_JAILBREAK_KEY, false)
    }

    @JvmStatic
    fun setAutoJailbreakEnabled(context: Context, enabled: Boolean) {
        val appContext = context.applicationContext
        appContext.getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE).edit {
            putBoolean(AUTO_JAILBREAK_KEY, enabled)
        }
        BaseBootCompletedReceiver.setEnabled(
            appContext,
            BootCompletedReceiver::class.java,
            enabled,
            TAG,
        )
    }

    @JvmStatic
    fun launch(context: Context, trigger: String = "manual"): Boolean {
        val appContext = context.applicationContext
        return runCatching {
            appContext.startService(Intent(appContext, MagicaService::class.java))
            Log.i(TAG, "MagicaService started from trigger: $trigger")
            true
        }.getOrElse {
            Log.e(TAG, "failed to start MagicaService from trigger: $trigger", it)
            false
        }
    }
}
