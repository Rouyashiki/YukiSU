package org.rouyashiki.yukisu.magica

import android.content.Context
import android.content.Intent
import org.rouyashiki.yukisu.boot.BootCompletedReceiver as BaseBootCompletedReceiver
import org.rouyashiki.yukisu.ui.util.rootAvailable

open class BootCompletedReceiver : BaseBootCompletedReceiver() {
    override fun acceptsAction(action: String): Boolean {
        return super.acceptsAction(action) || action == ACTION_LAUNCH
    }

    override fun onBootCompleted(context: Context, action: String) {
        if (rootAvailable()) {
            return
        }

        MagicaHelper.launch(context, action)
    }

    companion object {
        const val ACTION_LAUNCH = "org.rouyashiki.yukisu.magica.LAUNCH"
    }
}
