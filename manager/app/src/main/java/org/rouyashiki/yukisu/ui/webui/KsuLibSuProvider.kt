package org.rouyashiki.yukisu.ui.webui

import android.content.ServiceConnection
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.ksuApp
import com.dergoogler.mmrl.platform.Platform
import com.dergoogler.mmrl.platform.Platform.Companion.createPlatformIntent
import com.dergoogler.mmrl.platform.PlatformManager
import com.dergoogler.mmrl.platform.model.IProvider
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlin.coroutines.coroutineContext

class KsuLibSuProvider : IProvider {
    override val name = "KsuLibSu"
    override fun isAvailable() = true
    override suspend fun isAuthorized() = Natives.isManager

    override fun bind(connection: ServiceConnection) {
        RootService.bind(ksuApp.createPlatformIntent<SuService>(Platform.KsuNext), connection)
    }

    override fun unbind(connection: ServiceConnection) {
        Handler(Looper.getMainLooper()).post {
            runCatching { RootService.unbind(connection) }.onFailure {
                Log.e("KsuLibSu", "Failed to release WebUI service connection", it)
            }
        }
    }
}

private val platformInitMutex = Mutex()

suspend fun initPlatform(): Boolean = initializeWebUi(
    onFailure = { Log.e("KsuLibSu", "Failed to initialize platform", it) },
) {
    platformInitMutex.withLock {
        withContext(Dispatchers.Main.immediate) {
            if (PlatformManager.mServiceOrNull?.asBinder()?.isBinderAlive == true) {
                return@withContext PlatformManager.state()
            }
            PlatformManager.mServiceOrNull = null
            PlatformManager.state()
            val service = PlatformManager.from(KsuLibSuProvider())
            coroutineContext.ensureActive()
            check(service.asBinder().isBinderAlive) { "WebUI service disconnected during initialization" }
            PlatformManager.mServiceOrNull = service
            PlatformManager.state()
        }
    }
}
