package org.rouyashiki.yukisu.superkey

import android.content.Context
import android.content.Intent
import org.rouyashiki.yukisu.boot.BootCompletedReceiver as BaseBootCompletedReceiver

/** Starts exactly one saved-SuperKey authentication attempt after user unlock. */
class BootCompletedReceiver : BaseBootCompletedReceiver() {
    override fun acceptsAction(action: String): Boolean {
        // saved_superkey lives in credential-encrypted storage.
        return action == Intent.ACTION_BOOT_COMPLETED
    }

    override fun onBootCompleted(context: Context, action: String) {
        SuperKeyHelper.launchAutoAuthentication(context, action)
    }
}
