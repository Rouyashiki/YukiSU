package com.anatdx.yukisu.ui.webui

import android.app.Activity
import android.app.Application
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.webkit.WebView
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel.AppInfo
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.json.JSONArray
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.LooperMode
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertSame
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
@LooperMode(LooperMode.Mode.PAUSED)
@OptIn(ExperimentalCoroutinesApi::class)
class WebUIPackageLoadingTest {
    private val cache = SuperUserViewModel::class.java.getDeclaredField("cachedPackageApps").apply {
        isAccessible = true
    }
    private val loadMutex = SuperUserViewModel::class.java.getDeclaredField("fetchAppListMutex").let {
        it.isAccessible = true
        it.get(null) as Mutex
    }
    private var previousCache: Any? = null

    @Before
    fun clearCache() {
        previousCache = cache.get(null)
        cache.set(null, null)
    }

    @After
    fun restoreCache() {
        cache.set(null, previousCache)
    }

    @Test
    fun coldBridgeQueriesReturnWhilePackageLoadingIsPaused() {
        assertBridgeQueries(emptyList())
    }

    @Test
    fun bridgeQueriesReadPublishedPackagesDuringRefresh() {
        val packages = packages()
        cache.set(null, packages)
        assertBridgeQueries(packages)
    }

    @Test
    fun preloadWaitsForThePublishedInventoryBeforeContinuing() = runTest {
        assertTrue(loadMutex.tryLock())
        val preload = async(start = CoroutineStart.UNDISPATCHED) { preloadWebUIApps() }
        val packages = packages()
        try {
            assertFalse(preload.isCompleted)
            cache.set(null, packages)
        } finally {
            loadMutex.unlock()
        }
        runCurrent()
        preload.await()
        assertSame(packages, webUIApps())
    }

    @Suppress("DEPRECATION")
    private fun assertBridgeQueries(expected: List<AppInfo>) {
        val activity = Robolectric.buildActivity(Activity::class.java).setup()
        val view = WebView(activity.get())
        val bridge = WebViewInterface(activity.get(), view, "test")
        val executor = Executors.newSingleThreadExecutor()
        assertTrue(loadMutex.tryLock())
        val query = executor.submit {
            assertEquals(expected, webUIApps())
            val packageNames = if (expected.isEmpty()) "[]" else "[\"test.package\"]"
            assertEquals(packageNames, bridge.pm().listPackages("all"))
            assertEquals(packageNames, bridge.listPackages("all"))
            for (result in listOf(
                bridge.pm().getPackagesInfo("[\"test.package\"]"),
                bridge.getPackagesInfo("[\"test.package\"]"),
            )) {
                val info = JSONArray(result).getJSONObject(0)
                assertEquals(expected.isEmpty(), info.has("error"))
                if (expected.isNotEmpty()) assertEquals(10001, info.getInt("uid"))
            }
        }
        try {
            query.get(5, TimeUnit.SECONDS)
        } finally {
            query.cancel(true)
            executor.shutdownNow()
            executor.awaitTermination(5, TimeUnit.SECONDS)
            loadMutex.unlock()
            bridge.destroy()
            view.destroy()
            activity.pause().stop().destroy()
        }
    }

    private fun packages() = listOf(AppInfo(
        label = "Test package",
        packageInfo = PackageInfo().apply {
            packageName = "test.package"
            applicationInfo = ApplicationInfo().apply { uid = 10001 }
        },
        profile = null,
    ))
}
