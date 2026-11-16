package org.rouyashiki.yukisu.update

import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import androidx.annotation.StringRes
import androidx.core.content.FileProvider
import androidx.core.content.pm.PackageInfoCompat
import androidx.core.net.toUri
import org.rouyashiki.yukisu.BuildConfig
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ksuApp
import org.rouyashiki.yukisu.ui.util.KsuCli
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import okhttp3.Call
import okhttp3.CacheControl
import okhttp3.Callback
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.Request
import okhttp3.Response
import org.bouncycastle.openpgp.PGPCompressedData
import org.bouncycastle.openpgp.PGPObjectFactory
import org.bouncycastle.openpgp.PGPPublicKey
import org.bouncycastle.openpgp.PGPPublicKeyRingCollection
import org.bouncycastle.openpgp.PGPSignature
import org.bouncycastle.openpgp.PGPSignatureList
import org.bouncycastle.openpgp.PGPUtil
import org.bouncycastle.openpgp.operator.bc.BcKeyFingerprintCalculator
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest
import java.util.Date
import java.util.concurrent.TimeUnit
import java.util.zip.ZipInputStream
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

data class CiRun(
    val runId: Long,
    val versionCode: Int,
    val commitSha: String,
    val commitMessage: String,
    val apkSha256: String? = null,
    val apkSize: Long? = null,
)

data class PreparedCiUpdate(
    val apk: ByteArray,
    val signature: ByteArray,
)

sealed interface CiInstallResult {
    data object RootInstalled : CiInstallResult
    data object SystemInstallerStarted : CiInstallResult
    data class SystemInstallerPermissionRequired(val apk: File) : CiInstallResult
}

object CiUpdateManager {
    private const val TAG = "CiUpdateManager"
    private const val REPOSITORY = "Rouyashiki/YukiSU"
    private const val PAGES_BASE_URL = "https://yukisuupdates.rouyashiki.org"
    private const val RUNS_API =
        "https://api.github.com/repos/$REPOSITORY/actions/workflows/build-manager.yml/runs" +
            "?branch=main&status=success&per_page=1&exclude_pull_requests=true"
    private const val PAGES_METADATA_URL = "$PAGES_BASE_URL/ci-update.json"
    private const val PAGES_METADATA_SIGNATURE_URL = "$PAGES_BASE_URL/ci-update.sig"
    private const val NIGHTLY_METADATA_URL =
        "https://nightly.link/$REPOSITORY/workflows/build-manager/main/" +
            "Manager-update-metadata.zip"
    private const val PAGES_ARCHIVE_URL =
        "$PAGES_BASE_URL/Manager-arm64-v8a.zip"
    private const val NIGHTLY_ARCHIVE_URL =
        "https://nightly.link/$REPOSITORY/workflows/build-manager/main/Manager-arm64-v8a.zip"
    private const val APK_NAME = "app-release.apk"
    private const val SIGNATURE_NAME = "app-release.sig"
    private const val UPDATE_CACHE_DIR = "ci-update"
    private const val LEGACY_UPDATE_GLOB = "/data/local/tmp/yukisu-ci-update-*.apk"
    private const val METADATA_NAME = "ci-update.json"
    private const val METADATA_SIGNATURE_NAME = "ci-update.sig"
    private const val CI_RUN_ID_META_DATA = "org.rouyashiki.yukisu.CI_RUN_ID"
    private const val PRIMARY_KEY_FINGERPRINT = "71B2B58C2A543472BE0DA0D8F580A2CEEF67DC98"
    // Extend this set only when a new CI signing subkey is intentionally approved.
    private val ALLOWED_SIGNING_SUBKEY_FINGERPRINTS = setOf(
        "C09CE484EEA3F2D88E9CDCC8EBDB0D663D7AB2F6",
    )

    private const val MAX_ARCHIVE_BYTES = 32L * 1024 * 1024
    private const val MAX_APK_BYTES = 32L * 1024 * 1024
    private const val MAX_SIGNATURE_BYTES = 256L * 1024
    private const val MAX_METADATA_ARCHIVE_BYTES = 1024L * 1024
    private const val MAX_METADATA_BYTES = 256L * 1024
    private const val METADATA_SOURCE_TIMEOUT_MS = 5_000L
    private const val METADATA_CHECK_DEDUPLICATION_MS = 5_000L
    private const val CI_MANAGER_VERSION_CODE_BASE = 10_000 - 3_135
    private val metadataCheckMutex = Mutex()
    private var lastMetadataCheckAt = 0L
    private var lastMetadataCheck: Result<CiRun?>? = null

    private val metadataClient by lazy {
        ksuApp.okhttpClient.newBuilder()
            .connectTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .readTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .writeTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .callTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .build()
    }

    suspend fun latestSuccessfulMainRun(force: Boolean = false): CiRun? = metadataCheckMutex.withLock {
        val now = SystemClock.elapsedRealtime()
        val previous = lastMetadataCheck
        if (!force && previous != null && now - lastMetadataCheckAt < METADATA_CHECK_DEDUPLICATION_MS) {
            return previous.getOrThrow()
        }

        val result = try {
            Result.success(loadLatestSuccessfulMainRun())
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            Result.failure(error)
        }
        lastMetadataCheck = result
        lastMetadataCheckAt = SystemClock.elapsedRealtime()
        result.getOrThrow()
    }

    private suspend fun loadLatestSuccessfulMainRun(): CiRun? = withContext(Dispatchers.IO) {
        val sources: List<Pair<String, suspend () -> CiRun?>> = listOf(
            "Cloudflare Pages" to { latestCloudflareSignedMainRun() },
            "nightly.link" to { latestNightlySignedMainRun() },
            "GitHub API" to { latestSuccessfulMainRunFromApi() },
        )
        val failures = mutableListOf<Throwable>()
        for ((name, source) in sources) {
            try {
                return@withContext withTimeout(METADATA_SOURCE_TIMEOUT_MS) { source() }
            } catch (timeout: TimeoutCancellationException) {
                val error = IOException(
                    message(R.string.ci_update_error_metadata_timeout, name),
                    timeout,
                )
                Log.w(TAG, error.message, error)
                failures += error
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.w(TAG, "$name CI metadata source failed", error)
                failures += error
            }
        }

        throw IOException(message(R.string.ci_update_error_metadata_sources_failed)).apply {
            failures.forEach(::addSuppressed)
        }
    }

    private suspend fun latestCloudflareSignedMainRun(): CiRun = coroutineScope {
        val updateDir = metadataCacheDir("ci-update-metadata-pages")
        val metadata = File(updateDir, METADATA_NAME)
        val signature = File(updateDir, METADATA_SIGNATURE_NAME)
        metadata.delete()
        signature.delete()

        try {
            awaitAll(
                async {
                    downloadMetadataFile(
                        url = PAGES_METADATA_URL,
                        target = metadata,
                        maxBytes = MAX_METADATA_BYTES,
                    )
                },
                async {
                    downloadMetadataFile(
                        url = PAGES_METADATA_SIGNATURE_URL,
                        target = signature,
                        maxBytes = MAX_SIGNATURE_BYTES,
                    )
                },
            )
            verifyAndParseSignedMetadata(metadata, signature)
        } finally {
            metadata.delete()
            signature.delete()
        }
    }

    private suspend fun latestNightlySignedMainRun(): CiRun {
        val updateDir = metadataCacheDir("ci-update-metadata-nightly")
        val archive = File(updateDir, "Manager-update-metadata.zip")
        val metadata = File(updateDir, METADATA_NAME)
        val signature = File(updateDir, METADATA_SIGNATURE_NAME)
        archive.delete()
        metadata.delete()
        signature.delete()

        try {
            val request = Request.Builder()
                .url(NIGHTLY_METADATA_URL)
                .cacheControl(CacheControl.FORCE_NETWORK)
                .build()
            archive.writeBytes(requestMetadata(request, MAX_METADATA_ARCHIVE_BYTES).body)
            extractExpectedFiles(
                archive = archive,
                expected = mapOf(
                    METADATA_NAME to (metadata to MAX_METADATA_BYTES),
                    METADATA_SIGNATURE_NAME to (signature to MAX_SIGNATURE_BYTES),
                ),
            )
            return verifyAndParseSignedMetadata(metadata, signature)
        } finally {
            archive.delete()
            metadata.delete()
            signature.delete()
        }
    }

    private suspend fun latestSuccessfulMainRunFromApi(): CiRun? {
        val request = Request.Builder()
            .url(RUNS_API)
            .cacheControl(CacheControl.FORCE_NETWORK)
            .header("Accept", "application/vnd.github+json")
            .header("X-GitHub-Api-Version", "2022-11-28")
            .build()

        val response = requestMetadata(request, MAX_METADATA_ARCHIVE_BYTES)
        val run = parseLatestSuccessfulMainRun(response.body.decodeToString()) ?: return null
        return CiRun(
            runId = run.runId,
            versionCode = requestManagerVersionCode(run.commitSha),
            commitSha = run.commitSha,
            commitMessage = run.commitMessage,
        )
    }

    private fun metadataCacheDir(name: String): File = File(ksuApp.cacheDir, name).apply {
        check(isDirectory || mkdirs()) {
            message(R.string.ci_update_error_metadata_cache)
        }
    }

    private suspend fun downloadMetadataFile(
        url: String,
        target: File,
        maxBytes: Long,
    ) {
        val request = Request.Builder()
            .url(url)
            .cacheControl(CacheControl.FORCE_NETWORK)
            .build()
        target.writeBytes(requestMetadata(request, maxBytes).body)
    }

    private suspend fun requestMetadata(
        request: Request,
        maxBytes: Long,
    ): MetadataHttpResponse = suspendCancellableCoroutine { continuation ->
        val call = metadataClient.newCall(request)
        continuation.invokeOnCancellation { call.cancel() }
        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                if (continuation.isActive) continuation.resumeWithException(e)
            }

            override fun onResponse(call: Call, response: Response) {
                response.use {
                    try {
                        check(response.isSuccessful) {
                            message(
                                R.string.ci_update_error_http,
                                request.url.host,
                                response.code,
                            )
                        }
                        val body = response.body
                        val contentLength = body.contentLength()
                        check(contentLength < 0 || contentLength <= maxBytes) {
                            message(
                                R.string.ci_update_error_response_too_large,
                                request.url.host,
                            )
                        }
                        val output = ByteArrayOutputStream()
                        body.byteStream().use { input ->
                            copyWithLimit(input, output, maxBytes)
                        }
                        if (continuation.isActive) {
                            continuation.resume(
                                MetadataHttpResponse(
                                    body = output.toByteArray(),
                                    linkHeader = response.header("Link"),
                                )
                            )
                        }
                    } catch (error: Exception) {
                        if (continuation.isActive) continuation.resumeWithException(error)
                    }
                }
            }
        })
    }

    private fun verifyAndParseSignedMetadata(metadata: File, signature: File): CiRun {
        verifyDetachedSignature(ksuApp, metadata, signature)
        return parseSignedCiMetadata(metadata.readText())
    }

    private suspend fun requestManagerVersionCode(commitSha: String): Int {
        val request = Request.Builder()
            .url("https://api.github.com/repos/$REPOSITORY/commits?sha=$commitSha&per_page=1")
            .cacheControl(CacheControl.FORCE_NETWORK)
            .header("Accept", "application/vnd.github+json")
            .header("X-GitHub-Api-Version", "2022-11-28")
            .build()
        val response = requestMetadata(request, MAX_METADATA_BYTES)
        check(JSONArray(response.body.decodeToString()).length() > 0) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        val commitCount = response.linkHeader
            ?.split(',')
            ?.asSequence()
            ?.map(String::trim)
            ?.firstOrNull { it.endsWith("rel=\"last\"") }
            ?.substringAfter('<')
            ?.substringBefore('>')
            ?.toHttpUrlOrNull()
            ?.queryParameter("page")
            ?.toIntOrNull()
            ?: 1
        return CI_MANAGER_VERSION_CODE_BASE + commitCount
    }

    private fun parseLatestSuccessfulMainRun(body: String): GitHubCiRun? {
        val runs = JSONObject(body).getJSONArray("workflow_runs")
        if (runs.length() == 0) return null

        val run = runs.getJSONObject(0)
        check(run.optString("head_branch") == "main") {
            message(R.string.ci_update_error_metadata_invalid)
        }
        check(run.optString("event") in setOf("push", "workflow_dispatch")) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        check(run.optString("conclusion") == "success") {
            message(R.string.ci_update_error_metadata_invalid)
        }
        val commitSha = run.optString("head_sha")
        val commitMessage = run.optJSONObject("head_commit")
            ?.optString("message")
            ?.takeIf { it.isNotBlank() }
            ?: run.optString("display_title")
        return GitHubCiRun(
            runId = run.getLong("id"),
            commitSha = commitSha,
            commitMessage = commitMessage,
        )
    }

    private fun parseSignedCiMetadata(body: String): CiRun {
        val metadata = JSONObject(body)
        check(metadata.optInt("schema_version") == 1) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        check(metadata.optString("repository") == REPOSITORY) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        check(metadata.optString("workflow") == "build-manager.yml") {
            message(R.string.ci_update_error_metadata_invalid)
        }
        check(metadata.optString("branch") == "main") {
            message(R.string.ci_update_error_metadata_invalid)
        }
        val runId = metadata.optLong("run_id", -1L)
        check(runId > 0L) { message(R.string.ci_update_error_metadata_invalid) }
        val versionCode = metadata.optInt("version_code", -1)
        check(versionCode > 0) { message(R.string.ci_update_error_metadata_invalid) }
        val commitSha = metadata.optString("commit_sha")
        check(commitSha.matches(Regex("[0-9a-fA-F]{40}"))) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        val apkSha256 = metadata.optString("apk_sha256")
            .takeIf(String::isNotBlank)
            ?.also {
                check(it.matches(Regex("[0-9a-fA-F]{64}"))) {
                    message(R.string.ci_update_error_metadata_invalid)
                }
            }
        val apkSize = metadata.optLong("apk_size", -1L)
            .takeIf { it >= 0L }
            ?.also {
                check(it in 1..MAX_APK_BYTES) {
                    message(R.string.ci_update_error_metadata_invalid)
                }
            }
        check((apkSha256 == null) == (apkSize == null)) {
            message(R.string.ci_update_error_metadata_invalid)
        }
        return CiRun(
            runId = runId,
            versionCode = versionCode,
            commitSha = commitSha,
            commitMessage = metadata.optString("commit_message"),
            apkSha256 = apkSha256,
            apkSize = apkSize,
        )
    }

    private data class MetadataHttpResponse(
        val body: ByteArray,
        val linkHeader: String?,
    )

    private data class GitHubCiRun(
        val runId: Long,
        val commitSha: String,
        val commitMessage: String,
    )

    suspend fun downloadAndExtract(
        onProgress: (Int) -> Unit,
    ): PreparedCiUpdate = withContext(Dispatchers.IO) {
        val sources = listOf(
            "Cloudflare Pages" to PAGES_ARCHIVE_URL,
            "nightly.link" to NIGHTLY_ARCHIVE_URL,
        )
        val failures = mutableListOf<Throwable>()

        for ((name, url) in sources) {
            withContext(Dispatchers.Main) { onProgress(0) }
            try {
                val archive = downloadUpdateArchive(name, url, onProgress)
                val extracted = extractExpectedBytes(
                    archive = archive,
                    expected = mapOf(
                        APK_NAME to MAX_APK_BYTES,
                        SIGNATURE_NAME to MAX_SIGNATURE_BYTES,
                    )
                )
                return@withContext PreparedCiUpdate(
                    apk = checkNotNull(extracted[APK_NAME]) {
                        message(R.string.ci_update_error_artifact_invalid)
                    },
                    signature = checkNotNull(extracted[SIGNATURE_NAME]) {
                        message(R.string.ci_update_error_artifact_invalid)
                    },
                )
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                val failure = IOException(
                    message(R.string.ci_update_error_artifact_source_failed, name),
                    error,
                )
                Log.w(TAG, failure.message, failure)
                failures += failure
            }
        }

        throw IOException(message(R.string.ci_update_error_artifact_sources_failed)).apply {
            failures.forEach(::addSuppressed)
        }
    }

    private suspend fun downloadUpdateArchive(
        sourceName: String,
        url: String,
        onProgress: (Int) -> Unit,
    ): ByteArray {
        val request = Request.Builder().url(url).cacheControl(CacheControl.FORCE_NETWORK).build()
        return ksuApp.okhttpClient.newCall(request).execute().use { response ->
            check(response.isSuccessful) {
                message(R.string.ci_update_error_http, sourceName, response.code)
            }
            val body = response.body
            val contentLength = body.contentLength()
            check(contentLength < 0 || contentLength <= MAX_ARCHIVE_BYTES) {
                message(R.string.ci_update_error_response_too_large, sourceName)
            }
            val archive = ByteArrayOutputStream(
                contentLength.coerceIn(0L, MAX_ARCHIVE_BYTES).toInt()
            )
            body.byteStream().use { input ->
                copyDownloadWithProgress(
                    input = input,
                    output = archive,
                    contentLength = contentLength,
                    onProgress = onProgress,
                )
            }
            archive.toByteArray()
        }
    }

    fun verify(context: Context, run: CiRun, update: PreparedCiUpdate) {
        check(update.apk.size.toLong() in 1..MAX_APK_BYTES) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }
        try {
            verifyDetachedSignature(context, update.apk, update.signature)
        } catch (error: Exception) {
            throw IllegalStateException(
                context.getString(R.string.ci_update_error_signature_invalid),
                error,
            )
        }
        run.apkSize?.let { expectedSize ->
            check(update.apk.size.toLong() == expectedSize) {
                context.getString(R.string.ci_update_error_apk_invalid)
            }
        }
        run.apkSha256?.let { expectedDigest ->
            val digest = MessageDigest.getInstance("SHA-256").digest(update.apk).toHex()
            check(digest.equals(expectedDigest, ignoreCase = true)) {
                context.getString(R.string.ci_update_error_apk_invalid)
            }
        }
    }

    suspend fun install(
        context: Context,
        run: CiRun,
        update: PreparedCiUpdate,
    ): CiInstallResult = withContext(Dispatchers.IO) {
        if (KsuCli.SHELL.isRoot && run.apkSha256 != null && run.apkSize != null) {
            try {
                installWithRootService(context, update.apk)
                cleanupCachedUpdate(context)
                return@withContext CiInstallResult.RootInstalled
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.w(TAG, "Root CI update install failed; falling back to system installer", error)
            }
        }

        val apk = materializeSystemInstallerApk(context, update.apk)
        try {
            verifyApk(context, run, apk)
        } catch (error: Exception) {
            apk.delete()
            throw error
        }
        withContext(Dispatchers.Main) {
            startSystemInstallerOrRequestPermission(context, apk)
        }
    }

    fun canRequestPackageInstalls(context: Context): Boolean =
        context.packageManager.canRequestPackageInstalls()

    fun unknownSourcesPermissionIntent(context: Context): Intent =
        Intent(
            Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
            "package:${context.packageName}".toUri(),
        )

    fun startSystemInstaller(context: Context, apk: File) {
        check(apk.isFile) {
            context.getString(R.string.ci_update_error_apk_not_available)
        }
        val uri = FileProvider.getUriForFile(
            context,
            "${context.packageName}.fileprovider",
            apk,
        )
        val intent = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(intent)
    }

    /**
     * Removes an update left behind when installing this package terminated the
     * old app process before [install] could run its normal cleanup.
     */
    fun cleanupCachedUpdate(context: Context) {
        val updateDir = File(context.cacheDir, UPDATE_CACHE_DIR)
        if (updateDir.exists() && !updateDir.deleteRecursively()) {
            Log.w(TAG, "Failed to remove stale CI update cache at $updateDir")
        }
    }

    /**
     * Removes APKs left by manager versions that staged root installs in the
     * globally accessible shell temporary directory.
     */
    fun cleanupLegacyTemporaryUpdates() {
        if (!KsuCli.SHELL.isRoot) return
        val result = KsuCli.SHELL.newJob().add("rm -f $LEGACY_UPDATE_GLOB").exec()
        if (!result.isSuccess) {
            Log.w(TAG, "Failed to remove legacy CI update APKs")
        }
    }

    private suspend fun installWithRootService(context: Context, apk: ByteArray) {
        var sessionId: Int? = null
        var committed = false
        RootCiUpdateInstaller.connect(context).use { installer ->
            try {
                val id = installer.createSession(apk.size.toLong())
                sessionId = id
                installer.writeSession(id, apk.size.toLong()) { output ->
                    ByteArrayInputStream(apk).use { input -> input.copyTo(output) }
                }
                installer.commitSession(id)
                committed = true
            } finally {
                if (!committed) {
                    sessionId?.let { id ->
                        runCatching { installer.abandonSession(id) }
                            .onFailure { Log.w(TAG, "Failed to abandon CI install session $id", it) }
                    }
                }
            }
        }
    }

    private fun materializeSystemInstallerApk(context: Context, contents: ByteArray): File {
        cleanupCachedUpdate(context)
        val updateDir = File(context.cacheDir, UPDATE_CACHE_DIR).apply {
            check(isDirectory || mkdirs()) {
                context.getString(R.string.ci_update_error_update_cache)
            }
        }
        return File(updateDir, APK_NAME).also { apk ->
            apk.outputStream().buffered().use { output -> output.write(contents) }
        }
    }

    private fun startSystemInstallerOrRequestPermission(
        context: Context,
        apk: File,
    ): CiInstallResult {
        check(apk.isFile) {
            context.getString(R.string.ci_update_error_apk_not_available)
        }
        return if (canRequestPackageInstalls(context)) {
            startSystemInstaller(context, apk)
            CiInstallResult.SystemInstallerStarted
        } else {
            CiInstallResult.SystemInstallerPermissionRequired(apk)
        }
    }

    private fun extractExpectedFiles(
        archive: File,
        expected: Map<String, Pair<File, Long>>,
    ) {
        val extracted = mutableSetOf<String>()
        ZipInputStream(BufferedInputStream(FileInputStream(archive))).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) {
                    zip.closeEntry()
                    continue
                }
                val normalizedName = entry.name.removePrefix("./")
                check(entry.name == normalizedName || entry.name == "./$normalizedName") {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                val target = expected[normalizedName]
                    ?: error(message(R.string.ci_update_error_artifact_invalid))
                check('/' !in normalizedName && '\\' !in normalizedName) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                check(extracted.add(normalizedName)) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                check(entry.size < 0 || entry.size <= target.second) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                target.first.outputStream().buffered().use { output ->
                    copyWithLimit(zip, output, target.second)
                }
                check(target.first.length() > 0) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                zip.closeEntry()
            }
        }
        check(extracted == expected.keys) {
            message(R.string.ci_update_error_artifact_invalid)
        }
    }

    private fun extractExpectedBytes(
        archive: ByteArray,
        expected: Map<String, Long>,
    ): Map<String, ByteArray> {
        val extracted = mutableMapOf<String, ByteArray>()
        ZipInputStream(BufferedInputStream(ByteArrayInputStream(archive))).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) {
                    zip.closeEntry()
                    continue
                }
                val normalizedName = entry.name.removePrefix("./")
                check(entry.name == normalizedName || entry.name == "./$normalizedName") {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                val limit = expected[normalizedName]
                    ?: error(message(R.string.ci_update_error_artifact_invalid))
                check('/' !in normalizedName && '\\' !in normalizedName) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                check(normalizedName !in extracted) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                check(entry.size < 0 || entry.size <= limit) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                val output = ByteArrayOutputStream(
                    entry.size.coerceIn(0L, limit).toInt()
                )
                copyWithLimit(zip, output, limit)
                val contents = output.toByteArray()
                check(contents.isNotEmpty()) {
                    message(R.string.ci_update_error_artifact_invalid)
                }
                extracted[normalizedName] = contents
                zip.closeEntry()
            }
        }
        check(extracted.keys == expected.keys) {
            message(R.string.ci_update_error_artifact_invalid)
        }
        return extracted
    }

    private fun verifyDetachedSignature(context: Context, apk: File, signatureFile: File) {
        apk.inputStream().buffered().use { data ->
            signatureFile.inputStream().buffered().use { signature ->
                verifyDetachedSignature(context, data, signature)
            }
        }
    }

    private fun verifyDetachedSignature(
        context: Context,
        data: ByteArray,
        signature: ByteArray,
    ) {
        ByteArrayInputStream(data).use { dataInput ->
            ByteArrayInputStream(signature).use { signatureInput ->
                verifyDetachedSignature(context, dataInput, signatureInput)
            }
        }
    }

    private fun verifyDetachedSignature(
        context: Context,
        data: InputStream,
        signatureInput: InputStream,
    ) {
        val keyRings = context.assets.open("ci-update-public-key.asc").use { keyInput ->
            PGPPublicKeyRingCollection(
                PGPUtil.getDecoderStream(keyInput),
                BcKeyFingerprintCalculator(),
            )
        }
        val signature = readDetachedSignature(signatureInput)
        val signingKey = keyRings.getPublicKey(signature.keyID)
            ?: error(context.getString(R.string.ci_update_error_signature_invalid))

        val owningRing = keyRings.keyRings.asSequence().firstOrNull { ring ->
            ring.getPublicKey(signingKey.keyID) != null
        } ?: error(context.getString(R.string.ci_update_error_signature_invalid))
        check(fingerprint(owningRing.publicKey) == PRIMARY_KEY_FINGERPRINT) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        check(!signingKey.isMasterKey) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        check(fingerprint(signingKey) in ALLOWED_SIGNING_SUBKEY_FINGERPRINTS) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        check(!signingKey.hasRevocation()) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        check(signature.signatureType == PGPSignature.BINARY_DOCUMENT) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        checkSignatureTime(context, signingKey, signature.creationTime)

        signature.init(
            Ed25519PgpContentVerifierProvider,
            signingKey,
        )
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        while (true) {
            val count = data.read(buffer)
            if (count < 0) break
            signature.update(buffer, 0, count)
        }
        check(signature.verify()) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
    }

    private fun readDetachedSignature(input: InputStream): PGPSignature {
        PGPUtil.getDecoderStream(input).use { decoded ->
            return findSignature(PGPObjectFactory(decoded, BcKeyFingerprintCalculator()))
                ?: error(message(R.string.ci_update_error_signature_invalid))
        }
    }

    private fun findSignature(factory: PGPObjectFactory): PGPSignature? {
        while (true) {
            when (val item = factory.nextObject() ?: return null) {
                is PGPSignatureList -> {
                    check(item.size() == 1) {
                        message(R.string.ci_update_error_signature_invalid)
                    }
                    return item[0]
                }
                is PGPCompressedData -> {
                    val nested = PGPObjectFactory(item.dataStream, BcKeyFingerprintCalculator())
                    findSignature(nested)?.let { return it }
                }
            }
        }
    }

    private fun checkSignatureTime(
        context: Context,
        key: PGPPublicKey,
        signatureTime: Date,
    ) {
        check(!signatureTime.before(key.creationTime)) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
        val validSeconds = key.validSeconds
        if (validSeconds > 0) {
            val expiresAt = key.creationTime.time + validSeconds * 1000L
            check(signatureTime.time <= expiresAt) {
                context.getString(R.string.ci_update_error_signature_invalid)
            }
        }
        check(signatureTime.time <= System.currentTimeMillis() + 10L * 60 * 1000) {
            context.getString(R.string.ci_update_error_signature_invalid)
        }
    }

    @Suppress("DEPRECATION")
    private fun verifyApk(context: Context, requestedRun: CiRun, apk: File) {
        val packageManager = context.packageManager
        val archiveInfo = getPackageInfo(packageManager, apk.absolutePath)
            ?: error(context.getString(R.string.ci_update_error_apk_invalid))
        check(archiveInfo.packageName == context.packageName) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }

        val currentInfo = getPackageInfo(packageManager, context.packageName)
            ?: error(context.getString(R.string.ci_update_error_apk_invalid))
        val newVersion = PackageInfoCompat.getLongVersionCode(archiveInfo)
        val currentVersion = PackageInfoCompat.getLongVersionCode(currentInfo)
        check(newVersion > currentVersion) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }
        check(newVersion >= requestedRun.versionCode) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }

        val apkRunId = archiveInfo.applicationInfo?.metaData
            ?.getString(CI_RUN_ID_META_DATA)
            ?.removePrefix("run-")
            ?.toLongOrNull()
            ?: error(context.getString(R.string.ci_update_error_apk_invalid))
        check(apkRunId >= requestedRun.runId && apkRunId > BuildConfig.CI_RUN_ID) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }

        val archiveSigners = currentSigners(archiveInfo)
        val installedSigners = currentSigners(currentInfo)
        check(archiveSigners.isNotEmpty()) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }
        check(archiveSigners == installedSigners) {
            context.getString(R.string.ci_update_error_apk_invalid)
        }
    }

    @Suppress("DEPRECATION")
    private fun getPackageInfo(packageManager: PackageManager, packageNameOrPath: String): PackageInfo? {
        val flags = PackageManager.GET_META_DATA or PackageManager.GET_SIGNING_CERTIFICATES
        return if (File(packageNameOrPath).isFile) {
            packageManager.getPackageArchiveInfo(packageNameOrPath, flags)
        } else {
            packageManager.getPackageInfo(packageNameOrPath, flags)
        }
    }

    @Suppress("DEPRECATION")
    private fun currentSigners(info: PackageInfo): Set<String> {
        val signatures = info.signingInfo?.apkContentsSigners.orEmpty()
        return signatures.mapTo(mutableSetOf()) { signature ->
            MessageDigest.getInstance("SHA-256").digest(signature.toByteArray()).toHex()
        }
    }

    private fun copyWithLimit(
        input: InputStream,
        output: java.io.OutputStream,
        limit: Long,
    ) {
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var total = 0L
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            check(total <= limit) {
                message(R.string.ci_update_error_response_too_large, "CI")
            }
            output.write(buffer, 0, count)
        }
    }

    private suspend fun copyDownloadWithProgress(
        input: InputStream,
        output: java.io.OutputStream,
        contentLength: Long,
        onProgress: (Int) -> Unit,
    ) {
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var total = 0L
        var lastProgress = -1
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            check(total <= MAX_ARCHIVE_BYTES) {
                message(R.string.ci_update_error_response_too_large, "CI")
            }
            output.write(buffer, 0, count)
            if (contentLength > 0) {
                val progress = ((total * 100L) / contentLength).toInt().coerceIn(0, 100)
                if (progress != lastProgress) {
                    lastProgress = progress
                    withContext(Dispatchers.Main) { onProgress(progress) }
                }
            }
        }
    }

    private fun fingerprint(key: PGPPublicKey): String = key.fingerprint.toHex()

    private fun ByteArray.toHex(): String = joinToString("") { "%02X".format(it) }

    private fun message(@StringRes id: Int, vararg args: Any): String =
        ksuApp.getString(id, *args)

}
