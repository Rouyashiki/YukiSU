package com.anatdx.yukisu.ui.viewmodel

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class RecentAppsTest {
    private val now = 24 * RECENT_APP_WINDOW_MILLIS

    private fun app(installedAt: Long, updatedAt: Long = 0L) =
        AppGroupSortKey("App", 4, installedAt, updatedAt)

    @Test
    fun includesOldAppsUpdatedWithinTheLastHour() {
        assertTrue(isRecentApp(app(now - 2 * RECENT_APP_WINDOW_MILLIS, now - 1000), now))
        assertFalse(isRecentApp(app(now - 2 * RECENT_APP_WINDOW_MILLIS, now - RECENT_APP_WINDOW_MILLIS), now))
    }

    @Test
    fun includesInstallsAndUpdatesUntilExactlyOneHourOld() {
        for (age in listOf(0L, RECENT_APP_WINDOW_MILLIS - 1)) {
            assertTrue(isRecentApp(app(now - age), now))
            assertTrue(isRecentApp(app(1L, now - age), now))
        }
        for (age in listOf(RECENT_APP_WINDOW_MILLIS, RECENT_APP_WINDOW_MILLIS + 1)) {
            assertFalse(isRecentApp(app(now - age), now))
            assertFalse(isRecentApp(app(1L, now - age), now))
        }
    }

    @Test
    fun usesTheNewerOfInstallationAndUpdateTimes() {
        assertEquals(now, app(now, now - 1000).latestInstallOrUpdateTime)
        assertEquals(now, app(now - 1000, now).latestInstallOrUpdateTime)
        assertTrue(isRecentApp(app(now, now - RECENT_APP_WINDOW_MILLIS), now))
    }

    @Test
    fun interleavesInstallsAndUpdatesByRecencyWithoutPermissionPins() {
        val installed = app(now - 1000).copy(label = "Installed")
        val updated = app(1L, now - 500).copy(label = "Updated")
        val manager = app(1L, now - 2000).copy(label = "Manager", priority = 0)
        assertEquals(
            listOf(updated, installed, manager),
            listOf(manager, installed, updated).sortedWith(recentAppComparator()),
        )
    }

    @Test
    fun installTimeSortStillUsesTheOriginalInstallationTime() {
        val installed = app(now - 1000)
        val updated = app(1L, now)
        assertEquals(
            listOf(installed, updated),
            listOf(updated, installed).sortedWith(appGroupComparator(SortType.INSTALL_TIME_NEW)),
        )
    }

    @Test
    fun excludesUnknownAndFutureTimestamps() {
        for (timestamp in listOf(0L, -1L, now + 1, Long.MAX_VALUE)) {
            assertFalse(isRecentApp(app(timestamp), now))
            assertFalse(isRecentApp(app(0L, timestamp), now))
        }
    }

    @Test
    fun expiresOneHourAfterTheMostRecentUpdate() {
        val updated = app(1L, now - RECENT_APP_WINDOW_MILLIS + 1000)
        assertTrue(isRecentApp(updated, now))
        assertFalse(isRecentApp(updated, now + 1000))
    }
}
