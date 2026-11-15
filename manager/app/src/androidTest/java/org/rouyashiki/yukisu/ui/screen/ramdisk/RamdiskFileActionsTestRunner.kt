package org.rouyashiki.yukisu.ui.screen.ramdisk

import android.app.Application
import android.content.Context
import androidx.test.runner.AndroidJUnitRunner

/** These isolated file-provider tests do not start root services or the Manager application. */
class RamdiskFileActionsTestRunner : AndroidJUnitRunner() {
    override fun newApplication(cl: ClassLoader, className: String, context: Context): Application =
        super.newApplication(cl, Application::class.java.name, context)
}
