package org.rouyashiki.yukisu.magica

import android.app.ZygotePreload
import android.content.pm.ApplicationInfo
import android.util.Log
import java.io.File

open class AppZygotePreload : ZygotePreload {
    override fun doPreload(appInfo: ApplicationInfo) {
        val ksud = File(appInfo.nativeLibraryDir, "libksud.so")
        try {
            System.loadLibrary("kernelsu")
            Log.d(TAG, "executing magica bootstrap")
            forkDontCareAndExecKsud(ksud.absolutePath)
        } catch (t: Throwable) {
            Log.e(TAG, "failed to trigger magica bootstrap", t)
        }
    }

    private companion object {
        private const val TAG = "YukiSUMagica"

        @JvmStatic
        private external fun forkDontCareAndExecKsud(ksudPath: String)
    }
}
