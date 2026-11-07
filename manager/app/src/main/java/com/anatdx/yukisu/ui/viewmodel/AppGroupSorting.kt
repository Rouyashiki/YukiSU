package com.anatdx.yukisu.ui.viewmodel

internal data class AppGroupSortKey(
    val label: String,
    val priority: Int,
    val latestInstallTime: Long,
)

internal fun appGroupComparator(sortType: SortType): Comparator<AppGroupSortKey> {
    val byName = compareBy<AppGroupSortKey> { it.label.lowercase() }
    return when (sortType) {
        SortType.NAME_ASC -> compareBy<AppGroupSortKey> { it.priority }.then(byName)
        SortType.NAME_DESC -> compareBy<AppGroupSortKey> { it.priority }.then(byName.reversed())
        SortType.INSTALL_TIME_NEW -> compareByDescending<AppGroupSortKey> { it.latestInstallTime }.then(byName)
        SortType.INSTALL_TIME_OLD -> compareBy<AppGroupSortKey> { it.latestInstallTime }.then(byName)
    }
}
