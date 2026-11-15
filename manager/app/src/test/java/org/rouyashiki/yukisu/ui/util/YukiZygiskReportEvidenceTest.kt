package org.rouyashiki.yukisu.ui.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class YukiZygiskReportEvidenceTest {
    @Test
    fun enabledFeatureCollectsWithoutStoredEvidence() {
        assertTrue(shouldCollectYukiZygiskReport(true, YukiZygiskReportEvidence()))
    }

    @Test
    fun disabledFeatureCollectsCurrentOrOldDiagnostics() {
        assertTrue(
            shouldCollectYukiZygiskReport(
                false,
                YukiZygiskReportEvidence(currentDiagnostics = true),
            ),
        )
        assertTrue(
            shouldCollectYukiZygiskReport(
                false,
                YukiZygiskReportEvidence(oldDiagnostics = true),
            ),
        )
    }

    @Test
    fun unavailableFeatureCollectsLegacyLogs() {
        assertTrue(
            shouldCollectYukiZygiskReport(
                null,
                YukiZygiskReportEvidence(legacyLogs = true),
            ),
        )
    }

    @Test
    fun disabledOrUnavailableFeatureWithoutEvidenceIsSkipped() {
        assertFalse(shouldCollectYukiZygiskReport(false, YukiZygiskReportEvidence()))
        assertFalse(shouldCollectYukiZygiskReport(null, YukiZygiskReportEvidence()))
    }

    @Test
    fun outerBugreportArtifactsUseExplicitReferences() {
        assertEquals(
            "outer bugreport/tombstones.tar.gz",
            outerBugreportArtifactReference("tombstones.tar.gz", true),
        )
        assertEquals(
            "unavailable",
            outerBugreportArtifactReference("pstore.tar.gz", false),
        )
    }
}
