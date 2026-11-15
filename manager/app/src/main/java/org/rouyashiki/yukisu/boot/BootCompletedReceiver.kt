package org.rouyashiki.yukisu.boot

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.util.Log

/** Shared boot-broadcast plumbing for independently enabled boot features. */
abstract class BootCompletedReceiver : BroadcastReceiver() {
    final override fun onReceive(context: Context, intent: Intent?) {
        val action = intent?.action ?: return
        if (acceptsAction(action)) {
            onBootCompleted(context, action)
        }
    }

    protected open fun acceptsAction(action: String): Boolean {
        return action == Intent.ACTION_LOCKED_BOOT_COMPLETED ||
            action == Intent.ACTION_BOOT_COMPLETED
    }

    protected abstract fun onBootCompleted(context: Context, action: String)

    companion object {
        fun setEnabled(
            context: Context,
            receiverClass: Class<out BroadcastReceiver>,
            enabled: Boolean,
            logTag: String,
        ) {
            val appContext = context.applicationContext
            runCatching {
                appContext.packageManager.setComponentEnabledSetting(
                    ComponentName(appContext, receiverClass),
                    if (enabled) {
                        PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                    } else {
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED
                    },
                    PackageManager.DONT_KILL_APP,
                )
            }.onFailure {
                Log.e(logTag, "failed to update boot receiver state to $enabled", it)
            }
        }
    }
}
