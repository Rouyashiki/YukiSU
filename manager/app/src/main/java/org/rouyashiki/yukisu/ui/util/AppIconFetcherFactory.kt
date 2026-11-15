package org.rouyashiki.yukisu.ui.util

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.os.Process
import android.os.UserHandle
import coil.ImageLoader
import coil.fetch.FetchResult
import coil.fetch.Fetcher
import coil.request.Options
import me.zhanghai.android.appiconloader.coil.AppIconFetcher

class AppIconFetcherFactory(iconSize: Int, context: Context) : Fetcher.Factory<PackageInfo> {
    private val delegate = AppIconFetcher.Factory(iconSize, false, context.applicationContext)

    override fun create(data: PackageInfo, options: Options, imageLoader: ImageLoader): Fetcher? {
        val info = data.applicationInfo ?: return null
        val fetcher = delegate.create(data, options, imageLoader) ?: return null
        return object : Fetcher {
            override suspend fun fetch(): FetchResult? = try {
                fetcher.fetch()
            } catch (error: SecurityException) {
                if (UserHandle.getUserHandleForUid(info.uid) == Process.myUserHandle()) {
                    throw error
                }
                // Badging an unrelated user requires privileges the manager process lacks.
                val fallback = ApplicationInfo(info).apply {
                    uid = Process.myUid() / 100000 * 100000 + info.uid % 100000
                }
                val fallbackPackage = PackageInfo().apply {
                    packageName = data.packageName
                    applicationInfo = fallback
                }
                val fallbackFetcher = delegate.create(fallbackPackage, options, imageLoader)
                    ?: throw error
                fallbackFetcher.fetch()
            }
        }
    }
}
