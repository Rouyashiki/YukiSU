package com.anatdx.yukisu.ui.webui

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.util.Log
import android.webkit.JavascriptInterface
import android.webkit.JsPromptResult
import android.webkit.JsResult
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.webkit.WebViewAssetLoader
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.util.createRootShell
import com.anatdx.yukisu.ui.util.listModules
import com.anatdx.yukisu.ui.util.setTaskDescriptionLabel
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File

// WebView blocks its UI thread during synchronous Java bridge calls.
internal fun webUIApps(): List<SuperUserViewModel.AppInfo> = SuperUserViewModel.getAppsForWebUI()

internal suspend fun preloadWebUIApps() {
    try {
        SuperUserViewModel.loadAppsForWebUI()
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (error: Exception) {
        Log.e("WebUI", "Failed to load package inventory", error)
    }
}

@SuppressLint("SetJavaScriptEnabled")
internal suspend fun prepareWebView(activity: Activity, moduleId: String, state: WebUIState) {
    withContext(Dispatchers.IO) {
        val modules = JSONArray(listModules())
        val module = (0 until modules.length()).asSequence()
            .map { modules.getJSONObject(it) }
            .find { it.optString("dir_id", it.optString("id")) == moduleId }

        if (module == null) {
            withContext(Dispatchers.Main) {
                state.uiEvent = WebUIEvent.Error(activity.getString(R.string.no_such_module, moduleId))
            }
            return@withContext
        }

        val name = module.optString("name", moduleId)
        if (!module.optBoolean("web") || !module.optBoolean("enabled") ||
            module.optBoolean("update") || module.optBoolean("remove")
        ) {
            withContext(Dispatchers.Main) {
                state.uiEvent = WebUIEvent.Error(activity.getString(R.string.module_unavailable, name))
            }
            return@withContext
        }

        preloadWebUIApps()
        val downloadScript = activity.assets.open("webview/download.js").bufferedReader().use { it.readText() }
        val shell = createRootShell(true)
        var attached = false
        try {
            withContext(Dispatchers.Main) {
                state.rootShell = shell
                attached = true
                state.moduleName = name
                state.modDir = "/data/adb/modules/$moduleId"
                activity.setTaskDescriptionLabel("${activity.getString(R.string.app_name)} - $name")

                val prefs = activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
                WebView.setWebContentsDebuggingEnabled(
                    BuildConfig.DEBUG && prefs.getBoolean("enable_web_debugging", false)
                )

                val webView = WebView(activity)
                state.webView = webView
                webView.setBackgroundColor(Color.TRANSPARENT)
                webView.settings.apply {
                    javaScriptEnabled = true
                    domStorageEnabled = true
                    allowFileAccess = false
                }

                val assetLoader = WebViewAssetLoader.Builder()
                    .setDomain("mui.kernelsu.org")
                    .addPathHandler(
                        "/",
                        SuFilePathHandler(
                            File("${state.modDir}/webroot"), shell,
                            { state.currentInsets },
                            { webView.post { state.isInsetsEnabled = true } },
                            { state.colorsCss },
                        )
                    )
                    .build()

                webView.webViewClient = ModuleWebViewClient(
                    activity, assetLoader,
                    { view ->
                        if (state.webView === view) state.webView = null
                        state.requestExit()
                    },
                    { view ->
                        state.webCanGoBack = view.canGoBack()
                        if (state.isInsetsEnabled) view.evaluateJavascript(state.currentInsets.js, null)
                        view.evaluateJavascript(downloadScript, null)
                    },
                )
                webView.webChromeClient = object : WebChromeClient() {
                    override fun onJsAlert(view: WebView?, url: String?, message: String?, result: JsResult?): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowAlert(message, result)
                        return true
                    }

                    override fun onJsConfirm(view: WebView?, url: String?, message: String?, result: JsResult?): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowConfirm(message, result)
                        return true
                    }

                    override fun onJsPrompt(
                        view: WebView?, url: String?, message: String?, defaultValue: String?, result: JsPromptResult?
                    ): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowPrompt(message, defaultValue.orEmpty(), result)
                        return true
                    }

                    override fun onShowFileChooser(
                        webView: WebView?, callback: ValueCallback<Array<Uri>>?, params: FileChooserParams?
                    ): Boolean {
                        state.filePathCallback?.onReceiveValue(null)
                        state.filePathCallback = callback
                        val intent = params?.createIntent() ?: Intent(Intent.ACTION_GET_CONTENT).apply { type = "*/*" }
                        if (params?.mode == FileChooserParams.MODE_OPEN_MULTIPLE) {
                            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                        }
                        state.uiEvent = WebUIEvent.ShowFileChooser(intent)
                        return true
                    }
                }

                val webViewInterface = KsuWebViewInterface(activity, webView, moduleId, state)
                val downloadInterface = WebUIDownloadInterface(state)
                state.webViewInterface = webViewInterface
                state.downloadInterface = downloadInterface
                webView.addJavascriptInterface(webViewInterface, "ksu")
                webView.addJavascriptInterface(downloadInterface, "ksu_download")
                webView.setDownloadListener { url, _, _, _, _ -> downloadInterface.openExternal(url) }
                state.uiEvent = WebUIEvent.WebViewReady
            }
        } finally {
            if (!attached) shell.runCatching { close() }
        }
    }
}

internal class KsuWebViewInterface(
    activity: Activity,
    webView: WebView,
    moduleId: String,
    private val state: WebUIState,
) : WebViewInterface(activity, webView, moduleId) {
    @JavascriptInterface
    fun enableEdgeToEdge(enable: Boolean) {
        webView.post { state.isInsetsEnabled = enable }
    }

    @JavascriptInterface
    override fun fullScreen(enable: Boolean) {
        super.fullScreen(enable)
        enableEdgeToEdge(enable)
    }
}
