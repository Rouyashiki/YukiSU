package org.rouyashiki.yukisu.ui.util.module

enum class ModuleRuntimeKind(val wireName: String, val badgeLabel: String) {
    Zygisk("zygisk", "Zygisk"),
    Native("native", "ZN"),
}

internal fun parseModuleRuntimeKinds(
    runtimes: List<String>?,
    legacyRuntime: String,
): Set<ModuleRuntimeKind> {
    val names = runtimes?.toSet() ?: setOf(legacyRuntime)
    return ModuleRuntimeKind.entries.filterTo(linkedSetOf()) { it.wireName in names }
}
