package org.rouyashiki.yukisu.ui.kasumi.util

private val kasumiKernelPrefix = Regex(
    "^\\s*(?:<\\d+>)?\\s*(?:\\[\\s*\\d+(?:\\.\\d+)?]\\s*)?" +
        "(?:\\[\\s*[TC]\\d+]\\s*)?KernelSU: kasumi:(?:\\s|$)",
)

internal fun filterKasumiKernelLog(content: String): String = content.lineSequence()
    .filter { kasumiKernelPrefix.containsMatchIn(it) }
    .toList()
    .takeLast(1000)
    .joinToString("\n")
