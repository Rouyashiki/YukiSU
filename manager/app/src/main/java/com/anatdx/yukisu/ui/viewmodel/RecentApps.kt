package com.anatdx.yukisu.ui.viewmodel

internal const val RECENT_APP_WINDOW_MILLIS = 60 * 60 * 1000L

internal fun isRecentApp(sortKey: AppGroupSortKey, nowMillis: Long): Boolean {
    val changedAt = sortKey.latestInstallOrUpdateTime
    return changedAt > 0 && changedAt <= nowMillis &&
        nowMillis - changedAt < RECENT_APP_WINDOW_MILLIS
}

internal fun recentAppComparator(): Comparator<AppGroupSortKey> =
    compareByDescending<AppGroupSortKey> { it.latestInstallOrUpdateTime }
        .thenBy { it.label.lowercase() }
