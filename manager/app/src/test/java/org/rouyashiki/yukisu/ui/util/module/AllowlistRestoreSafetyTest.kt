package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.Natives
import kotlin.test.*

class AllowlistRestoreSafetyTest {
    private val cert = "a".repeat(64)
    private val original = Natives.Profile("test.app", 10123, allowSu = true)
    private fun plan(before: Natives.Profile = original, rule: String = "", extra: Natives.Profile? = null): AllowlistPlan.Plan {
        val target = Natives.Profile("test.app", 10123, rules = rule)
        val profiles = listOfNotNull(before, extra).associateBy { it.currentUid }
        val installed = listOfNotNull(target, extra).map {
            BackupPackage(it.name, it.currentUid, currentSigners = listOf(cert), status = BackupPackage.OK)
        }
        return AllowlistPlan.create(AllowlistPlan.Context(
            AllowlistBackup.Document(true, listOf(AllowlistBackup.Entry(listOf("test.app"), target, mapOf("test.app" to listOf(cert))))),
            AllowlistPlan.Inventory(installed), profiles,
            (profiles.values.map { it.name } + "test.app").associateWith { "old rules" }, emptySet()
        ))
    }

    private class Store(initial: List<Natives.Profile>) : AllowlistRestore.Store {
        val profiles = initial.associateBy { it.currentUid }.toMutableMap()
        val rules = initial.associate { it.name to "old rules" }.toMutableMap()
        val writes = mutableListOf<Natives.Profile>()
        val ruleWrites = mutableListOf<Pair<String, String>>()
        var identityValid = true
        var changeIdentityOnWrite = false
        var changeIdentityOnRuleWrite = false
        var failUid: Int? = null
        var beforeWrite: ((Natives.Profile) -> Unit)? = null
        override fun identitiesMatch(uids: Set<Int>) = identityValid
        override fun read(uid: Int) = profiles[uid]
        override fun write(profile: Natives.Profile): Boolean {
            writes += profile
            beforeWrite?.invoke(profile)
            if (profile.currentUid == failUid) return false
            profiles[profile.currentUid] = profile
            if (changeIdentityOnWrite) identityValid = false
            return true
        }
        override fun readRules(key: String) = rules[key].orEmpty()
        override fun writeRules(key: String, rules: String): Boolean {
            ruleWrites += key to rules
            this.rules[key] = rules
            if (changeIdentityOnRuleWrite) identityValid = false
            return true
        }
    }

    @Test fun changedIdentityBeforeExecutionProducesNoWrites() {
        val store = Store(listOf(original)).apply { identityValid = false }
        assertFails { AllowlistRestore.apply(plan(), store) }
        assertTrue(store.writes.isEmpty())
        assertTrue(store.ruleWrites.isEmpty())
    }

    @Test fun rollbackNeverRestoresOldRootGrantAfterUidIdentityChanges() {
        val store = Store(listOf(original)).apply { changeIdentityOnWrite = true }
        val failure = assertFailsWith<AllowlistRestore.Failure> { AllowlistRestore.apply(plan(rule = "old rules"), store) }
        assertTrue(failure.rollbackFailed)
        assertEquals(1, store.writes.size)
        assertFalse(store.writes.single().allowSu)
    }

    @Test fun identityChangeDuringRuleWriteStopsProfilesAndReportsIncompleteRollback() {
        val store = Store(listOf(original)).apply { changeIdentityOnRuleWrite = true }
        val failure = assertFailsWith<AllowlistRestore.Failure> { AllowlistRestore.apply(plan(rule = "new rules"), store) }
        assertTrue(failure.rollbackFailed)
        assertTrue(store.writes.isEmpty())
        assertEquals(listOf("test.app" to "new rules"), store.ruleWrites)
    }

    @Test fun staleProfileKeyIsNeverRegrantedByRollback() {
        val stale = original.copy(name = "test.removed")
        val store = Store(listOf(stale)).apply { failUid = 10123; rules["test.app"] = "old rules" }
        val failure = assertFailsWith<AllowlistRestore.Failure> { AllowlistRestore.apply(plan(stale, "old rules"), store) }
        assertTrue(failure.rollbackFailed)
        assertTrue(store.writes.none { it.name == stale.name && it.allowSu })
    }

    @Test fun mergeWritesNeverResetUnselectedProfilesOrClearTheirRules() {
        val extra = Natives.Profile("test.extra", 10500, allowSu = true)
        val store = Store(listOf(original, extra))
        AllowlistRestore.apply(plan(extra = extra), store)
        assertEquals(extra, store.profiles[10500])
        assertEquals("old rules", store.rules[extra.name])
        assertEquals(setOf(10123), store.writes.map { it.currentUid }.toSet())
        assertTrue(store.ruleWrites.all { it.first == "test.app" })
        assertFalse(9999 in store.profiles)
    }

    @Test fun concurrentProfileAndRuleEditsAreNotOverwritten() {
        val profileChanged = Store(listOf(original.copy(allowSu = false)))
        assertFails { AllowlistRestore.apply(plan(), profileChanged) }
        assertTrue(profileChanged.writes.isEmpty())
        val rulesChanged = Store(listOf(original)).apply { rules["test.app"] = "edited" }
        assertFails { AllowlistRestore.apply(plan(), rulesChanged) }
        assertTrue(rulesChanged.ruleWrites.isEmpty())
    }

    @Test fun rejectedReplacementCannotBeExecutedAsImplicitMerge() {
        val source = plan().context
        val invalid = AllowlistPlan.create(source, AllowlistPlan.Options(replace = true, excluded = setOf(10123)))
        val store = Store(listOf(original))
        assertFails { AllowlistRestore.apply(invalid, store) }
        assertTrue(store.writes.isEmpty())
    }

    @Test fun rollbackPreservesConcurrentProfileEdits() {
        val extra = Natives.Profile("test.extra", 10500, allowSu = true)
        val context = plan(extra = extra).context
        val entry = AllowlistBackup.Entry(
            listOf(extra.name), extra.copy(allowSu = false, rules = "old rules"), mapOf(extra.name to listOf(cert))
        )
        val restore = AllowlistPlan.create(context.copy(document = context.document.copy(apps = context.document.apps + entry)))
        val edited = original.copy(rootUseDefault = false, uid = 1000)
        val store = Store(listOf(original, extra)).apply {
            failUid = extra.currentUid
            beforeWrite = { if (it.currentUid == extra.currentUid) profiles[original.currentUid] = edited }
        }
        val failure = assertFailsWith<AllowlistRestore.Failure> { AllowlistRestore.apply(restore, store) }
        assertTrue(failure.rollbackFailed)
        assertEquals(edited, store.profiles[original.currentUid])
    }

    @Test fun rollbackPreservesConcurrentRuleEdits() {
        val store = Store(listOf(original)).apply {
            failUid = original.currentUid
            beforeWrite = { rules[original.name] = "concurrent rules" }
        }
        val failure = assertFailsWith<AllowlistRestore.Failure> { AllowlistRestore.apply(plan(rule = "restore rules"), store) }
        assertTrue(failure.rollbackFailed)
        assertEquals("concurrent rules", store.rules[original.name])
    }
}
