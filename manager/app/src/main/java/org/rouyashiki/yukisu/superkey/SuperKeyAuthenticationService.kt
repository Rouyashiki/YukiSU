package org.rouyashiki.yukisu.superkey

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import org.rouyashiki.yukisu.Natives
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** A non-restarting background action that authenticates a saved SuperKey once. */
class SuperKeyAuthenticationService : Service() {
    private val executor = Executors.newSingleThreadExecutor()
    private val authenticationStarted = AtomicBoolean(false)

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action != ACTION_AUTHENTICATE_SAVED_SUPERKEY) {
            stopSelfResult(startId)
            return START_NOT_STICKY
        }

        if (authenticationStarted.compareAndSet(false, true)) {
            val trigger = intent.getStringExtra(EXTRA_TRIGGER) ?: "unknown"
            executor.execute {
                try {
                    val superKey = SuperKeyHelper.getSavedSuperKey(this)
                    val success = if (
                        SuperKeyHelper.isAutoAuthenticationEnabled(this) && superKey != null
                    ) {
                        // One logical authentication attempt. The key never leaves this process.
                        Natives.authenticateSuperKey(superKey)
                    } else {
                        false
                    }
                    Log.i(TAG, "automatic SuperKey authentication success=$success trigger=$trigger")
                } catch (throwable: Throwable) {
                    Log.e(TAG, "automatic SuperKey authentication failed", throwable)
                } finally {
                    // START_NOT_STICKY + stopSelf means a failure is swallowed without retry.
                    stopSelf()
                }
            }
        }

        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "YukiSUSuperKey"
        const val ACTION_AUTHENTICATE_SAVED_SUPERKEY =
            "org.rouyashiki.yukisu.superkey.AUTHENTICATE_SAVED_SUPERKEY"
        const val EXTRA_TRIGGER = "trigger"
    }
}
