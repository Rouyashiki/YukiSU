package org.rouyashiki.yukisu.ui.util.module

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ModuleActionAvailabilityTest {
    private val active = """{"id":"declared.id","dir_id":"installed.id","name":"Module","action":true,"enabled":true,"update":false,"remove":false}"""

    @Test fun resolvesTheInstalledDirectoryInsteadOfTheDeclaredId() {
        assertEquals(ModuleActionAvailability("Module", true), moduleActionAvailability("[$active]", "installed.id"))
        assertNull(moduleActionAvailability("[$active]", "declared.id"))
        assertNull(moduleActionAvailability("[]", "installed.id"))
    }

    @Test fun acceptsBothCliBooleanEncodings() {
        val quoted = active.replace(":true", ":\"true\"").replace(":false", ":\"false\"")
        assertTrue(moduleActionAvailability("[$quoted]", "installed.id")!!.available)
        val legacy = active.replace("\"dir_id\":\"installed.id\",", "")
        assertTrue(moduleActionAvailability("[$legacy]", "declared.id")!!.available)
    }

    @Test fun rejectsEveryUnavailableModuleState() {
        listOf(
            active.replace("\"action\":true", "\"action\":false"),
            active.replace("\"enabled\":true", "\"enabled\":false"),
            active.replace("\"update\":false", "\"update\":true"),
            active.replace("\"remove\":false", "\"remove\":true"),
        ).forEach { assertFalse(moduleActionAvailability("[$it]", "installed.id")!!.available) }
    }

    @Test fun cannotAuthorizeAnActionFromIncompleteOrUnknownFlags() {
        listOf(
            active.replace("\"enabled\":true,", ""),
            active.replace("\"update\":false", "\"update\":\"unknown\""),
        ).forEach { assertFalse(moduleActionAvailability("[$it]", "installed.id")!!.available) }
    }
}
