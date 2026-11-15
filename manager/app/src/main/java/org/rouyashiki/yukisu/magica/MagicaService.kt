package org.rouyashiki.yukisu.magica

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder

open class MagicaService : Service() {
    override fun onBind(intent: Intent?): IBinder? = Binder()
}
