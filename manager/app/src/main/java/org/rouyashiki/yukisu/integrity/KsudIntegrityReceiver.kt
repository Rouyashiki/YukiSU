package org.rouyashiki.yukisu.integrity

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

class KsudIntegrityReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != KsudIntegrity.ACTION_CHANGED) {
            return
        }

        val pendingResult = goAsync()
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            try {
                KsudIntegrity.initialize(context)
                KsudIntegrity.refresh(context, notifyOnMismatch = true)
            } catch (error: Throwable) {
                Log.e(TAG, "failed to verify ksud integrity", error)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private companion object {
        const val TAG = "KsudIntegrityReceiver"
    }
}
