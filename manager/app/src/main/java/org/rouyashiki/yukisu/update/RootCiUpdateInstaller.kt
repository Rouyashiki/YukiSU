package org.rouyashiki.yukisu.update

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.content.pm.PackageInstaller
import android.os.IBinder
import android.os.ParcelFileDescriptor
import androidx.core.content.ContextCompat
import org.rouyashiki.yukisu.ICiUpdateInstaller
import org.rouyashiki.yukisu.R
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.io.Closeable
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

internal class RootCiUpdateInstaller private constructor(
    private val context: Context,
    private val remote: ICiUpdateInstaller,
    private val connection: ServiceConnection,
) : Closeable {
    fun createSession(apkSize: Long): Int =
        remote.createSession(apkSize, context.packageName)

    suspend fun writeSession(
        sessionId: Int,
        apkSize: Long,
        write: suspend (OutputStream) -> Unit,
    ) = coroutineScope {
        val pipe = ParcelFileDescriptor.createReliablePipe()
        val readSide = pipe[0]
        val writeSide = pipe[1]
        val remoteWrite = async(Dispatchers.IO) {
            readSide.use {
                remote.writeSession(sessionId, it, apkSize)
            }
        }

        val output = ParcelFileDescriptor.AutoCloseOutputStream(writeSide)
        try {
            write(output)
            output.close()
            remoteWrite.await()
        } catch (error: Exception) {
            runCatching {
                writeSide.closeWithError(
                    error.message
                        ?: context.getString(R.string.ci_update_error_stream_failed)
                )
            }
            runCatching { output.close() }
            runCatching { remoteWrite.await() }
            throw error
        }
    }

    @SuppressLint("UnspecifiedImmutableFlag")
    suspend fun commitSession(sessionId: Int) {
        val action = "${context.packageName}.CI_UPDATE_RESULT.${UUID.randomUUID()}"
        val result = CompletableDeferred<Intent>()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent) {
                result.complete(intent)
            }
        }
        ContextCompat.registerReceiver(
            context,
            receiver,
            IntentFilter(action),
            // PackageInstaller is outside this app; the per-request UUID keeps
            // the exported result channel unguessable.
            ContextCompat.RECEIVER_EXPORTED,
        )
        val pendingIntent = PendingIntent.getBroadcast(
            context,
            sessionId,
            Intent(action).setPackage(context.packageName),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
        )

        try {
            remote.commitSession(sessionId, pendingIntent.intentSender)
            val statusIntent = withTimeout(INSTALL_RESULT_TIMEOUT_MS) { result.await() }
            val status = statusIntent.getIntExtra(
                PackageInstaller.EXTRA_STATUS,
                PackageInstaller.STATUS_FAILURE,
            )
            check(status == PackageInstaller.STATUS_SUCCESS) {
                statusIntent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                    ?: context.getString(R.string.ci_update_error_install_status, status)
            }
        } finally {
            pendingIntent.cancel()
            runCatching { context.unregisterReceiver(receiver) }
        }
    }

    fun abandonSession(sessionId: Int) {
        remote.abandonSession(sessionId)
    }

    override fun close() {
        runCatching { RootService.unbind(connection) }
    }

    companion object {
        private const val INSTALL_RESULT_TIMEOUT_MS = 120_000L

        suspend fun connect(context: Context): RootCiUpdateInstaller =
            withContext(Dispatchers.Main.immediate) {
                suspendCancellableCoroutine { continuation ->
                    val completed = AtomicBoolean(false)
                    val appContext = context.applicationContext
                    lateinit var connection: ServiceConnection
                    connection = object : ServiceConnection {
                        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                            if (!completed.compareAndSet(false, true)) return
                            if (binder == null) {
                                continuation.resumeWithException(
                                    IllegalStateException(
                                        appContext.getString(
                                            R.string.ci_update_error_installer_unavailable
                                        )
                                    )
                                )
                                return
                            }
                            continuation.resume(
                                RootCiUpdateInstaller(
                                    context = appContext,
                                    remote = ICiUpdateInstaller.Stub.asInterface(binder),
                                    connection = this,
                                )
                            )
                        }

                        override fun onServiceDisconnected(name: ComponentName?) {
                            if (completed.compareAndSet(false, true)) {
                                continuation.resumeWithException(
                                    IllegalStateException(
                                        appContext.getString(
                                            R.string.ci_update_error_installer_unavailable
                                        )
                                    )
                                )
                            }
                        }
                    }

                    continuation.invokeOnCancellation {
                        if (completed.compareAndSet(false, true)) {
                            runCatching { RootService.unbind(connection) }
                        }
                    }

                    try {
                        val intent = Intent(appContext, CiUpdateInstallerService::class.java)
                        val task = RootService.bindOrTask(intent, Shell.EXECUTOR, connection)
                        task?.let { Shell.getShell().execTask(it) }
                    } catch (error: Exception) {
                        if (completed.compareAndSet(false, true)) {
                            continuation.resumeWithException(error)
                        }
                    }
                }
            }
    }
}
