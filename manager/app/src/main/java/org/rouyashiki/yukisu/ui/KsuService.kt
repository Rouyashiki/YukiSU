package org.rouyashiki.yukisu.ui

import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.content.pm.Signature
import android.os.*
import android.util.Log
import com.topjohnwu.superuser.ipc.RootService
import org.rouyashiki.yukisu.IKsuInterface
import org.rouyashiki.yukisu.BackupPackage
import java.io.File
import java.nio.file.Files
import java.nio.file.NoSuchFileException
import java.util.concurrent.TimeUnit
import java.lang.reflect.InvocationTargetException
import java.security.MessageDigest
import java.util.UUID

/**
 * @author ShirkNeko
 * @date 2025/10/17.
 */
class KsuService : RootService() {

    private val TAG = "KsuService"

    private val cacheLock = Any()
    private class BackupSnapshot(val packages: List<BackupPackage>) {
        val created = SystemClock.elapsedRealtime()
        val signatures = mutableMapOf<Int, BackupPackage>()
        val digests = mutableMapOf<Signature, String>()
    }
    private val backupSnapshots = mutableMapOf<String, BackupSnapshot>()
    private var _all: List<PackageInfo>? = null
    private val allPackages: List<PackageInfo>
        get() = synchronized(cacheLock) {
            _all ?: loadAllPackages().also { _all = it }
        }

    private fun loadAllPackages(): List<PackageInfo> {
        val tmp = arrayListOf<PackageInfo>()
        val users = try {
            queryAliveUserIds(getSystemService(USER_SERVICE) as UserManager)
        } catch (error: Exception) {
            Log.e(TAG, "Cannot enumerate Android users", error)
            listOf(0)
        }
        for (userId in users) {
            tmp += getInstalledPackagesAsUser(userId)
        }
        return tmp
    }

    internal inner class Stub : IKsuInterface.Stub() {
        override fun beginBackupSnapshot(): String {
            val started = SystemClock.elapsedRealtime()
            val users = queryAliveUserIds(getSystemService(USER_SERVICE) as UserManager)
            val packages = users.flatMap { userId ->
                queryInstalledPackagesAsUser(userId).map { info ->
                    val uid = checkNotNull(info.applicationInfo) { "Package has no application info" }.uid
                    check(uid / 100000 == userId) { "Package user changed during query" }
                    BackupPackage(info.packageName, uid, info.longVersionCode, info.lastUpdateTime)
                }
            }.sortedWith(compareBy({ it.uid }, { it.packageName }))
            check(packages.map { it.userId to it.packageName }.distinct().size == packages.size) {
                "Duplicate package identity"
            }
            val id = UUID.randomUUID().toString()
            synchronized(backupSnapshots) {
                backupSnapshots.entries.removeAll { started - it.value.created > 600_000 }
                check(backupSnapshots.size < 8) { "Too many backup operations" }
                backupSnapshots[id] = BackupSnapshot(packages)
            }
            Log.i(TAG, "Backup package query: ${packages.size} packages, ${SystemClock.elapsedRealtime() - started} ms")
            return id
        }

        override fun getBackupPackages(snapshot: String, start: Int, count: Int): List<BackupPackage> {
            require(start >= 0 && count in 1..BackupPackage.PAGE_SIZE)
            val packages = backupSnapshot(snapshot).packages
            return if (start >= packages.size) emptyList() else packages.subList(start, minOf(start + count, packages.size))
        }

        override fun getBackupSignatures(snapshot: String, indices: IntArray): List<BackupPackage> {
            require(indices.size <= BackupPackage.PAGE_SIZE)
            val data = backupSnapshot(snapshot)
            return synchronized(data) {
                indices.map { index ->
                    require(index in data.packages.indices)
                    data.signatures.getOrPut(index) { readBackupSignature(data.packages[index], data) }
                }
            }
        }

        override fun closeBackupSnapshot(snapshot: String) {
            synchronized(backupSnapshots) { backupSnapshots.remove(snapshot) }
        }

        override fun getBackupProfileRules(keys: Array<String>): List<String> {
            require(keys.size <= 4)
            return keys.map(::readProfileRulesForBackup)
        }

        override fun getBackupUidIdentity(uid: Int): List<BackupPackage> {
            require(uid >= 0)
            val names = packageManager.getPackagesForUid(uid).orEmpty().sorted()
            val current = names.map { name ->
                val info = queryBackupPackage(name, uid / 100000)
                val actualUid = checkNotNull(info.applicationInfo).uid
                check(actualUid == uid) { "UID changed during verification" }
                BackupPackage(name, uid, info.longVersionCode, info.lastUpdateTime)
            }
            val snapshot = BackupSnapshot(current)
            return current.map { readBackupSignature(it, snapshot) }
        }

        override fun getPackageCount(): Int = allPackages.size

        override fun refreshPackages(): Int = synchronized(cacheLock) {
            loadAllPackages().also { _all = it }.size
        }

        override fun getPackages(start: Int, maxCount: Int): List<PackageInfo> {
            val list = allPackages
            val end = (start + maxCount).coerceAtMost(list.size)
            return if (start >= list.size) emptyList()
            else list.subList(start, end)
        }

        override fun getUidPackagesForBackup(uid: Int): Array<String> {
            require(uid >= 0)
            return allPackages
                .filter { it.applicationInfo?.uid == uid }
                .map { it.packageName }.distinct().sorted().toTypedArray()
        }

        override fun readProfileRulesForBackup(packageName: String): String {
            val file = profileRulesFile(packageName)
            return try {
                require(Files.size(file.toPath()) <= 65536) { "SELinux rules too large" }
                String(Files.readAllBytes(file.toPath()), Charsets.UTF_8)
            } catch (_: NoSuchFileException) {
                ""
            }
        }

        override fun checkProfileRulesForBackup(rules: String): Boolean =
            rules.isEmpty() || runProfileCommand("sepolicy", "check", rules)

        override fun writeProfileRulesForBackup(packageName: String, rules: String): Boolean {
            profileRulesFile(packageName)
            require(rules.toByteArray(Charsets.UTF_8).size <= 65536 && '\u0000' !in rules)
            return runProfileCommand("profile", "set-sepolicy", packageName, rules) &&
                readProfileRulesForBackup(packageName) == rules
        }
    }

    private fun backupSnapshot(id: String): BackupSnapshot = synchronized(backupSnapshots) {
        checkNotNull(backupSnapshots[id]) { "Backup snapshot expired" }
    }

    @SuppressLint("PrivateApi")
    private fun queryBackupPackage(name: String, userId: Int): PackageInfo {
        val method = packageManager.javaClass.getDeclaredMethod(
            "getPackageInfoAsUser", String::class.java, Int::class.java, Int::class.java
        )
        return method.invoke(packageManager, name, PackageManager.GET_SIGNING_CERTIFICATES, userId) as PackageInfo
    }

    private fun readBackupSignature(expected: BackupPackage, snapshot: BackupSnapshot): BackupPackage {
        val started = SystemClock.elapsedRealtime()
        val info = try {
            queryBackupPackage(expected.packageName, expected.userId)
        } catch (failure: Exception) {
            val cause = if (failure is InvocationTargetException) failure.targetException else failure
            if (cause is RemoteException) throw IllegalStateException("Package service unavailable", cause)
            Log.w(TAG, "Cannot query signing identity: ${expected.packageName}", cause)
            return expected.copy(status = if (cause is PackageManager.NameNotFoundException) BackupPackage.MISSING else BackupPackage.UNAVAILABLE)
        }
        val queryMs = SystemClock.elapsedRealtime() - started
        if (info.applicationInfo?.uid != expected.uid || info.longVersionCode != expected.versionCode ||
            info.lastUpdateTime != expected.lastUpdateTime) return expected.copy(status = BackupPackage.UNAVAILABLE)
        val signing = info.signingInfo ?: return expected.copy(status = BackupPackage.UNAVAILABLE)
        fun digest(signature: Signature): String = snapshot.digests.getOrPut(signature) {
            val bytes = MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
            val hex = "0123456789abcdef"
            buildString(64) { bytes.forEach { append(hex[(it.toInt() ushr 4) and 15]); append(hex[it.toInt() and 15]) } }
        }
        val history = if (signing.hasMultipleSigners()) emptyList() else signing.signingCertificateHistory?.toList().orEmpty()
        val current = if (signing.hasMultipleSigners()) signing.apkContentsSigners?.toList().orEmpty() else history.takeLast(1)
        if (current.isEmpty() || current.size + history.size > 64) return expected.copy(status = BackupPackage.UNAVAILABLE)
        val digestStarted = SystemClock.elapsedRealtime()
        val currentDigests = current.map(::digest).distinct().sorted()
        val historyDigests = history.map(::digest).distinct()
        Log.d(TAG, "Backup signing identity: query=$queryMs ms, digest=${SystemClock.elapsedRealtime() - digestStarted} ms")
        return expected.copy(
            currentSigners = currentDigests, signingHistory = historyDigests, status = BackupPackage.OK,
            label = runCatching { info.applicationInfo?.loadLabel(packageManager)?.toString()?.take(256) }.getOrNull() ?: expected.packageName
        )
    }

    private fun profileRulesFile(packageName: String): File {
        require(packageName.length < 256 &&
            Regex("[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)*").matches(packageName))
        return File("/data/adb/ksu/profile/selinux", packageName)
    }

    private fun runProfileCommand(vararg arguments: String): Boolean {
        val daemon = File(applicationInfo.nativeLibraryDir, "libksud.so").absolutePath
        val process = ProcessBuilder(listOf(daemon) + arguments)
            .redirectInput(ProcessBuilder.Redirect.from(File("/dev/null")))
            .redirectOutput(ProcessBuilder.Redirect.to(File("/dev/null")))
            .redirectError(ProcessBuilder.Redirect.to(File("/dev/null")))
            .start()
        if (!process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            process.waitFor(1, TimeUnit.SECONDS)
            Log.e(TAG, "ksud command timed out: ${arguments.joinToString(" ")}")
            return false
        }
        return process.exitValue() == 0
    }

    override fun onBind(intent: Intent): IBinder = Stub()

    @SuppressLint("PrivateApi")
    private fun getInstalledPackagesAsUser(userId: Int): List<PackageInfo> {
        return try {
            queryInstalledPackagesAsUser(userId)
        } catch (e: Throwable) {
            Log.e(TAG, "getInstalledPackagesAsUser", e)
            emptyList()
        }
    }

    @SuppressLint("PrivateApi")
    private fun queryInstalledPackagesAsUser(userId: Int): List<PackageInfo> {
        val pm = packageManager
        val method = pm.javaClass.getDeclaredMethod(
            "getInstalledPackagesAsUser", Int::class.java, Int::class.java
        )
        @Suppress("UNCHECKED_CAST")
        return method.invoke(pm, 0, userId) as List<PackageInfo>
    }
}
