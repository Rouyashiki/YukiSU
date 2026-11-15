package org.rouyashiki.yukisu

import android.app.Application
import android.os.Process
import android.system.Os
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.rouyashiki.yukisu.ui.viewmodel.SuperUserViewModel
import org.rouyashiki.yukisu.integrity.KsudIntegrity
import org.rouyashiki.yukisu.update.CiUpdateManager
import coil.Coil
import coil.ImageLoader
import com.dergoogler.mmrl.platform.PlatformManager
import org.rouyashiki.yukisu.ui.util.AppIconFetcherFactory
import me.zhanghai.android.appiconloader.coil.AppIconKeyer
import okhttp3.Cache
import okhttp3.OkHttpClient
import java.io.File
import java.util.Locale

lateinit var ksuApp: KernelSUApplication

class KernelSUApplication : Application(), ViewModelStoreOwner {

    lateinit var okhttpClient: OkHttpClient
    private val appViewModelStore by lazy { ViewModelStore() }

    override fun onCreate() {
        super.onCreate()
        ksuApp = this

        // Boot and isolated Magica processes only need the native/bootstrap environment.
        if (isBootstrapProcess()) {
            Os.setenv("TMPDIR", cacheDir.absolutePath, true)
            return
        }

        KsudIntegrity.initialize(this)

        CiUpdateManager.cleanupCachedUpdate(this)
        CoroutineScope(Dispatchers.IO).launch {
            CiUpdateManager.cleanupLegacyTemporaryUpdates()
        }

        // For faster response when first entering superuser or webui activity
        val superUserViewModel = ViewModelProvider(this)[SuperUserViewModel::class.java]
        CoroutineScope(Dispatchers.Main).launch {
            superUserViewModel.fetchAppList()
        }

        PlatformManager.setHiddenApiExemptions()

        val context = this
        val iconSize = resources.getDimensionPixelSize(android.R.dimen.app_icon_size)
        Coil.setImageLoader(
            ImageLoader.Builder(context)
                .components {
                    add(AppIconKeyer())
                    add(AppIconFetcherFactory(iconSize, context))
                }
                .build()
        )

        val webroot = File(dataDir, "webroot")
        if (!webroot.exists()) {
            webroot.mkdir()
        }

        // Provide working env for rust's temp_dir()
        Os.setenv("TMPDIR", cacheDir.absolutePath, true)

        okhttpClient =
            OkHttpClient.Builder().cache(Cache(File(cacheDir, "okhttp"), 10 * 1024 * 1024))
                .addInterceptor { block ->
                    block.proceed(
                        block.request().newBuilder()
                            .header("User-Agent", "YukiSU/${BuildConfig.VERSION_CODE}")
                            .header("Accept-Language", Locale.getDefault().toLanguageTag()).build()
                    )
                }.build()
    }
    override val viewModelStore: ViewModelStore
        get() = appViewModelStore

    private fun isBootstrapProcess(): Boolean =
        Process.isIsolated() || Application.getProcessName() == "$packageName:boot"
}
