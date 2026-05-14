package com.anatdx.yukisu.ui.viewmodel

import org.junit.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class RecentlyInstalledAppsTest {
    private val now = 24 * RECENTLY_INSTALLED_WINDOW_MILLIS

    @Test
    fun includesNewInstallsUntilExactlyOneHourOld() {
        assertTrue(isRecentlyInstalled(now, now))
        assertTrue(isRecentlyInstalled(now - RECENTLY_INSTALLED_WINDOW_MILLIS + 1, now))
        assertFalse(isRecentlyInstalled(now - RECENTLY_INSTALLED_WINDOW_MILLIS, now))
        assertFalse(isRecentlyInstalled(now - RECENTLY_INSTALLED_WINDOW_MILLIS - 1, now))
    }

    @Test
    fun excludesUnknownAndFutureTimestamps() {
        assertFalse(isRecentlyInstalled(0, now))
        assertFalse(isRecentlyInstalled(-1, now))
        assertFalse(isRecentlyInstalled(now + 1, now))
        assertFalse(isRecentlyInstalled(Long.MAX_VALUE, now))
    }

    @Test
    fun cachedInstallTimesExpireAsTimeAdvances() {
        val installedAt = now - RECENTLY_INSTALLED_WINDOW_MILLIS + 1000
        assertTrue(isRecentlyInstalled(installedAt, now))
        assertFalse(isRecentlyInstalled(installedAt, now + 1000))
    }
}
