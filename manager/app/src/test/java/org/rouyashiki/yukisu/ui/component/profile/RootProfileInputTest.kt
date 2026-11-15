package org.rouyashiki.yukisu.ui.component.profile

import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class RootProfileInputTest {
    @Test fun acceptsNonNegativeIdsIncludingBoundaries() {
        for (value in listOf("0", "1000", "2000", "10123", "00042", Int.MAX_VALUE.toString())) {
            assertTrue(isTextValidUid(value), value)
        }
    }

    @Test fun rejectsOverflowWithoutThrowing() {
        for (value in listOf("2147483648", "4294967295", "9".repeat(256))) {
            assertFalse(isTextValidUid(value), value)
        }
    }

    @Test fun rejectsNonNumericInputWithoutThrowing() {
        for (value in listOf("", "-1", "+1", " 1000", "1000 ", "1.0", "1e3", "root")) {
            assertFalse(isTextValidUid(value), value)
        }
    }
}
