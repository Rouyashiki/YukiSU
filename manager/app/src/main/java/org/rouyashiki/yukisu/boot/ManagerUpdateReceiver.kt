package org.rouyashiki.yukisu.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.ui.util.migrateFeatureDefaults
import org.rouyashiki.yukisu.ui.util.withNewRootShell
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class ManagerUpdateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        val pending = goAsync()
        CoroutineScope(Dispatchers.IO).launch {
            try {
                if (Natives.isManager) {
                    withNewRootShell { migrateFeatureDefaults(this) }
                }
            } catch (e: Exception) {
                Log.e("ManagerUpdateReceiver", "Feature migration deferred until manager access", e)
            } finally {
                pending.finish()
            }
        }
    }
}
