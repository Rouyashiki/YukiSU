package org.rouyashiki.yukisu.ui.util.module

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class ModuleRuntimeKindTest {
    @Test
    fun mixedModuleRetainsBothTypesDespiteLegacyNativeValue() {
        val kinds = parseModuleRuntimeKinds(listOf("zygisk", "native"), "native")

        assertEquals(setOf(ModuleRuntimeKind.Zygisk, ModuleRuntimeKind.Native), kinds)
        assertEquals("Zygisk + ZN", kinds.joinToString(" + ") { it.badgeLabel })
    }

    @Test
    fun singleTypeModulesKeepTheirOwnBadge() {
        assertEquals(
            setOf(ModuleRuntimeKind.Zygisk),
            parseModuleRuntimeKinds(listOf("zygisk"), "zygisk"),
        )
        assertEquals(
            setOf(ModuleRuntimeKind.Native),
            parseModuleRuntimeKinds(listOf("native"), "native"),
        )
    }

    @Test
    fun oldDaemonsStillProvideSingleTypeBadges() {
        assertEquals(setOf(ModuleRuntimeKind.Zygisk), parseModuleRuntimeKinds(null, "zygisk"))
        assertEquals(setOf(ModuleRuntimeKind.Native), parseModuleRuntimeKinds(null, "native"))
        assertTrue(parseModuleRuntimeKinds(null, "").isEmpty())
        assertTrue(parseModuleRuntimeKinds(null, "unknown").isEmpty())
    }

    @Test
    fun explicitTypeListOverridesLegacyTypeIncludingAnEmptyList() {
        assertTrue(parseModuleRuntimeKinds(emptyList(), "native").isEmpty())
        assertEquals(
            setOf(ModuleRuntimeKind.Zygisk),
            parseModuleRuntimeKinds(listOf("zygisk"), "native"),
        )
    }

    @Test
    fun duplicatesUnknownTypesAndInputOrderDoNotChangeTheBadge() {
        val kinds = parseModuleRuntimeKinds(listOf("native", "unknown", "zygisk", "native"), "")

        assertEquals("Zygisk + ZN", kinds.joinToString(" + ") { it.badgeLabel })
        assertTrue(parseModuleRuntimeKinds(listOf("unknown"), "native").isEmpty())
    }
}
