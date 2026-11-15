package org.rouyashiki.yukisu.ui.util

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.FileProvider
import org.rouyashiki.yukisu.data.registry.ExpectedFile
import org.rouyashiki.yukisu.data.registry.RegistryDownloadStore
import org.rouyashiki.yukisu.data.registry.RegistryUrls
import org.rouyashiki.yukisu.ksuApp
import org.rouyashiki.yukisu.ui.util.module.LatestVersionInfo
import okhttp3.CacheControl
import okhttp3.Call
import okhttp3.Callback
import okhttp3.Request
import okhttp3.Response
import java.io.File
import java.io.IOException

private const val TAG = "DownloadUtil"
private val mainHandler by lazy { Handler(Looper.getMainLooper()) }

data class DownloadProgress(
    val downloadedBytes: Long = 0L,
    val totalBytes: Long? = null,
) {
    val fraction: Float?
        get() = totalBytes?.takeIf { it > 0L }
            ?.let { (downloadedBytes.toDouble() / it.toDouble()).toFloat().coerceIn(0f, 1f) }
}

fun interface DownloadHandle {
    fun cancel()
}

/**
 * @author weishu
 * @date 2023/6/22.
 */
fun download(
    context: Context,
    url: String,
    fileName: String,
    description: String,
    maxBytes: Long? = null,
    requireSecureTransport: Boolean = false,
    expected: ExpectedFile? = null,
    onDownloaded: (Uri) -> Unit = {},
    onProgress: (DownloadProgress) -> Unit = {},
    onError: (String) -> Unit = {}
): DownloadHandle {
    Log.d(TAG, "Start Download: $url")
    val appContext = context.applicationContext
    if (expected != null && !RegistryUrls.isAllowedReleaseDownloadUrl(url)) {
        deliverDownloadError(url, IOException("Registry download URL is not allowed"), onError)
        return DownloadHandle {}
    }
    val request = Request.Builder()
        .url(url)
        .cacheControl(CacheControl.Builder().noStore().build())
        .header("Accept", "application/zip, application/octet-stream")
        .header("Accept-Encoding", "identity")
        .build()
    val client = if (expected == null) ksuApp.okhttpClient else ksuApp.okhttpClient.newBuilder()
        .addNetworkInterceptor { chain ->
            if (!RegistryUrls.isAllowedDownloadHop(chain.request().url.toString())) {
                throw IOException("Registry download redirected to a blocked URL")
            }
            chain.proceed(chain.request())
        }.build()
    val call = client.newCall(request)

    call.enqueue(object : Callback {
        override fun onFailure(call: Call, e: IOException) {
            if (!call.isCanceled()) deliverDownloadError(url, e, onError)
        }

        override fun onResponse(call: Call, response: Response) {
            var partialFile: File? = null
            try {
                response.use {
                    check(response.isSuccessful) { "HTTP ${response.code} for $url" }
                    val finalUrl = response.request.url
                    val loopbackHttp = finalUrl.scheme == "http" &&
                        finalUrl.host in setOf("127.0.0.1", "0.0.0.0", "::1", "[::1]")
                    check(!requireSecureTransport || finalUrl.isHttps || loopbackHttp) {
                        "Download redirected to an insecure URL"
                    }
                    val body = checkNotNull(response.body) { "Empty response body for $url" }
                    if (expected != null) {
                        check(RegistryUrls.isAllowedDownloadHop(finalUrl.toString())) {
                            "Registry download redirected to a blocked URL"
                        }
                        val store = File(appContext.filesDir, RegistryDownloadStore.DIR_NAME)
                        RegistryDownloadStore.cleanup(store, 7L * 24 * 60 * 60 * 1000)
                        var lastPercent = -1
                        val target = body.byteStream().use { input ->
                            RegistryDownloadStore.download(
                                input, store, expected, "${java.util.UUID.randomUUID()}-$fileName",
                                onProgress = { bytes ->
                                    val percent = (bytes.toDouble() / expected.size * 100).toInt()
                                    if (percent != lastPercent) {
                                        lastPercent = percent
                                        mainHandler.post { onProgress(DownloadProgress(bytes, expected.size)) }
                                    }
                                },
                                checkActive = { if (call.isCanceled()) throw IOException("Download cancelled") },
                            )
                        }
                        mainHandler.post {
                            if (call.isCanceled()) RegistryDownloadStore.delete(store, target)
                            else onDownloaded(Uri.fromFile(target))
                        }
                        return
                    }
                    val downloadDir = File(appContext.cacheDir, "module_downloads")
                    check(downloadDir.isDirectory || downloadDir.mkdirs()) {
                        "Cannot create the module download cache"
                    }
                    val targetFile = File(downloadDir, safeDownloadFileName(fileName))
                    val temporaryFile = File(
                        downloadDir,
                        ".${targetFile.name}.${System.nanoTime()}.part",
                    )
                    partialFile = temporaryFile
                    val contentLength = body.contentLength().takeIf { it >= 0L }
                    check(maxBytes == null || contentLength == null || contentLength <= maxBytes) {
                        "Download exceeds the size limit"
                    }
                    body.byteStream().use { input ->
                        temporaryFile.outputStream().buffered().use { output ->
                            copyDownloadWithProgress(
                                input = input,
                                output = output,
                                contentLength = contentLength,
                                maxBytes = maxBytes,
                                onProgress = onProgress,
                            )
                        }
                    }
                    if (targetFile.exists()) {
                        check(targetFile.delete()) { "Cannot replace ${targetFile.name}" }
                    }
                    check(temporaryFile.renameTo(targetFile)) { "Cannot finalize ${targetFile.name}" }
                    partialFile = null

                    val uri = FileProvider.getUriForFile(
                        appContext,
                        "${appContext.packageName}.fileprovider",
                        targetFile,
                    )
                    Log.d(TAG, "Downloaded $description to ${targetFile.absolutePath}")
                    mainHandler.post { onDownloaded(uri) }
                }
            } catch (error: Exception) {
                if (!call.isCanceled()) deliverDownloadError(url, error, onError)
            } finally {
                partialFile?.delete()
            }
        }
    })
    return DownloadHandle(call::cancel)
}

internal fun safeDownloadFileName(fileName: String): String {
    val candidate = File(fileName).name
        .replace(Regex("[^A-Za-z0-9._() -]"), "_")
        .trim()
    return candidate.takeUnless { it.isEmpty() || it == "." || it == ".." } ?: "module.zip"
}

private fun deliverDownloadError(url: String, error: Exception, onError: (String) -> Unit) {
    Log.e(TAG, "Failed to download $url", error)
    val detail = error.message?.takeIf(String::isNotBlank) ?: error.javaClass.simpleName
    mainHandler.post { onError(detail) }
}

private fun copyDownloadWithProgress(
    input: java.io.InputStream,
    output: java.io.OutputStream,
    contentLength: Long?,
    maxBytes: Long?,
    onProgress: (DownloadProgress) -> Unit,
) {
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var downloaded = 0L
    var lastPercent = -1
    var lastUnknownLengthUpdate = 0L

    fun report(force: Boolean = false) {
        val percent = contentLength?.takeIf { it > 0L }
            ?.let { ((downloaded * 100L) / it).toInt().coerceIn(0, 100) }
        val now = System.nanoTime()
        val shouldReport = force || if (percent == null) {
            now - lastUnknownLengthUpdate >= 250_000_000L
        } else {
            percent != lastPercent
        }
        if (!shouldReport) return
        if (percent == null) lastUnknownLengthUpdate = now else lastPercent = percent
        val progress = DownloadProgress(downloaded, contentLength)
        mainHandler.post { onProgress(progress) }
    }

    report(force = true)
    while (true) {
        val count = input.read(buffer)
        if (count < 0) break
        check(maxBytes == null || downloaded + count <= maxBytes) {
            "Download exceeds the size limit"
        }
        output.write(buffer, 0, count)
        downloaded += count
        report()
    }
    report(force = true)
}

fun checkNewVersion(): LatestVersionInfo {
    val url = "https://api.github.com/repos/Anatdx/YukiSU/releases/latest"
    val defaultValue = LatestVersionInfo()
    return runCatching {
        val request = Request.Builder()
            .url(url)
            .build()

        ksuApp.okhttpClient.newCall(request).execute().use { response ->
            if (!response.isSuccessful) {
                Log.d("CheckUpdate", "Network request failed: ${response.message}")
                return defaultValue
            }
            val body = response.body.string()
            Log.d("CheckUpdate", "Return data: $body")
            val json = org.json.JSONObject(body)

            // 直接从 tag_name 提取版本号（如 v1.1）
            val tagName = json.optString("tag_name", "")
            val versionName = tagName.removePrefix("v") // 移除前缀 "v"

            // 从 body 字段获取更新日志（保留换行符）
            val changelog = json.optString("body")
                .replace("\\r\\n", "\n") // 转换换行符

            val assets = json.getJSONArray("assets")
            for (i in 0 until assets.length()) {
                val asset = assets.getJSONObject(i)
                val name = asset.getString("name")
                if (!name.endsWith(".apk")) continue

                val regex = Regex("YukiSU.*_(\\d+)-release")
                val matchResult = regex.find(name)
                if (matchResult == null) {
                    Log.d("CheckUpdate", "No matches found: $name, skip over")
                    continue
                }
                val versionCode = matchResult.groupValues[1].toInt()

                val downloadUrl = asset.getString("browser_download_url")
                return LatestVersionInfo(
                    versionCode,
                    downloadUrl,
                    changelog,
                    versionName
                )
            }
            Log.d("CheckUpdate", "No valid APK resource found, return default value")
            defaultValue
        }
    }.getOrDefault(defaultValue)
}
