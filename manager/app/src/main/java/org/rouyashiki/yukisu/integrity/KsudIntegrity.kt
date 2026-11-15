package org.rouyashiki.yukisu.integrity

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.edit
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.MainActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicLong

enum class KsudIntegrityStatus {
    UNKNOWN,
    MATCH,
    MISMATCH,
    UNAVAILABLE,
}

object KsudIntegrity {
    const val ACTION_CHANGED = "org.rouyashiki.yukisu.action.KSUD_INTEGRITY_CHANGED"
    const val NOTIFICATION_PERMISSION_REQUESTED = "ksud_integrity_notification_requested"

    private const val CHANNEL_ID = "ksud_integrity"
    private const val NOTIFICATION_ID = 0x4b535549
    private const val PREFERENCES = "ksud_integrity"
    private const val PREFERENCE_MISMATCH = "mismatch"

    private val mutableStatus = MutableStateFlow(KsudIntegrityStatus.UNKNOWN)
    private val refreshMutex = Mutex()
    private val updateGeneration = AtomicLong()
    val status: StateFlow<KsudIntegrityStatus> = mutableStatus.asStateFlow()

    fun initialize(context: Context) {
        createNotificationChannel(context)
        if (preferences(context).getBoolean(PREFERENCE_MISMATCH, false)) {
            mutableStatus.value = KsudIntegrityStatus.MISMATCH
        }
    }

    fun markBundledDaemonInstalled(context: Context) {
        updateGeneration.incrementAndGet()
        preferences(context).edit { putBoolean(PREFERENCE_MISMATCH, false) }
        mutableStatus.value = KsudIntegrityStatus.MATCH
        notificationManager(context).cancel(NOTIFICATION_ID)
    }

    fun verifyBundledDaemon(context: Context): KsudIntegrityStatus {
        val source = context.applicationInfo.nativeLibraryDir + File.separator + "libksud.so"
        return when (runCatching { Natives.verifyKsudDaemon(source) }.getOrDefault(
            Natives.KSUD_INTEGRITY_UNAVAILABLE,
        )) {
            Natives.KSUD_INTEGRITY_MATCH -> KsudIntegrityStatus.MATCH
            Natives.KSUD_INTEGRITY_MISMATCH -> KsudIntegrityStatus.MISMATCH
            else -> KsudIntegrityStatus.UNAVAILABLE
        }
    }

    suspend fun refresh(
        context: Context,
        notifyOnMismatch: Boolean,
    ): KsudIntegrityStatus = refreshMutex.withLock {
        val generation = updateGeneration.get()
        val verified = withContext(Dispatchers.IO) {
            verifyBundledDaemon(context)
        }
        if (generation != updateGeneration.get()) {
            return@withLock mutableStatus.value
        }
        when (verified) {
            KsudIntegrityStatus.MATCH -> {
                preferences(context).edit { putBoolean(PREFERENCE_MISMATCH, false) }
                mutableStatus.value = verified
                notificationManager(context).cancel(NOTIFICATION_ID)
            }

            KsudIntegrityStatus.MISMATCH -> {
                preferences(context).edit { putBoolean(PREFERENCE_MISMATCH, true) }
                mutableStatus.value = verified
                if (notifyOnMismatch) {
                    postNotification(context)
                }
            }

            KsudIntegrityStatus.UNAVAILABLE -> {
                if (mutableStatus.value != KsudIntegrityStatus.MISMATCH) {
                    mutableStatus.value = verified
                }
            }

            KsudIntegrityStatus.UNKNOWN -> Unit
        }
        verified
    }

    private fun storageContext(context: Context): Context =
        context.applicationContext.createDeviceProtectedStorageContext()

    private fun preferences(context: Context) =
        storageContext(context).getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    private fun notificationManager(context: Context): NotificationManager =
        context.getSystemService(NotificationManager::class.java)

    private fun createNotificationChannel(context: Context) {
        val channel = NotificationChannel(
            CHANNEL_ID,
            context.getString(R.string.ksud_integrity_notification_channel),
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = context.getString(R.string.ksud_integrity_notification_channel_summary)
        }
        notificationManager(context).createNotificationChannel(channel)
    }

    private fun postNotification(context: Context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }

        val pendingIntent = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val message = context.getString(R.string.ksud_integrity_warning)
        val notification = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ms_warning)
            .setContentTitle(context.getString(R.string.ksud_integrity_notification_title))
            .setContentText(message)
            .setStyle(Notification.BigTextStyle().bigText(message))
            .setCategory(Notification.CATEGORY_ERROR)
            .setContentIntent(pendingIntent)
            .setAutoCancel(true)
            .build()
        notificationManager(context).notify(NOTIFICATION_ID, notification)
    }
}
