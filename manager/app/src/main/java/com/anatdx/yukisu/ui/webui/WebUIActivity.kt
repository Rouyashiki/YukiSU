package com.anatdx.yukisu.ui.webui

import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.KernelSUTheme
import com.anatdx.yukisu.ui.theme.ThemeManager
import kotlinx.coroutines.CancellationException

class WebUIActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        window.isNavigationBarContrastEnforced = false
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
        super.onCreate(savedInstanceState)

        val moduleId = intent.getStringExtra("id") ?: run {
            finishAndRemoveTask()
            return
        }

        ThemeManager.loadThemeMode(this)
        ThemeManager.loadThemeColors(this)
        ThemeManager.loadDynamicColorState(this)
        ThemeManager.loadUiStyle(this)
        ThemeManager.loadMotionPreset(this)
        ThemeManager.loadPageTransitionStyle(this)

        setContent {
            KernelSUTheme {
                val state = remember { WebUIState() }
                val colorScheme = MaterialTheme.colorScheme
                val colorsCss = remember(colorScheme) { MonetColorsProvider.getColorsCss(colorScheme) }
                SideEffect { state.colorsCss = colorsCss }

                LaunchedEffect(moduleId) {
                    try {
                        prepareWebView(this@WebUIActivity, moduleId, state)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        Log.e("WebUIActivity", "Failed to prepare module WebUI", e)
                        state.uiEvent = WebUIEvent.Error(getString(R.string.operation_failed))
                    }
                }

                DisposableEffect(state) {
                    onDispose { state.dispose(this@WebUIActivity) }
                }

                when (val event = state.uiEvent) {
                    is WebUIEvent.Loading -> Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center,
                    ) {
                        CircularProgressIndicator()
                    }
                    is WebUIEvent.Error -> LaunchedEffect(event) {
                        Toast.makeText(this@WebUIActivity, event.message, Toast.LENGTH_SHORT).show()
                        finishAndRemoveTask()
                    }
                    is WebUIEvent.Close -> LaunchedEffect(event) { finishAndRemoveTask() }
                    else -> WebUIScreen(state)
                }
            }
        }
    }
}
