package org.rouyashiki.yukisu.ui.webui

import androidx.compose.material3.ButtonColors
import androidx.compose.material3.CardColors
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.surfaceColorAtElevation
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.dergoogler.mmrl.ext.toCssValue
import com.dergoogler.mmrl.ui.component.button.defaultFilledTonalButtonColors
import com.dergoogler.mmrl.ui.component.card.defaultCardColors
import java.util.Locale

internal object WebUiColorMaps {
    class Snapshot(
        @JvmField val colorScheme: ColorScheme,
        @JvmField val filledTonalButtonColors: ButtonColors,
        @JvmField val cardColors: CardColors,
        @JvmField val colorSchemeMap: Map<String, Color>,
        @JvmField val filledTonalButtonColorsMap: Map<String, Color>,
        @JvmField val cardColorsMap: Map<String, Color>,
        @JvmField val allCssColors: String,
    )

    @JvmStatic
    fun create(colorScheme: ColorScheme): Snapshot {
        val buttons = colorScheme.defaultFilledTonalButtonColors
        val cards = colorScheme.defaultCardColors
        val colors = linkedMapOf(
            "background" to colorScheme.background,
            "error" to colorScheme.error,
            "errorContainer" to colorScheme.errorContainer,
            "inverseOnSurface" to colorScheme.inverseOnSurface,
            "inversePrimary" to colorScheme.inversePrimary,
            "inverseSurface" to colorScheme.inverseSurface,
            "onBackground" to colorScheme.onBackground,
            "onError" to colorScheme.onError,
            "onErrorContainer" to colorScheme.onErrorContainer,
            "onPrimary" to colorScheme.onPrimary,
            "onPrimaryContainer" to colorScheme.onPrimaryContainer,
            "onPrimaryFixed" to colorScheme.onPrimaryFixed,
            "onPrimaryFixedVariant" to colorScheme.onPrimaryFixedVariant,
            "onSecondary" to colorScheme.onSecondary,
            "onSecondaryContainer" to colorScheme.onSecondaryContainer,
            "onSecondaryFixed" to colorScheme.onSecondaryFixed,
            "onSecondaryFixedVariant" to colorScheme.onSecondaryFixedVariant,
            "onSurface" to colorScheme.onSurface,
            "onSurfaceVariant" to colorScheme.onSurfaceVariant,
            "onTertiary" to colorScheme.onTertiary,
            "onTertiaryContainer" to colorScheme.onTertiaryContainer,
            "onTertiaryFixed" to colorScheme.onTertiaryFixed,
            "onTertiaryFixedVariant" to colorScheme.onTertiaryFixedVariant,
            "outline" to colorScheme.outline,
            "outlineVariant" to colorScheme.outlineVariant,
            "primary" to colorScheme.primary,
            "primaryContainer" to colorScheme.primaryContainer,
            "primaryFixed" to colorScheme.primaryFixed,
            "primaryFixedDim" to colorScheme.primaryFixedDim,
            "scrim" to colorScheme.scrim,
            "secondary" to colorScheme.secondary,
            "secondaryContainer" to colorScheme.secondaryContainer,
            "secondaryFixed" to colorScheme.secondaryFixed,
            "secondaryFixedDim" to colorScheme.secondaryFixedDim,
            "surface" to colorScheme.surface,
            "surfaceBright" to colorScheme.surfaceBright,
            "surfaceContainer" to colorScheme.surfaceContainer,
            "surfaceContainerHigh" to colorScheme.surfaceContainerHigh,
            "surfaceContainerHighest" to colorScheme.surfaceContainerHighest,
            "surfaceContainerLow" to colorScheme.surfaceContainerLow,
            "surfaceContainerLowest" to colorScheme.surfaceContainerLowest,
            "surfaceDim" to colorScheme.surfaceDim,
            "surfaceTint" to colorScheme.surfaceTint,
            "surfaceVariant" to colorScheme.surfaceVariant,
            "tertiary" to colorScheme.tertiary,
            "tertiaryContainer" to colorScheme.tertiaryContainer,
            "tertiaryFixed" to colorScheme.tertiaryFixed,
            "tertiaryFixedDim" to colorScheme.tertiaryFixedDim,
        )
        val buttonColors = linkedMapOf(
            "containerColor" to buttons.containerColor,
            "contentColor" to buttons.contentColor,
            "disabledContainerColor" to buttons.disabledContainerColor,
            "disabledContentColor" to buttons.disabledContentColor,
        )
        val cardColors = linkedMapOf(
            "containerColor" to cards.containerColor,
            "contentColor" to cards.contentColor,
            "disabledContainerColor" to cards.disabledContainerColor,
            "disabledContentColor" to cards.disabledContentColor,
        )
        val css = buildString {
            appendLine(":root {")
            appendLine("\t/* App Base Colors */")
            colors.forEach { (name, color) ->
                appendLine("\t--$name: ${color.toCssValue()};")
                if (name == "surface") {
                    appendLine("\t--tonalSurface: ${colorScheme.surfaceColorAtElevation(1.dp).toCssValue()};")
                }
            }
            appendLine("\t/* Filled Tonal Button Colors */")
            buttonColors.forEach { (name, color) ->
                appendLine("\t--filledTonalButton${name.cssSuffix()}: ${color.toCssValue()};")
            }
            appendLine("\t/* Filled Card Colors */")
            cardColors.forEach { (name, color) ->
                appendLine("\t--filledCard${name.cssSuffix()}: ${color.toCssValue()};")
            }
            appendLine("}")
        }
        return Snapshot(colorScheme, buttons, cards, colors, buttonColors, cardColors, css)
    }

    private fun String.cssSuffix(): String = replaceFirstChar {
        if (it.isLowerCase()) it.titlecase(Locale.getDefault()) else it.toString()
    }

    @JvmStatic
    fun asStringMap(value: Any?): Map<String, Any?> {
        if (value !is Map<*, *>) return emptyMap()
        return buildMap {
            value.forEach { (key, entry) -> if (key is String) put(key, entry) }
        }
    }
}
