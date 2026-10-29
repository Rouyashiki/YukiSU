package com.anatdx.yukisu.ui.motion

enum class PageTransitionStyle(val persistedValue: String) {
    Reveal("reveal"),
    Slide("slide");

    companion object {
        fun fromPersistedValue(value: String?): PageTransitionStyle =
            entries.firstOrNull { it.persistedValue == value } ?: Reveal
    }
}
