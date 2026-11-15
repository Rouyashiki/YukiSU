package org.rouyashiki.yukisu.ui.util

import org.rouyashiki.yukisu.Natives
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertSame
import kotlin.test.assertTrue

class ProfileSepolicyTest {
    private val packageName = "test.app"
    private val diagnostic = "No sepolicy profile for $packageName"

    @Test fun restoredCustomProfileKeepsAllSettingsExceptLegacyDiagnostic() {
        val restored = Natives.Profile(
            name = packageName, currentUid = 10123, allowSu = true,
            rootUseDefault = false, rootTemplate = "custom.template",
            uid = 1000, gid = 2000, groups = listOf(1000, 3003),
            capabilities = listOf(0, 1), capabilitiesPermitted = listOf(0),
            capabilitiesInheritable = listOf(1), context = "u:r:custom:s0",
            namespace = 2, flags = 1, rules = diagnostic
        )
        val editable = restored.withoutLegacySepolicyDiagnostic(packageName)

        assertEquals(restored.copy(rules = ""), editable)
        assertEquals(diagnostic, restored.rules)
        assertEquals("", editable.copy(gid = 1000).withoutLegacySepolicyDiagnostic(packageName).rules)
    }

    @Test fun existingRootGrantCanSwitchFromDefaultToCustomWithoutLegacyRules() {
        val restored = Natives.Profile(packageName, 10123, allowSu = true, rules = diagnostic)
        val editable = restored.withoutLegacySepolicyDiagnostic(packageName)
        val custom = editable.copy(rootUseDefault = false, uid = 1000, flags = 1)

        assertEquals(restored.copy(rootUseDefault = false, uid = 1000, flags = 1, rules = ""), custom)
    }

    @Test fun matchesLegacyDiagnosticWithLineEndingVariants() {
        for (rules in listOf(diagnostic, "$diagnostic\n", " \t$diagnostic\r\n")) {
            val restored = Natives.Profile(packageName, rules = rules)
            val editable = restored.withoutLegacySepolicyDiagnostic(packageName)
            assertEquals("", editable.rules)
            assertSame(editable, editable.withoutLegacySepolicyDiagnostic(packageName))
        }
    }

    @Test fun preservesRealRulesAndOtherInvalidText() {
        for (rules in listOf(
            "", " \n\t", "# custom rules\nallow ksu shell process signal\n",
            "No sepolicy profile for another.app", "$diagnostic.extra",
            "$diagnostic\nallow ksu shell process signal", "$diagnostic # comment",
            "invalid policy text"
        )) {
            val restored = Natives.Profile(packageName, rules = rules)
            assertSame(restored, restored.withoutLegacySepolicyDiagnostic(packageName))
        }
    }

    @Test fun matchesActualProfileKeyRatherThanRestoredName() {
        val restored = Natives.Profile("another.app", rules = diagnostic)
        assertEquals("", restored.withoutLegacySepolicyDiagnostic(packageName).rules)
        assertSame(restored, restored.withoutLegacySepolicyDiagnostic("another.app"))
    }

    @Test fun clearingExistingRulesPersistsAcrossReload() {
        val file = Files.createTempFile("profile-sepolicy-", ".txt")
        try {
            Files.writeString(file, "allow ksu shell process signal\n")
            val previousRules = Files.readString(file)
            val updated = Natives.Profile(packageName, allowSu = true, rootUseDefault = false, rules = "")

            assertTrue(saveProfileSepolicy(previousRules, updated) { rules ->
                Files.writeString(file, rules)
                true
            })
            assertEquals("", Files.readString(file))
        } finally {
            Files.deleteIfExists(file)
        }
    }

    @Test fun changingAnotherFieldAfterEmptyReadDoesNotEraseUnreadRules() {
        val unreadRules = "allow ksu shell process signal\n"
        var storedRules = unreadRules
        val updated = Natives.Profile(packageName, allowSu = true, rootUseDefault = false, uid = 1000)

        assertTrue(saveProfileSepolicy("", updated) { rules ->
            storedRules = rules
            true
        })
        assertEquals(unreadRules, storedRules)
    }

    @Test fun normalizedLegacyRulesDoNotEraseRawBackupContents() {
        var storedRules = diagnostic
        val previous = Natives.Profile(packageName, allowSu = true, rules = diagnostic)
            .withoutLegacySepolicyDiagnostic(packageName)
        val updated = previous.copy(rootUseDefault = false, gid = 1000)

        assertTrue(saveProfileSepolicy(previous.rules, updated) { rules ->
            storedRules = rules
            true
        })
        assertEquals(diagnostic, storedRules)
    }

    @Test fun failureToPersistClearedRulesIsReported() {
        var writeAttempted = false
        val updated = Natives.Profile(packageName, allowSu = true, rootUseDefault = false)

        assertFalse(saveProfileSepolicy("allow ksu shell process signal", updated) { rules ->
            writeAttempted = true
            assertEquals("", rules)
            false
        })
        assertTrue(writeAttempted)
    }

    @Test fun genuineRulesAreStillWrittenAndAppliedWhenUnchanged() {
        val rules = "# custom rules\nallow ksu shell process signal\n"
        val updated = Natives.Profile(packageName, allowSu = true, rootUseDefault = false, rules = rules)
        val writes = mutableListOf<String>()

        assertTrue(saveProfileSepolicy(rules, updated) { value ->
            writes += value
            true
        })
        assertEquals(listOf(rules), writes)
    }

    @Test fun defaultAndNonRootProfilesDoNotWriteRules() {
        val custom = Natives.Profile(packageName, allowSu = true, rootUseDefault = false)
        for (updated in listOf(custom.copy(rootUseDefault = true), custom.copy(allowSu = false))) {
            assertTrue(saveProfileSepolicy("allow ksu shell process signal", updated) {
                error("Unrelated profile changes must not write rules")
            })
        }
    }
}
