package com.anatdx.yukisu.ui.activity.util

import android.content.Context
import android.widget.Toast
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.runtime.Composable
import androidx.lifecycle.LifecycleCoroutineScope
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.MainActivity
import com.anatdx.yukisu.ui.util.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.*
import android.net.Uri
import androidx.lifecycle.lifecycleScope
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.ui.component.ZipFileDetector
import com.anatdx.yukisu.ui.component.ZipFileInfo
import com.anatdx.yukisu.ui.component.ZipType
import com.anatdx.yukisu.ui.component.Ak3FlashOptions
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.anatdx.yukisu.ui.screen.FlashIt
import kotlinx.coroutines.withContext
import androidx.core.content.edit

object AnimatedBottomBar {
    @Composable
    fun AnimatedBottomBarWrapper(
        showBottomBar: Boolean,
        content: @Composable () -> Unit
    ) {
        val motion = LocalYukiMotion.current
        AnimatedVisibility(
            visible = showBottomBar,
            enter = slideInVertically(motion.spatial(), initialOffsetY = { it }) + fadeIn(motion.effects()),
            exit = slideOutVertically(motion.effects(), targetOffsetY = { it }) + fadeOut(motion.effects())
        ) {
            content()
        }
    }
}

object UltraActivityUtils {

    suspend fun detectZipTypeAndShowConfirmation(
        activity: MainActivity,
        zipUris: ArrayList<Uri>,
        onResult: (List<ZipFileInfo>) -> Unit
    ) {
        val infos = ZipFileDetector.detectAndParseZipFiles(activity, zipUris)
        withContext(Dispatchers.Main) { onResult(infos) }
    }

    fun navigateToFlashScreen(
        activity: MainActivity,
        zipFiles: List<ZipFileInfo>,
        ak3Options: Ak3FlashOptions?,
        navigator: DestinationsNavigator
    ) {
        activity.lifecycleScope.launch {
            val moduleUris = zipFiles.filter { it.type == ZipType.MODULE }.map { it.uri }
            val kernelFile = zipFiles.singleOrNull { it.type == ZipType.KERNEL }

            if (kernelFile != null && moduleUris.isEmpty() && ak3Options != null) {
                navigator.navigate(
                    FlashScreenDestination(
                        FlashIt.FlashAk3(
                            zipPath = kernelFile.stagedPath,
                            targetSlot = ak3Options.targetSlot,
                            useMkbootfs = ak3Options.useMkbootfs,
                        )
                    )
                )
            } else if (moduleUris.isNotEmpty() && kernelFile == null) {
                if (Natives.isSafeMode) {
                    ZipFileDetector.cleanupStagedFiles(zipFiles)
                    Toast.makeText(activity, R.string.safe_mode_module_disabled, Toast.LENGTH_LONG).show()
                    return@launch
                }
                navigator.navigate(
                    FlashScreenDestination(
                        FlashIt.FlashModules(ArrayList(moduleUris))
                    )
                )
            }
        }
    }
}

object AppData {
    object DataRefreshManager {
        private val _superuserCount = MutableStateFlow(0)
        private val _moduleCount = MutableStateFlow(0)
        private val _pluginCount = MutableStateFlow(0)
        private val _isFullFeatured = MutableStateFlow(false)

        val superuserCount: StateFlow<Int> = _superuserCount.asStateFlow()
        val moduleCount: StateFlow<Int> = _moduleCount.asStateFlow()
        val pluginCount: StateFlow<Int> = _pluginCount.asStateFlow()
        val isFullFeatured: StateFlow<Boolean> = _isFullFeatured.asStateFlow()

        fun refreshData() {
            val sc = getSuperuserCountUse()
            val mc = getModuleCountUse()
            val pc = getPluginCountUse()
            val ff = isFullFeatured()
            if (_superuserCount.value != sc) _superuserCount.value = sc
            if (_moduleCount.value != mc) _moduleCount.value = mc
            if (_pluginCount.value != pc) _pluginCount.value = pc
            if (_isFullFeatured.value != ff) _isFullFeatured.value = ff
        }
    }

    fun getSuperuserCountUse(): Int {
        return try {
            if (!rootAvailable()) return 0
            getSuperuserCount()
        } catch (_: Exception) {
            0
        }
    }

    fun getModuleCountUse(): Int {
        return try {
            if (!rootAvailable()) return 0
            getModuleCount()
        } catch (_: Exception) {
            0
        }
    }

    fun getPluginCountUse(): Int {
        return try {
            if (!rootAvailable()) return 0
            getPluginCount()
        } catch (_: Exception) {
            0
        }
    }

    fun isFullFeatured(): Boolean {
        return Natives.isFullFeatured()
    }
}

object DataRefreshUtils {
    fun startDataRefreshCoroutine(scope: LifecycleCoroutineScope) {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                AppData.DataRefreshManager.refreshData()
                delay(10000) // 10s，减少轮询频率
            }
        }
    }

    fun startSettingsMonitorCoroutine(
        scope: LifecycleCoroutineScope,
        activity: MainActivity,
        settingsStateFlow: MutableStateFlow<MainActivity.SettingsState>
    ) {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                val prefs = activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
                val newState = MainActivity.SettingsState(
                    isHideOtherInfo = prefs.getBoolean("is_hide_other_info", false)
                )
                if (settingsStateFlow.value != newState) {
                    settingsStateFlow.value = newState
                }
                delay(5000) // 5s，减少轮询频率
            }
        }
    }

    fun refreshData(scope: LifecycleCoroutineScope) {
        scope.launch {
            AppData.DataRefreshManager.refreshData()
        }
    }
}

object DisplayUtils {
    fun applyCustomDpi(context: Context) {
        val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val customDpi = prefs.getInt("app_dpi", 0)

        if (customDpi > 0) {
            try {
                val resources = context.resources
                val metrics = resources.displayMetrics
                metrics.density = customDpi / 160f
                @Suppress("DEPRECATION")
                metrics.scaledDensity = customDpi / 160f
                metrics.densityDpi = customDpi
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }
}
