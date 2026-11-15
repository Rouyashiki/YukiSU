package org.rouyashiki.yukisu.update

import android.annotation.SuppressLint
import android.content.Intent
import android.content.IntentSender
import android.content.pm.PackageInstaller
import android.os.IBinder
import android.os.ParcelFileDescriptor
import org.rouyashiki.yukisu.ICiUpdateInstaller
import org.rouyashiki.yukisu.R
import com.topjohnwu.superuser.ipc.RootService

class CiUpdateInstallerService : RootService() {
    private val installer: PackageInstaller
        get() = packageManager.packageInstaller

    private inner class InstallerStub : ICiUpdateInstaller.Stub() {
        override fun createSession(apkSize: Long, packageName: String): Int {
            require(apkSize in 1..MAX_APK_BYTES) {
                getString(R.string.ci_update_error_apk_invalid)
            }
            require(packageName.isNotBlank()) {
                getString(R.string.ci_update_error_install_failed)
            }

            val params = PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL
            ).apply {
                setAppPackageName(packageName)
                setSize(apkSize)
                enableReplaceExisting()
            }
            return installer.createSession(params)
        }

        override fun writeSession(
            sessionId: Int,
            apkStream: ParcelFileDescriptor,
            apkSize: Long,
        ) {
            require(apkSize in 1..MAX_APK_BYTES) {
                getString(R.string.ci_update_error_apk_invalid)
            }
            installer.openSession(sessionId).use { session ->
                ParcelFileDescriptor.AutoCloseInputStream(apkStream).use { input ->
                    session.openWrite(APK_SPLIT_NAME, 0L, apkSize).use { output ->
                        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                        var total = 0L
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            total += count
                            check(total <= apkSize) {
                                getString(R.string.ci_update_error_apk_invalid)
                            }
                            output.write(buffer, 0, count)
                        }
                        check(total == apkSize) {
                            getString(
                                R.string.ci_update_error_apk_truncated,
                                apkSize,
                                total,
                            )
                        }
                        session.fsync(output)
                    }
                }
            }
        }

        override fun commitSession(sessionId: Int, statusReceiver: IntentSender) {
            installer.openSession(sessionId).use { session ->
                session.commit(statusReceiver)
            }
        }

        override fun abandonSession(sessionId: Int) {
            if (installer.getSessionInfo(sessionId) != null) {
                installer.abandonSession(sessionId)
            }
        }
    }

    override fun onBind(intent: Intent): IBinder = InstallerStub()

    @SuppressLint("DiscouragedPrivateApi")
    private fun PackageInstaller.SessionParams.enableReplaceExisting() {
        val installFlags = javaClass.getDeclaredField("installFlags").apply {
            isAccessible = true
        }
        installFlags.setInt(
            this,
            installFlags.getInt(this) or INSTALL_REPLACE_EXISTING,
        )
    }

    private companion object {
        const val APK_SPLIT_NAME = "base.apk"
        const val INSTALL_REPLACE_EXISTING = 0x00000002
        const val MAX_APK_BYTES = 32L * 1024 * 1024
    }
}
