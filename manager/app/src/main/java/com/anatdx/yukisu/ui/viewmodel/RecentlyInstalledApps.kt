package com.anatdx.yukisu.ui.viewmodel

internal const val RECENTLY_INSTALLED_WINDOW_MILLIS = 60 * 60 * 1000L

internal fun isRecentlyInstalled(firstInstallTime: Long, nowMillis: Long): Boolean =
    firstInstallTime > 0 && firstInstallTime <= nowMillis &&
        nowMillis - firstInstallTime < RECENTLY_INSTALLED_WINDOW_MILLIS
