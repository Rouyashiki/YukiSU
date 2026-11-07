package com.anatdx.yukisu.ui.viewmodel

import org.junit.Test
import kotlin.test.assertEquals

class AppGroupSortingTest {
    private val manager = AppGroupSortKey("Manager", 0, 10L)
    private val shell = AppGroupSortKey("Shell", 1, 20L)
    private val root = AppGroupSortKey("Root", 2, 30L)
    private val custom = AppGroupSortKey("Custom", 3, 40L)
    private val app = AppGroupSortKey("App", 4, 50L)
    private val groups = listOf(manager, shell, root, custom, app)

    @Test
    fun installTimeIgnoresAllPermissionPrioritiesInBothDirections() {
        assertEquals(groups.reversed(), groups.sortedWith(appGroupComparator(SortType.INSTALL_TIME_NEW)))
        assertEquals(groups, groups.reversed().sortedWith(appGroupComparator(SortType.INSTALL_TIME_OLD)))
    }

    @Test
    fun nameSortingPreservesPinsAndSortsWithinEachPriority() {
        val zebra = app.copy(label = "Zebra")
        val input = listOf(zebra, app, root, manager)
        assertEquals(
            listOf(manager, root, app, zebra),
            input.sortedWith(appGroupComparator(SortType.NAME_ASC)),
        )
        assertEquals(
            listOf(manager, root, zebra, app),
            input.sortedWith(appGroupComparator(SortType.NAME_DESC)),
        )
    }

    @Test
    fun equalInstallTimesUseNamesInsteadOfPermissionPriority() {
        val input = listOf(manager, app.copy(latestInstallTime = manager.latestInstallTime))
        for (sort in listOf(SortType.INSTALL_TIME_NEW, SortType.INSTALL_TIME_OLD)) {
            assertEquals(listOf("App", "Manager"), input.sortedWith(appGroupComparator(sort)).map { it.label })
        }
    }

    @Test
    fun removedSortPreferencesFallBackToNameAscending() {
        for (key in listOf("SIZE_ASC", "SIZE_DESC", "USAGE_FREQ")) {
            assertEquals(SortType.NAME_ASC, SortType.fromPersistKey(key))
        }
        assertEquals(SortType.INSTALL_TIME_NEW, SortType.fromPersistKey("INSTALL_TIME_NEW"))
        assertEquals(SortType.INSTALL_TIME_OLD, SortType.fromPersistKey("INSTALL_TIME_OLD"))
    }
}
