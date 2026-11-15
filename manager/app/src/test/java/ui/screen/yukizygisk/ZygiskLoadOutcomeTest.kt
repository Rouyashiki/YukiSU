package ui.screen.yukizygisk

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ZygiskLoadOutcomeTest {
    @Test
    fun bothReportOrdersPreservePartialFailure() {
        val success = ZygiskLoadOutcome.fromRecord("injected", true, false)
        val failure = ZygiskLoadOutcome.fromRecord("failed", false, true)
        assertTrue(success.merge(failure).partial)
        assertEquals(success.merge(failure), failure.merge(success))
        assertEquals(success, success.merge(success))
    }

    @Test
    fun stickyLegacyStateDoesNotHideSuccessfulLoads() {
        val result = ZygiskLoadOutcome.fromRecord("failed", true, true)
        assertTrue(result.partial)
        assertTrue(result.complete)
    }

    @Test
    fun legacyFailuresCannotProveThatNoLoadSucceeded() {
        val result = ZygiskLoadOutcome.fromRecord("failed", null, null)
        assertTrue(result.failure)
        assertFalse(result.success)
        assertFalse(result.complete)
        assertFalse(result.merge(ZygiskLoadOutcome(false, true, true)).complete)
    }

    @Test
    fun incompleteOrAbsentFieldsUseLegacyEvidence() {
        assertEquals(ZygiskLoadOutcome(true), ZygiskLoadOutcome.fromRecord("injected", false, null))
        assertEquals(ZygiskLoadOutcome(), ZygiskLoadOutcome.fromRecord("unknown", null, null))
        assertFalse(ZygiskLoadOutcome.fromRecord("failed", false, false).failure)
    }
}
