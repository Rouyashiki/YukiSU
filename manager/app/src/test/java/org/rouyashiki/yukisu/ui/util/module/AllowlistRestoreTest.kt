package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.BackupPackage
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class AllowlistRestoreTest {
    private fun entry(profile: Natives.Profile) = AllowlistBackup.Entry(
        listOf(profile.name), profile, mapOf(profile.name to listOf("a".repeat(64)))
    )

    private fun apply(document: AllowlistBackup.Document, before: Map<Int, Natives.Profile>, protected: Set<Int>, store: Store) {
        val profiles = (before.values + document.apps.map { it.profile }).filter { it.currentUid != 9999 }.associateBy { it.currentUid }
        val installed = AllowlistPlan.Inventory(profiles.values.map {
            BackupPackage(it.name, it.currentUid, currentSigners = listOf("a".repeat(64)), status = BackupPackage.OK)
        })
        val keys = profiles.values.map { it.name }.toSet()
        val context = AllowlistPlan.Context(document, installed, before, keys.associateWith(store::readRules), protected)
        val plan = AllowlistPlan.create(context, AllowlistPlan.Options(replace = true, restoreDefault = true))
        AllowlistRestore.apply(plan, store)
    }

    private val original = Natives.Profile("test.app", 10123)
    private val target = original.copy(allowSu = true, rules = "new rules")
    private val document = AllowlistBackup.Document(true, listOf(entry(target)))

    private class Store(initial: List<Natives.Profile>) : AllowlistRestore.Store {
        val profiles = initial.associateBy { it.currentUid }.toMutableMap()
        val rules = mutableMapOf("test.app" to "old rules")
        var failOnce: Int? = null
        var silentlyIgnore: Int? = null
        var failRollback = false
        val writes = mutableListOf<Int>()
        override fun identitiesMatch(uids: Set<Int>) = true
        override fun read(uid: Int) = profiles[uid]
        override fun write(profile: Natives.Profile): Boolean {
            writes += profile.currentUid
            if (failOnce == profile.currentUid) { failOnce = null; return false }
            if (failRollback && profile.currentUid == 10123 && !profile.allowSu) return false
            if (silentlyIgnore != profile.currentUid) profiles[profile.currentUid] = profile
            return true
        }
        override fun readRules(key: String): String {
            require(key != AllowlistRestore.DEFAULT_KEY)
            return rules[key].orEmpty()
        }
        override fun writeRules(key: String, rules: String): Boolean { this.rules[key] = rules; return true }
    }

    @Test fun restoreUpdatesProfilesRulesAndDefaultLast() {
        val store = Store(listOf(original))
        apply(document, store.profiles.toMap(), emptySet(), store)
        assertEquals(target.copy(rules = ""), store.profiles[10123])
        assertEquals("new rules", store.rules["test.app"])
        val defaults = store.profiles.getValue(9999)
        assertTrue(defaults.umountModules)
        assertFalse(defaults.nonRootUseDefault)
        assertEquals(9999, store.writes.last())
    }

    @Test fun restoreOverwritesDifferentSettingsAndPreservesHistoricalRuleText() {
        val custom = target.copy(
            rootUseDefault = false, rootTemplate = "test.template", uid = 1000, gid = 1000,
            groups = listOf(0, 1000, 3003), capabilities = listOf(0, 1, 40),
            capabilitiesPermitted = listOf(0, 40), capabilitiesInheritable = listOf(1),
            context = "u:r:su:s0", namespace = 2, flags = 1,
            rules = "No sepolicy profile for test.app\n"
        )
        val allowed = Natives.Profile(
            "test.hide", 10124, allowSu = true, rootUseDefault = false,
            uid = 1000, gid = 1000, groups = listOf(3003), namespace = 1
        )
        val denied = Natives.Profile(
            "test.hide", 10124, nonRootUseDefault = false, umountModules = true
        )
        val defaults = Natives.Profile(
            AllowlistRestore.DEFAULT_KEY, AllowlistRestore.DEFAULT_UID,
            nonRootUseDefault = false, umountModules = true
        )
        val store = Store(listOf(original, allowed, defaults)).apply {
            rules["test.hide"] = "allow su app_data_file file read;"
        }
        val backup = AllowlistBackup.Document(false, listOf(custom, denied).map {
            entry(it)
        })
        val parsed = AllowlistBackup.parse(AllowlistBackup.encode(backup))
        AllowlistBackup.validateAgainst(
            parsed, mapOf(10123 to listOf("test.app"), 10124 to listOf("test.hide")), emptySet()
        )

        apply(parsed, store.profiles.toMap(), emptySet(), store)

        assertEquals(custom.copy(rules = ""), store.profiles[10123])
        assertEquals(denied, store.profiles[10124])
        assertEquals(defaults.copy(umountModules = false), store.profiles[9999])
        assertEquals(custom.rules, store.rules["test.app"])
        assertEquals("", store.rules["test.hide"])
    }

    @Test fun historicalCurrentRulesDoNotBlockReplacement() {
        val restored = target.copy(rules = "allow su app_data_file file read;")
        val backup = document.copy(apps = listOf(entry(restored)))
        val store = Store(listOf(original)).apply {
            rules["test.app"] = "No sepolicy profile for test.app\n"
        }
        AllowlistBackup.validateAgainst(backup, mapOf(10123 to listOf("test.app")), emptySet())

        apply(backup, store.profiles.toMap(), emptySet(), store)

        assertEquals(restored.copy(rules = ""), store.profiles[10123])
        assertEquals(restored.rules, store.rules["test.app"])
    }

    @Test fun failureRollsBackEarlierPermissionsAndRules() {
        val store = Store(listOf(original)).apply { failOnce = 9999 }
        val failure = assertFailsWith<AllowlistRestore.Failure> { apply(document, store.profiles.toMap(), emptySet(), store) }
        assertFalse(failure.rollbackFailed)
        assertEquals(original, store.profiles[10123])
        assertEquals("old rules", store.rules["test.app"])
        assertFalse(store.profiles[9999]?.umountModules == true)
    }

    @Test fun successfulWriteWithoutEffectIsDetected() {
        val store = Store(listOf(original)).apply { silentlyIgnore = 10123 }
        val failure = assertFailsWith<AllowlistRestore.Failure> { apply(document, store.profiles.toMap(), emptySet(), store) }
        assertFalse(failure.rollbackFailed)
        assertEquals("old rules", store.rules["test.app"])
    }

    @Test fun rollbackFailureIsReportedExplicitly() {
        val store = Store(listOf(original)).apply { failOnce = 9999; failRollback = true }
        val failure = assertFailsWith<AllowlistRestore.Failure> { apply(document, store.profiles.toMap(), emptySet(), store) }
        assertTrue(failure.rollbackFailed)
    }

    @Test fun newEntriesReturnToDefaultDeniedBehaviorOnFailure() {
        val store = Store(emptyList()).apply { failOnce = 9999 }
        assertFailsWith<AllowlistRestore.Failure> { apply(document, emptyMap(), emptySet(), store) }
        assertTrue(10123 in store.writes)
        assertFalse(store.profiles[10123]?.allowSu == true)
        assertEquals("old rules", store.rules["test.app"])
    }

    @Test fun restoresNewProfileAfterSnapshottingItsExistingRules() {
        val store = Store(emptyList())
        apply(document, emptyMap(), emptySet(), store)
        assertEquals(target.copy(rules = ""), store.profiles[10123])
        assertEquals("new rules", store.rules["test.app"])
    }

    @Test fun defaultUmountProfileDoesNotHavePackageRules() {
        val defaults = Natives.Profile(
            AllowlistRestore.DEFAULT_KEY, AllowlistRestore.DEFAULT_UID,
            nonRootUseDefault = false
        )
        val store = Store(listOf(original, defaults))
        apply(document, store.profiles.toMap(), emptySet(), store)
        assertEquals(defaults.copy(umountModules = true), store.profiles[9999])
    }

    @Test fun profilesAbsentFromBackupAreResetWithoutTouchingUnlistedManagers() {
        val extra = Natives.Profile("test.extra", 10124, allowSu = true)
        val manager = Natives.Profile("test.manager", 10125, allowSu = true)
        val store = Store(listOf(original, extra, manager))
        val before = mapOf(10123 to original, 10124 to extra)
        apply(document, before, emptySet(), store)
        assertFalse(store.profiles.getValue(10124).allowSu)
        assertTrue(store.profiles.getValue(10124).nonRootUseDefault)
        assertEquals(manager, store.profiles[10125])
        assertFalse(10125 in store.writes)
    }
}
