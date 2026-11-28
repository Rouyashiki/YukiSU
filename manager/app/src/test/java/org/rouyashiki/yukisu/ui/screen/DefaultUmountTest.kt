package org.rouyashiki.yukisu.ui.screen

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.rouyashiki.yukisu.readDefaultUmountModules

class DefaultUmountTest {
    @Test
    fun defaultUmountDistinguishesOffOnAndUnknown() {
        assertEquals(false, readDefaultUmountModules { 0 })
        assertEquals(true, readDefaultUmountModules { 1 })
        assertNull(readDefaultUmountModules { -1 })
    }

    @Test
    fun successfulSaveUsesAcknowledgedValueAndFailedSaveReadsCurrentState() {
        for (enabled in listOf(false, true)) {
            assertEquals(true to enabled, changeDefaultUmount(enabled, write = { it == enabled }, read = { error("unexpected read") }))
            assertEquals(false to !enabled, changeDefaultUmount(enabled, write = { false }, read = { !enabled }))
            assertEquals(false to null, changeDefaultUmount(enabled, write = { false }, read = { null }))
        }
    }
}
