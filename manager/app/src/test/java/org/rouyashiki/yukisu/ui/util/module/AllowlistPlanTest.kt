package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.Natives
import kotlin.test.*

class AllowlistPlanTest {
    private val cert = "a".repeat(64)
    private val otherCert = "b".repeat(64)
    private fun app(name: String = "test.app", uid: Int = 10123, signers: List<String> = listOf(cert)) =
        BackupPackage(name, uid, currentSigners = signers, status = BackupPackage.OK)
    private fun entry(name: String = "test.app", uid: Int = 10123, rules: String = "") = AllowlistBackup.Entry(
        listOf(name), Natives.Profile(name, uid, allowSu = true, rules = rules), mapOf(name to listOf(cert))
    )
    private fun context(
        entries: List<AllowlistBackup.Entry> = listOf(entry()),
        installed: List<BackupPackage> = listOf(app()),
        before: List<Natives.Profile> = emptyList(),
        protected: Set<Int> = emptySet(),
        rules: Map<String, String> = emptyMap(),
        version: Int = 2
    ): AllowlistPlan.Context {
        val keys = installed.map { it.packageName } + before.map { it.name } + entries.map { it.profile.name }
        return AllowlistPlan.Context(
            AllowlistBackup.Document(true, entries, version), AllowlistPlan.Inventory(installed),
            before.associateBy { it.currentUid }, keys.associateWith { rules[it].orEmpty() }, protected
        )
    }

    @Test fun remapsCurrentUidWithoutChangingRootCredentialsOrOldUidOwner() {
        val source = entry().let { it.copy(profile = it.profile.copy(rootUseDefault = false, uid = 1000, gid = 2000, groups = listOf(3003))) }
        val plan = AllowlistPlan.create(context(listOf(source), listOf(app(uid = 10456), app("test.reused", 10123))))
        val target = plan.profiles.getValue(10456)
        assertEquals(1000, target.uid)
        assertEquals(2000, target.gid)
        assertEquals(listOf(3003), target.groups)
        assertFalse(10123 in plan.profiles)
        assertEquals(1, plan.migratedCount)
    }

    @Test fun missingEntryDoesNotBlockOtherEntriesOrResetExistingProfiles() {
        val extra = Natives.Profile("test.extra", 10456, allowSu = true)
        val plan = AllowlistPlan.create(context(
            listOf(entry(), entry("test.missing", 10555)), listOf(app(), app(extra.name, extra.currentUid)), listOf(extra)
        ))
        assertEquals(1, plan.selectedCount)
        assertEquals(AllowlistPlan.Status.MISSING, plan.rows[1].status)
        assertEquals(setOf(10123), plan.profiles.keys)
        assertFalse(plan.replaceAvailable)
        assertFalse(9999 in plan.profiles)
    }

    @Test fun sameNameWithDifferentSignerAndUnreadableIdentityAreDistinct() {
        assertEquals(AllowlistPlan.Status.SIGNATURE, AllowlistPlan.create(context(installed = listOf(app(signers = listOf(otherCert))))).rows.single().status)
        assertEquals(AllowlistPlan.Status.UNREADABLE, AllowlistPlan.create(context(installed = listOf(app().copy(status = BackupPackage.UNAVAILABLE)))).rows.single().status)
    }

    @Test fun rotationAcceptsOnlyForwardLineageAndMultipleSignersRequireExactSet() {
        assertTrue(app(signers = listOf(otherCert)).copy(signingHistory = listOf(cert, otherCert)).accepts(listOf(cert)))
        assertFalse(app().accepts(listOf(otherCert)))
        assertTrue(app(signers = listOf(cert, otherCert)).accepts(listOf(otherCert, cert)))
        assertFalse(app(signers = listOf(cert, otherCert)).accepts(listOf(cert)))
        assertFalse(app().copy(signingHistory = listOf(otherCert, cert)).accepts(listOf(cert, otherCert)))
    }

    @Test fun sharedUidMustRetainExactMembership() {
        val shared = entry().copy(packages = listOf("test.app", "test.shared"), signingCertificates = mapOf("test.app" to listOf(cert), "test.shared" to listOf(cert)))
        val intact = listOf(app(uid = 10500), app("test.shared", 10500))
        assertEquals(1, AllowlistPlan.create(context(listOf(shared), intact)).selectedCount)
        listOf(intact.take(1), intact + app("test.extra", 10500), listOf(app(), app("test.shared", 10500))).forEach {
            assertEquals(AllowlistPlan.Status.SHARED_UID, AllowlistPlan.create(context(listOf(shared), it)).rows.single().status)
        }
        assertEquals(AllowlistPlan.Status.SHARED_UID, AllowlistPlan.create(context(installed = intact)).rows.single().status)
    }

    @Test fun randomNameRequiresExplicitMappingEvenForOneCandidate() {
        val data = context(installed = listOf(app("test.random", 10500)))
        assertEquals(listOf("test.random"), data.installed.candidates(entry(), emptySet()).map { it.packageName })
        assertEquals(0, AllowlistPlan.create(data).selectedCount)
        val selected = AllowlistPlan.create(data, AllowlistPlan.Options(mappings = mapOf(10123 to 10500)))
        assertEquals(AllowlistPlan.Status.MAPPED, selected.rows.single().status)
        assertEquals("test.random", selected.profiles.getValue(10500).name)
    }

    @Test fun candidateSearchNeverTreatsCertificateAsUniqueAppIdentity() {
        val data = context(installed = listOf(app("test.random", 10500), app("test.unrelated", 10501)))
        assertEquals(2, data.installed.candidates(entry(), emptySet()).size)
        assertTrue(AllowlistPlan.create(data).profiles.isEmpty())
    }

    @Test fun candidateMappingRejectsSharedUidCrossUserAndProtectedTargets() {
        val options = AllowlistPlan.Options(mappings = mapOf(10123 to 10500))
        assertFalse(AllowlistPlan.create(context(installed = listOf(app("test.random", 10500), app("test.shared", 10500))), options).canApply)
        val otherUser = app("test.random", 1010500)
        assertFalse(AllowlistPlan.create(context(installed = listOf(otherUser)), options.copy(mappings = mapOf(10123 to otherUser.uid))).canApply)
        assertFalse(AllowlistPlan.create(context(installed = listOf(app("test.random", 10500)), protected = setOf(10500)), options).canApply)
    }

    @Test fun protectionUsesTargetAppIdInsteadOfHistoricalUid() {
        assertEquals(1, AllowlistPlan.create(context(installed = listOf(app(uid = 10500)), protected = setOf(10123))).selectedCount)
        assertEquals(0, AllowlistPlan.create(context(installed = listOf(app(uid = 10500)), protected = setOf(10500))).selectedCount)
        assertEquals(0, AllowlistPlan.create(context(listOf(entry(uid = 1010123)), listOf(app(uid = 1010500)), protected = setOf(10500))).selectedCount)
    }

    @Test fun samePackageInAnotherUserIsNotAnAutomaticMatch() {
        assertEquals(0, AllowlistPlan.create(context(installed = listOf(app(uid = 1010123)))).selectedCount)
        val data = context(listOf(entry(), entry(uid = 1010123)), listOf(app(uid = 10500), app(uid = 1010500)))
        assertEquals(setOf(10500, 1010500), AllowlistPlan.create(data).profiles.keys)
    }

    @Test fun v1CanMigrateUidButCannotMapRandomNames() {
        val source = entry().copy(signingCertificates = emptyMap())
        assertEquals(setOf(10500), AllowlistPlan.create(context(listOf(source), listOf(app(uid = 10500)), version = 1)).profiles.keys)
        val random = context(listOf(source), listOf(app("test.random", 10500)), version = 1)
        assertTrue(random.installed.candidates(source, emptySet()).isEmpty())
        assertFalse(AllowlistPlan.create(random, AllowlistPlan.Options(mappings = mapOf(10123 to 10500))).canApply)
    }

    @Test fun duplicateMappingBlocksAllCollidingRowsUntilOneIsDeselected() {
        val data = context(listOf(entry(), entry("test.old", 10500)), listOf(app()))
        val options = AllowlistPlan.Options(mappings = mapOf(10500 to 10123))
        val conflict = AllowlistPlan.create(data, options)
        assertTrue(conflict.rows.all { it.status == AllowlistPlan.Status.DUPLICATE })
        assertTrue(conflict.profiles.isEmpty())
        assertEquals(1, AllowlistPlan.create(data, options.copy(excluded = setOf(10500))).selectedCount)
    }

    @Test fun replacementAndGlobalDefaultRequireSeparateExplicitOptions() {
        val extra = Natives.Profile("test.extra", 10500, allowSu = true)
        val data = context(installed = listOf(app(), app(extra.name, extra.currentUid)), before = listOf(extra))
        assertTrue(AllowlistPlan.create(data).replaceAvailable)
        val full = AllowlistPlan.create(data, AllowlistPlan.Options(replace = true))
        assertEquals(setOf(10500), full.resetUids)
        assertFalse(full.profiles.getValue(10500).allowSu)
        assertFalse(9999 in full.profiles)
        val global = AllowlistPlan.create(data, AllowlistPlan.Options(restoreDefault = true))
        assertTrue(global.profiles.getValue(9999).umountModules)
        assertFalse(10500 in global.profiles)
    }

    @Test fun staleExistingProfileBlocksOnlyReplacement() {
        val stale = Natives.Profile("test.removed", 10500, allowSu = true)
        val data = context(before = listOf(stale))
        assertEquals(1, AllowlistPlan.create(data).selectedCount)
        assertFalse(AllowlistPlan.create(data).replaceAvailable)
        assertFalse(AllowlistPlan.create(data, AllowlistPlan.Options(replace = true, restoreDefault = true)).canApply)
    }

    @Test fun emptyBackupIsNoOpUnlessReplacementOrGlobalChangeWasExplicitlySelected() {
        val original = Natives.Profile("test.app", 10123, allowSu = true)
        val data = context(emptyList(), before = listOf(original), rules = mapOf("test.app" to "historical rules"))
        assertFalse(AllowlistPlan.create(data).canApply)
        val full = AllowlistPlan.create(data, AllowlistPlan.Options(replace = true))
        assertEquals(setOf(10123), full.resetUids)
        assertEquals("", full.rules["test.app"])
    }

    @Test fun ruleChangesCannotOverwriteUnselectedUsersOrProtectedPackages() {
        val otherUser = Natives.Profile("test.app", 1010123, allowSu = true)
        val data = context(listOf(entry(rules = "new")), listOf(app(), app(uid = otherUser.currentUid)), listOf(otherUser), rules = mapOf("test.app" to "old"))
        val blocked = AllowlistPlan.create(data)
        assertEquals(AllowlistPlan.Status.RULE_CONFLICT, blocked.rows.single().status)
        assertTrue(blocked.rules.isEmpty())
        val protected = context(listOf(entry(rules = "new")), listOf(app(), app(uid = 1010500)), protected = setOf(10500))
        assertFalse(AllowlistPlan.create(protected).canApply)
    }

    @Test fun mappedRuleConflictsAreDetectedWithoutClearingUnrelatedRules() {
        val entries = listOf(entry("test.old", 10123, "one"), entry("test.new", 1010500, "two"))
        val data = context(entries, listOf(app("test.new", 10500), app("test.new", 1010500)))
        val plan = AllowlistPlan.create(data, AllowlistPlan.Options(mappings = mapOf(10123 to 10500)))
        assertTrue(plan.rows.all { it.status == AllowlistPlan.Status.RULE_CONFLICT })
        assertTrue(plan.profiles.isEmpty())
        assertTrue(plan.rules.isEmpty())
    }

    @Test fun explicitReplacementCanResolveRulesBlockedByAnUnlistedOrdinaryProfile() {
        val otherUser = Natives.Profile("test.app", 1010123, allowSu = true)
        val data = context(listOf(entry(rules = "new")), listOf(app(), app(uid = otherUser.currentUid)), listOf(otherUser))
        val merge = AllowlistPlan.create(data)
        assertFalse(merge.canApply)
        assertTrue(merge.replaceAvailable)
        val replacement = AllowlistPlan.create(data, AllowlistPlan.Options(replace = true))
        assertTrue(replacement.canApply)
        assertEquals(setOf(otherUser.currentUid), replacement.resetUids)
        assertEquals("new", replacement.rules["test.app"])
    }

    @Test fun changesToPackagesProfilesRulesAndProtectionInvalidatePreview() {
        val data = context(listOf(entry(rules = "new")), rules = mapOf("test.app" to "old"))
        val plan = AllowlistPlan.create(data)
        assertTrue(plan.sameExecution(AllowlistPlan.create(data)))
        listOf(
            data.copy(installed = AllowlistPlan.Inventory(listOf(app().copy(lastUpdateTime = 1)))),
            data.copy(before = mapOf(10123 to Natives.Profile("test.app", 10123))),
            data.copy(oldRules = mapOf("test.app" to "changed")),
            data.copy(protected = setOf(12345))
        ).forEach { assertFalse(plan.sameExecution(AllowlistPlan.create(it))) }
    }

    @org.junit.Test(timeout = 15000) fun matchesTenThousandEntriesUsingPackageIndices() {
        val entries = (0 until 10000).map { entry("test.app$it", 10000 + it) }
        val installed = (0 until 10000).map { app("test.app$it", 30000 + it) }
        val plan = AllowlistPlan.create(context(entries, installed))
        assertEquals(10000, plan.selectedCount)
        assertEquals(10000, plan.migratedCount)
    }
}
