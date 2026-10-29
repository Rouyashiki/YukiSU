package com.anatdx.yukisu.ui.webui

import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.compose.setContent
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.KernelSUTheme
import com.anatdx.yukisu.ui.theme.ThemeConfig
import com.anatdx.yukisu.ui.theme.ThemeManager
import com.anatdx.yukisu.ui.util.listModules
import com.anatdx.yukisu.ui.util.setTaskDescriptionLabel
import com.dergoogler.mmrl.hybridwebui.interfaces.prebuilt.FileChooserInterface
import com.dergoogler.mmrl.platform.PlatformManager
import com.dergoogler.mmrl.webui.activity.WXActivity
import com.dergoogler.mmrl.webui.util.WebUIOptions
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray

class WebUIXActivity : WXActivity() {
    private var hostedView by mutableStateOf<WebUIXHost?>(null)
    private val themeReady = CompletableDeferred<Pair<ColorScheme, Boolean>>()

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeManager.loadThemeMode(this)
        ThemeManager.loadThemeColors(this)
        ThemeManager.loadDynamicColorState(this)
        ThemeManager.loadUiStyle(this)
        ThemeManager.loadMotionPreset(this)
        ThemeManager.loadPageTransitionStyle(this)
        super.onCreate(savedInstanceState)
    }

    override suspend fun onRender(scope: CoroutineScope) {
        super.onRender(scope)
        setContent {
            KernelSUTheme {
                val colors = MaterialTheme.colorScheme
                val dark = ThemeConfig.forceDarkMode ?: isSystemInDarkTheme()
                SideEffect { themeReady.complete(colors to dark) }
                val content = hostedView
                if (content == null) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator()
                    }
                } else {
                    AndroidView(factory = { content.view }, modifier = Modifier.fillMaxSize())
                }
            }
        }

        val id = modId ?: failInitialization(getString(R.string.unknown_module))
        try {
            val module = withContext(Dispatchers.IO) {
                val modules = JSONArray(listModules())
                (0 until modules.length()).asSequence().map { modules.getJSONObject(it) }
                    .find { it.optString("dir_id", it.optString("id")) == id.id }
            } ?: failInitialization(getString(R.string.no_such_module, id.id))
            val name = module.optString("name", id.id)
            if (!module.optBoolean("web") || !module.optBoolean("enabled") ||
                module.optBoolean("update") || module.optBoolean("remove")
            ) {
                failInitialization(getString(R.string.module_unavailable, name))
            }
            if (!initPlatform()) failInitialization(getString(R.string.operation_failed))

            val (colors, dark) = themeReady.await()
            val prefs = getSharedPreferences("settings", MODE_PRIVATE)
            val platformVersion = PlatformManager.get(-1) { moduleManager.versionCode }
            val options = WebUIOptions(
                modId = id,
                context = this,
                debug = BuildConfig.DEBUG && prefs.getBoolean("enable_web_debugging", false),
                enableEruda = BuildConfig.DEBUG && prefs.getBoolean("use_webuix_eruda", false),
                disableGlobalExitConfirm = true,
                forceKillWebUIProcess = false,
                pluginsEnabled = true,
                isDarkMode = dark,
                colorScheme = colors,
                userAgentString = "YukiSU/${BuildConfig.VERSION_NAME} (Linux; Android ${Build.VERSION.RELEASE}; ${Build.MODEL}; KsuNext/$platformVersion)",
                cls = WebUIXActivity::class.java,
            )
            val content = WebUIXHost(options)
            view = content.view
            hostedView = content
            val ready = initializeWebUi(onFailure = { Log.e("WebUIX", "WebView initialization failed", it) }) {
                content.awaitReady()
                true
            }
            if (!ready) failInitialization(getString(R.string.operation_failed))

            content.wx.addJavascriptInterface(WebUIXBridge(this, content.wx, id.id), "ksu")
            val fileChooser = FileChooserInterface(this, content.wx)
            content.wx.addJavascriptInterface(fileChooser, fileChooser.name)
            setTaskDescriptionLabel("${getString(R.string.app_name)} - $name")
            if (lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
                content.wx.onActivityResumeInterfaces()
            }
            content.wx.loadDomain()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Log.e("WebUIX", "Unable to open WebUI X", e)
            failInitialization(getString(R.string.operation_failed))
        }
    }

    private fun failInitialization(message: String): Nothing {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        finishAndRemoveTask()
        throw CancellationException("WebUI X initialization failed")
    }

    override fun onDestroy() {
        val content = view
        super.onDestroy()
        content?.let {
            (it.parent as? ViewGroup)?.removeView(it)
            it.wx.destroy()
        }
        view = null
        hostedView = null
        themeReady.cancel()
        setTaskDescriptionLabel(getString(R.string.app_name))
    }
}
