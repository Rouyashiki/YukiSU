package org.rouyashiki.yukisu

import kotlin.test.Test
import kotlin.test.assertEquals

class NativesProfileTest {
    @Test fun nativeCapabilityReadsCanPopulateEveryList() {
        val profile = Natives.Profile()
        val add = List::class.java.getMethod("add", Any::class.java)
        listOf(profile.capabilities, profile.capabilitiesPermitted, profile.capabilitiesInheritable).forEach {
            add.invoke(it, 1)
            assertEquals(listOf(1), it)
        }
        val another = Natives.Profile()
        assertEquals(emptyList(), another.capabilitiesPermitted)
        assertEquals(emptyList(), another.capabilitiesInheritable)
    }
}
