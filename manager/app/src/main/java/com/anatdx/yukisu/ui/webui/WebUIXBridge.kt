package com.anatdx.yukisu.ui.webui

import android.webkit.JavascriptInterface
import androidx.activity.ComponentActivity
import com.dergoogler.mmrl.hybridwebui.HybridWebUI
import com.dergoogler.mmrl.hybridwebui.interfaces.JavaScriptInterface as HybridInterface

internal class WebUIXBridge(
    activity: ComponentActivity,
    view: HybridWebUI,
    moduleId: String,
) : HybridInterface(activity, view) {
    override var name = "ksu"
    private val bridge = WebViewInterface(activity, view, moduleId)

    @JavascriptInterface
    fun io() = bridge.io()

    @JavascriptInterface
    fun pm() = bridge.pm()

    @JavascriptInterface
    fun exec(command: String): String = bridge.exec(command)

    @JavascriptInterface
    fun exec(command: String, callback: String) = bridge.exec(command, callback)

    @JavascriptInterface
    fun exec(command: String, options: String?, callback: String) = bridge.exec(command, options, callback)

    @JavascriptInterface
    fun spawn(command: String, args: String, options: String?, callback: String) =
        bridge.spawn(command, args, options, callback)

    @JavascriptInterface
    fun toast(message: String) = bridge.toast(message)

    @JavascriptInterface
    fun fullScreen(enable: Boolean) = bridge.fullScreen(enable)

    @JavascriptInterface
    fun moduleInfo(): String = bridge.moduleInfo()

    @Suppress("DEPRECATION")
    @JavascriptInterface
    fun listPackages(type: String): String = bridge.listPackages(type)

    @Suppress("DEPRECATION")
    @JavascriptInterface
    fun getPackagesInfo(packages: String): String = bridge.getPackagesInfo(packages)

    @JavascriptInterface
    fun exit() = bridge.exit()

    override fun onDestroy() {
        bridge.destroy()
        super.onDestroy()
    }
}
