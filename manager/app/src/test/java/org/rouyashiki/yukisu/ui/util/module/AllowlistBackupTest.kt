package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.Natives
import com.google.gson.JsonParser
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFails
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class AllowlistBackupTest {
    private fun entry(uid: Int = 10123, name: String = "test.app") =
        AllowlistBackup.Entry(listOf(name), Natives.Profile(name, uid), mapOf(name to listOf("a".repeat(64))))
    private fun document(vararg entries: AllowlistBackup.Entry) = AllowlistBackup.Document(true, entries.toList())
    private fun json() = AllowlistBackup.encode(document(entry()))

    @Test fun roundTripRetainsExplicitDefaultDenyCustomRootAndGlobalUmount() {
        val custom = entry(10124, "test.root").let { it.copy(profile = it.profile.copy(
            allowSu = true, rootUseDefault = false, rootTemplate = "test.template", uid = 1000, gid = 1000,
            groups = listOf(0, 1000, 3003), capabilities = listOf(0, 1, 40),
            capabilitiesPermitted = listOf(0, 40), capabilitiesInheritable = listOf(1),
            context = "u:r:su:s0", namespace = 2, flags = 1,
            rules = "allow su app_data_file file read;"
        )) }
        val unmount = entry(10125, "test.hide").let { it.copy(profile = it.profile.copy(
            nonRootUseDefault = false, umountModules = true
        )) }
        val original = document(entry(), custom, unmount)
        assertEquals(original, AllowlistBackup.parse(AllowlistBackup.encode(original)))
    }

    @Test fun samePackageInTwoUsersIsValid() {
        val backup = document(entry(), entry(1010123))
        val parsed = AllowlistBackup.parse(AllowlistBackup.encode(backup))
        AllowlistBackup.validateAgainst(parsed, mapOf(10123 to listOf("test.app"), 1010123 to listOf("test.app")), emptySet())
        assertEquals(2, parsed.apps.size)
    }

    @Test fun sharedUidRequiresExactMembershipButAllowsExistingKeyToDiffer() {
        val backup = document(entry().copy(packages = listOf("test.app", "test.shared"), signingCertificates = mapOf("test.app" to listOf("a".repeat(64)), "test.shared" to listOf("a".repeat(64)))))
        AllowlistBackup.validateAgainst(backup, mapOf(10123 to listOf("test.shared", "test.app")), emptySet())
        assertFails { AllowlistBackup.validateAgainst(backup, mapOf(10123 to listOf("test.app")), emptySet()) }
        assertFails { AllowlistBackup.validateAgainst(backup, mapOf(10123 to listOf("test.app", "test.shared", "test.other")), emptySet()) }
    }

    @Test fun missingWrongUidAndManagerUidAreRejected() {
        val backup = document(entry())
        assertFails { AllowlistBackup.validateAgainst(backup, emptyMap(), emptySet()) }
        assertFails { AllowlistBackup.validateAgainst(backup, mapOf(10124 to listOf("test.app")), emptySet()) }
        assertFails { AllowlistBackup.validateAgainst(backup, mapOf(10123 to listOf("test.app")), setOf(10123)) }
        assertFails { AllowlistBackup.validateAgainst(document(entry(1010123)), mapOf(1010123 to listOf("test.app")), setOf(10123)) }
    }

    @Test fun malformedLegacyDuplicateAndNonJsonSyntaxAreRejected() {
        listOf("\u007fKSU", "{}", "[]", json() + "{}", json().replace("\"version\": 2", "\"version\": 2, \"version\": 2"),
            json().replace("\"version\": 2", "version: 2"), json().replace("\"format\":", "/*comment*/ \"format\":"))
            .forEach { assertFails(it.take(50)) { AllowlistBackup.parse(it) } }
    }

    @Test fun unknownMissingAndWrongTypedFieldsAreRejected() {
        listOf("dynamicManagers", "arbitrary").forEach { key ->
            val root = JsonParser.parseString(json()).asJsonObject
            root.addProperty(key, true)
            assertFails { AllowlistBackup.parse(root.toString()) }
        }
        val root = JsonParser.parseString(json()).asJsonObject
        root.remove("defaultUmountModules")
        assertFails { AllowlistBackup.parse(root.toString()) }
        assertFails { AllowlistBackup.parse(json().replace("\"allowSu\": false", "\"allowSu\": \"false\"")) }
        assertFails { AllowlistBackup.parse(json().replace("\"version\": 2", "\"version\": 3")) }
    }

    @Test fun unsafeNumbersStringsAndUnionParametersAreRejected() {
        listOf("2147483648", "-1", "1.0", "1e3", "18446744073709551617").forEach {
            assertFails { AllowlistBackup.parse(json().replace("\"appUid\": 10123", "\"appUid\": $it")) }
        }
        listOf("\"context\": \"${Natives.KERNEL_SU_DOMAIN}\"" to "\"context\": \"bad\\u0000context\"",
            "\"flags\": 0" to "\"flags\": 2", "\"rootUid\": 0" to "\"rootUid\": 1",
            "\"umountModules\": false" to "\"umountModules\": true",
            "\"profileKey\": \"test.app\"" to "\"profileKey\": \"../escape\"")
            .forEach { (old, new) -> assertFails { AllowlistBackup.parse(json().replace(old, new)) } }
    }

    @Test fun duplicateUidOrSharedPolicyConflictsAreRejected() {
        assertFails { AllowlistBackup.encode(document(entry(), entry())) }
        assertFails { AllowlistBackup.encode(document(entry(), entry(1010123).let {
            it.copy(profile = it.profile.copy(rules = "different"))
        })) }
    }

    @Test fun streamRejectsExcessSizeAndMalformedUtf8() {
        assertFails { AllowlistBackup.read(ByteArray(4 * 1024 * 1024 + 1).inputStream()) }
        assertFails { AllowlistBackup.read(byteArrayOf(0xc3.toByte(), 0x28).inputStream()) }
    }

    @Test fun exportNeverContainsManagerTrustFields() {
        val output = json()
        assertFalse(output.contains("dynamic", ignoreCase = true))
        assertTrue(output.contains("signingCertificates"))
        assertTrue(output.contains("defaultUmountModules"))
    }

    @Test fun v1RemainsReadableButCannotBeExportedWithoutCertificates() {
        val root = JsonParser.parseString(json()).asJsonObject
        root.addProperty("version", 1)
        root.getAsJsonArray("apps")[0].asJsonObject.remove("signingCertificates")
        val document = AllowlistBackup.parse(root.toString())
        assertEquals(1, document.version)
        assertTrue(document.apps.single().signingCertificates.isEmpty())
        assertFails { AllowlistBackup.encode(document) }
    }

    @Test fun legacyDefaultDomainsMigrateWithoutChangingCustomProfiles() {
        val root = entry(10124, "test.root").let {
            it.copy(profile = it.profile.copy(allowSu = true))
        }
        val custom = entry(10125, "test.custom").let {
            it.copy(profile = it.profile.copy(allowSu = true, rootUseDefault = false,
                context = "u:r:su:s0", flags = 0, rules = "allow su app_data_file file read;"))
        }
        val original = document(entry(), root, custom)
        val legacy = AllowlistBackup.encode(original).replace("u:r:ksu:s0", "u:r:su:s0")
        val restored = AllowlistBackup.parse(legacy)
        assertEquals(original, restored)
        assertEquals("u:r:su:s0", restored.apps.last().profile.context)
    }

    @Test fun v2RequiresExactPackageCertificateCoverageAndValidDistinctDigests() {
        val source = entry()
        listOf(
            emptyMap(), mapOf("test.other" to listOf("a".repeat(64))),
            mapOf("test.app" to emptyList()), mapOf("test.app" to listOf("A".repeat(64))),
            mapOf("test.app" to listOf("a".repeat(63))), mapOf("test.app" to listOf("g".repeat(64))),
            mapOf("test.app" to listOf("a".repeat(64), "a".repeat(64)))
        ).forEach { assertFails { AllowlistBackup.encode(document(source.copy(signingCertificates = it))) } }
        val root = JsonParser.parseString(json()).asJsonObject
        root.getAsJsonArray("apps")[0].asJsonObject.remove("signingCertificates")
        assertFails { AllowlistBackup.parse(root.toString()) }
    }

    @Test fun duplicatePackageWithinOneUserIsRejectedEvenWithDifferentUids() {
        assertFails { AllowlistBackup.encode(document(entry(), entry(10500))) }
    }
}
