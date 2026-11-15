package org.rouyashiki.yukisu.ui.webui

import android.content.Intent
import android.widget.FrameLayout
import android.util.Log
import androidx.activity.result.ActivityResult
import androidx.compose.material3.surfaceColorAtElevation
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.unit.dp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.doOnAttach
import com.dergoogler.mmrl.ext.createNewWX
import com.dergoogler.mmrl.hybridwebui.HybridWebUIStore
import com.dergoogler.mmrl.hybridwebui.interfaces.JavaScriptInterface
import com.dergoogler.mmrl.hybridwebui.interfaces.JavaScriptInterfaceImplementation
import com.dergoogler.mmrl.webui.interfaces.WXInterface
import com.dergoogler.mmrl.webui.interfaces.WXOptions
import com.dergoogler.mmrl.webui.util.WebUIOptions
import com.dergoogler.mmrl.webui.view.WXSwipeRefresh
import com.dergoogler.mmrl.webui.view.WXView
import com.dergoogler.mmrl.webui.view.WebUIXView
import kotlinx.coroutines.CompletableDeferred

internal class WebUIXHost(options: WebUIOptions) {
    private val ready = CompletableDeferred<Unit>()
    val view = WebUIXView(options.context)
    val wx get() = view.wx

    init {
        view.apply {
            id = com.dergoogler.mmrl.webui.R.id.webuixview
            this.options = options
            wx = createActivityWebView(options, ready)
            swipeView = WXSwipeRefresh(options.context, wx)
            val params = FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            layoutParams = params
            if (options.config.pullToRefresh) {
                with(options.colorScheme) {
                    swipeView.setProgressBackgroundColorSchemeColor(surfaceColorAtElevation(1.dp).toArgb())
                    swipeView.setColorSchemeColors(primary.toArgb(), secondary.toArgb())
                }
                var initialOffsetSet = false
                wx.doOnAttach { attached ->
                    ViewCompat.setOnApplyWindowInsetsListener(attached) { _, insets ->
                        val top = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
                        if (!initialOffsetSet && top > 0) {
                            swipeView.setProgressViewOffset(false, 0, top + 32)
                            initialOffsetSet = true
                        }
                        insets
                    }
                }
                swipeView.setOnRefreshListener(this)
                swipeView.addView(wx, params)
                addView(swipeView, params)
            } else {
                addView(wx, params)
            }
        }
    }

    suspend fun awaitReady() = ready.await()
}

private fun createActivityWebView(hostOptions: WebUIOptions, ready: CompletableDeferred<Unit>): WXView =
    object : WXView(hostOptions) {
        private var storeReady = false
        private var viewReady = false
        private var destroyNotified = false
        private val registeredInterfaces = linkedMapOf<String, JavaScriptInterface>()

        override fun onReady(store: HybridWebUIStore) {
            try {
                super.onReady(store)
                storeReady = true
                completeReady()
            } catch (e: Exception) {
                ready.completeExceptionally(e)
            }
        }

        override suspend fun onInit() {
            try {
                super.onInit()
                viewReady = true
                completeReady()
            } catch (e: Exception) {
                ready.completeExceptionally(e)
            }
        }

        private fun completeReady() {
            if (storeReady && viewReady) ready.complete(Unit)
        }

        override fun addJavascriptInterface(obj: JavaScriptInterfaceImplementation<out JavaScriptInterface>) {
            // v438 constructs a detached Activity for WX interfaces; use the attached owner.
            val instance = if (WXInterface::class.java.isAssignableFrom(obj.clazz)) {
                obj.createNewWX(WXOptions(activity, this, hostOptions))
            } else {
                obj.createNew(activity, this)
            }
            addJavascriptInterface(checkNotNull(instance) { "Unable to create ${obj.clazz.name}" })
        }

        override fun addJavascriptInterface(obj: Any, name: String) {
            super.addJavascriptInterface(obj, name)
            if (obj is JavaScriptInterface && isStoreInitialized) {
                registeredInterfaces.putIfAbsent(name, obj)
            }
        }

        private fun forEachWxInterface(block: (WXInterface) -> Unit) {
            registeredInterfaces.values.toList().forEach {
                if (it is WXInterface) {
                    runCatching { block(it) }.onFailure { error ->
                        Log.e("WebUIX", "Interface lifecycle callback failed", error)
                    }
                }
            }
        }

        override fun onActivityResumeInterfaces() = forEachWxInterface { it.onActivityResume() }
        override fun onActivityPauseInterfaces() = forEachWxInterface { it.onActivityPause() }
        override fun onActivityStopInterfaces() = forEachWxInterface { it.onActivityStop() }
        override fun onActivityResult(result: ActivityResult) = forEachWxInterface { it.onActivityResult(result) }

        @Deprecated("WebUI X still uses this callback for its legacy file picker")
        @Suppress("DEPRECATION")
        override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) =
            forEachWxInterface { it.onActivityResult(requestCode, resultCode, data) }

        override fun onActivityDestroyInterfaces() {
            if (!destroyNotified) {
                destroyNotified = true
                forEachWxInterface { it.onActivityDestroy() }
            }
        }

        override fun clearState() {
            // The upstream store clears its interfaces before dispatching destruction.
            onActivityDestroyInterfaces()
            registeredInterfaces.values.toList().forEach {
                runCatching { it.onDestroy() }.onFailure { error ->
                    Log.e("WebUIX", "Interface cleanup failed", error)
                }
            }
            registeredInterfaces.clear()
            super.clearState()
        }

        override fun destroy() {
            ready.cancel()
            super.destroy()
        }
    }
