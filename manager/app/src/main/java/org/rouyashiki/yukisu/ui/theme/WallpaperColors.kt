package org.rouyashiki.yukisu.ui.theme

import android.app.UiModeManager
import android.app.WallpaperColors
import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.compose.material3.ColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import com.materialkolor.hct.Hct
import com.materialkolor.scheme.SchemeTonalSpot

private const val MAX_SAMPLE_EDGE = 128

internal fun extractWallpaperSeed(context: Context, uri: Uri): Int? {
    val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    fun decode() = context.contentResolver.openInputStream(uri)?.use {
        BitmapFactory.decodeStream(it, null, options)
    }

    return try {
        decode()
        if (options.outWidth <= 0 || options.outHeight <= 0) return null
        val longestEdge = maxOf(options.outWidth, options.outHeight)
        options.inJustDecodeBounds = false
        options.inSampleSize = 1
        while (longestEdge > MAX_SAMPLE_EDGE.toLong() * options.inSampleSize) {
            options.inSampleSize *= 2
        }
        val bitmap = decode() ?: return null
        try {
            WallpaperColors.fromBitmap(bitmap).primaryColor.toArgb()
        } finally {
            bitmap.recycle()
        }
    } catch (e: Exception) {
        Log.w("WallpaperColors", "Could not extract a seed from the custom background", e)
        null
    }
}

@Composable
internal fun rememberWallpaperColorScheme(context: Context, seed: Int, dark: Boolean): ColorScheme {
    val contrast = if (Build.VERSION.SDK_INT >= 34) {
        val manager = remember(context) { context.getSystemService(UiModeManager::class.java) }
        val level by produceState(manager.contrast, manager) {
            val listener = UiModeManager.ContrastChangeListener { value = it }
            manager.addContrastChangeListener(context.mainExecutor, listener)
            value = manager.contrast
            awaitDispose { manager.removeContrastChangeListener(listener) }
        }
        level.toDouble()
    } else {
        0.0
    }
    return remember(seed, dark, contrast) { wallpaperColorScheme(seed, dark, contrast) }
}

internal fun wallpaperColorScheme(seed: Int, dark: Boolean, contrast: Double): ColorScheme {
    val scheme = SchemeTonalSpot(Hct.fromInt(seed), dark, contrast)
    return ColorScheme(
        primary = Color(scheme.primary),
        onPrimary = Color(scheme.onPrimary),
        primaryContainer = Color(scheme.primaryContainer),
        onPrimaryContainer = Color(scheme.onPrimaryContainer),
        inversePrimary = Color(scheme.inversePrimary),
        secondary = Color(scheme.secondary),
        onSecondary = Color(scheme.onSecondary),
        secondaryContainer = Color(scheme.secondaryContainer),
        onSecondaryContainer = Color(scheme.onSecondaryContainer),
        tertiary = Color(scheme.tertiary),
        onTertiary = Color(scheme.onTertiary),
        tertiaryContainer = Color(scheme.tertiaryContainer),
        onTertiaryContainer = Color(scheme.onTertiaryContainer),
        background = Color(scheme.background),
        onBackground = Color(scheme.onBackground),
        surface = Color(scheme.surface),
        onSurface = Color(scheme.onSurface),
        surfaceVariant = Color(scheme.surfaceVariant),
        onSurfaceVariant = Color(scheme.onSurfaceVariant),
        surfaceTint = Color(scheme.surfaceTint),
        inverseSurface = Color(scheme.inverseSurface),
        inverseOnSurface = Color(scheme.inverseOnSurface),
        error = Color(scheme.error),
        onError = Color(scheme.onError),
        errorContainer = Color(scheme.errorContainer),
        onErrorContainer = Color(scheme.onErrorContainer),
        outline = Color(scheme.outline),
        outlineVariant = Color(scheme.outlineVariant),
        scrim = Color(scheme.scrim),
        surfaceBright = Color(scheme.surfaceBright),
        surfaceDim = Color(scheme.surfaceDim),
        surfaceContainer = Color(scheme.surfaceContainer),
        surfaceContainerHigh = Color(scheme.surfaceContainerHigh),
        surfaceContainerHighest = Color(scheme.surfaceContainerHighest),
        surfaceContainerLow = Color(scheme.surfaceContainerLow),
        surfaceContainerLowest = Color(scheme.surfaceContainerLowest),
        primaryFixed = Color(scheme.primaryFixed),
        primaryFixedDim = Color(scheme.primaryFixedDim),
        onPrimaryFixed = Color(scheme.onPrimaryFixed),
        onPrimaryFixedVariant = Color(scheme.onPrimaryFixedVariant),
        secondaryFixed = Color(scheme.secondaryFixed),
        secondaryFixedDim = Color(scheme.secondaryFixedDim),
        onSecondaryFixed = Color(scheme.onSecondaryFixed),
        onSecondaryFixedVariant = Color(scheme.onSecondaryFixedVariant),
        tertiaryFixed = Color(scheme.tertiaryFixed),
        tertiaryFixedDim = Color(scheme.tertiaryFixedDim),
        onTertiaryFixed = Color(scheme.onTertiaryFixed),
        onTertiaryFixedVariant = Color(scheme.onTertiaryFixedVariant),
    )
}
