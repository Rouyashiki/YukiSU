package com.anatdx.yukisu.ui.motion

import kotlin.test.Test
import kotlin.test.assertEquals

class PageTransitionStyleTest {
    @Test
    fun selectionsRoundTripThroughTheirStablePreferenceValues() {
        val selections = mapOf(
            "reveal" to PageTransitionStyle.Reveal,
            "slide" to PageTransitionStyle.Slide,
        )
        assertEquals(selections.size, PageTransitionStyle.entries.size)
        selections.forEach { (saved, style) ->
            assertEquals(saved, style.persistedValue)
            assertEquals(style, PageTransitionStyle.fromPersistedValue(saved))
        }
    }

    @Test
    fun missingOrUnknownPreferenceUsesTheDefaultReveal() {
        listOf(null, "", "unknown-future-style").forEach { saved ->
            assertEquals(PageTransitionStyle.Reveal, PageTransitionStyle.fromPersistedValue(saved))
        }
    }
}
