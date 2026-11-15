package org.rouyashiki.yukisu.superkey

import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.edit
import org.rouyashiki.yukisu.boot.BootCompletedReceiver as BaseBootCompletedReceiver

object SuperKeyHelper {
    private const val TAG = "YukiSUSuperKey"
    private const val PREFS_NAME = "superkey"
    private const val SAVED_SUPERKEY_KEY = "saved_superkey"
    private const val SKIP_STORE_SUPERKEY_KEY = "skip_store_superkey"
    private const val AUTO_AUTHENTICATE_KEY = "auto_authenticate_superkey"

    fun getSavedSuperKey(context: Context): String? {
        return preferences(context).getString(SAVED_SUPERKEY_KEY, null)?.takeIf {
            it.isNotBlank()
        }
    }

    fun hasSavedSuperKey(context: Context): Boolean = getSavedSuperKey(context) != null

    fun saveSuperKey(context: Context, superKey: String) {
        if (shouldSkipStorage(context) || superKey.isBlank()) {
            return
        }
        preferences(context).edit { putString(SAVED_SUPERKEY_KEY, superKey) }
        updateBootReceiver(context)
    }

    fun clearSavedSuperKey(context: Context) {
        preferences(context).edit {
            remove(SAVED_SUPERKEY_KEY)
            putBoolean(AUTO_AUTHENTICATE_KEY, false)
        }
        updateBootReceiver(context)
    }

    fun shouldSkipStorage(context: Context): Boolean {
        return preferences(context).getBoolean(SKIP_STORE_SUPERKEY_KEY, false)
    }

    fun setSkipStorage(context: Context, skip: Boolean) {
        preferences(context).edit {
            putBoolean(SKIP_STORE_SUPERKEY_KEY, skip)
            if (skip) {
                remove(SAVED_SUPERKEY_KEY)
                putBoolean(AUTO_AUTHENTICATE_KEY, false)
            }
        }
        updateBootReceiver(context)
    }

    fun isAutoAuthenticationEnabled(context: Context): Boolean {
        return preferences(context).getBoolean(AUTO_AUTHENTICATE_KEY, false)
    }

    fun setAutoAuthenticationEnabled(context: Context, enabled: Boolean) {
        preferences(context).edit { putBoolean(AUTO_AUTHENTICATE_KEY, enabled) }
        updateBootReceiver(context)
    }

    fun shouldAutoAuthenticate(context: Context): Boolean {
        return isAutoAuthenticationEnabled(context) && hasSavedSuperKey(context)
    }

    fun launchAutoAuthentication(context: Context, trigger: String): Boolean {
        val appContext = context.applicationContext
        if (!shouldAutoAuthenticate(appContext)) {
            updateBootReceiver(appContext)
            return false
        }
        return runCatching {
            appContext.startService(
                Intent(appContext, SuperKeyAuthenticationService::class.java).apply {
                    action = SuperKeyAuthenticationService.ACTION_AUTHENTICATE_SAVED_SUPERKEY
                    putExtra(SuperKeyAuthenticationService.EXTRA_TRIGGER, trigger)
                },
            )
            true
        }.getOrElse {
            Log.e(TAG, "failed to start automatic SuperKey authentication", it)
            false
        }
    }

    private fun updateBootReceiver(context: Context) {
        BaseBootCompletedReceiver.setEnabled(
            context,
            BootCompletedReceiver::class.java,
            shouldAutoAuthenticate(context),
            TAG,
        )
    }

    private fun preferences(context: Context) = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
}
