package org.rouyashiki.yukisu.ui.screen.moreSettings

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.view.WindowManager
import androidx.core.content.FileProvider
import org.rouyashiki.yukisu.R
import com.yalantis.ucrop.UCrop
import com.yalantis.ucrop.UCropActivity
import java.io.File
import kotlin.math.max
import kotlin.math.roundToInt

internal data class WallpaperCropStyle(
    val toolbarColor: Int,
    val toolbarWidgetColor: Int,
    val accentColor: Int,
    val lightStatusBar: Boolean,
)

internal fun createWallpaperCropIntent(
    context: Context,
    sourceUri: Uri,
    toolbarTitle: String,
    style: WallpaperCropStyle,
): Intent {
    val outputSize = context.deviceScreenPixelSize()
    val outputDirectory = File(context.cacheDir, "wallpaper-crops")
    check(outputDirectory.exists() || outputDirectory.mkdirs()) {
        "Unable to create wallpaper crop cache directory"
    }
    val outputUri = FileProvider.getUriForFile(
        context,
        "${context.packageName}.fileprovider",
        File(outputDirectory, "wallpaper-${System.currentTimeMillis()}.jpg"),
    )
    val density = context.resources.displayMetrics.density

    val options = UCrop.Options().apply {
        setCompressionFormat(Bitmap.CompressFormat.JPEG)
        setCompressionQuality(92)
        setMaxBitmapSize(max(outputSize.width, outputSize.height))
        setMaxScaleMultiplier(32f)
        setImageToCropBoundsAnimDuration(280)
        setAllowedGestures(UCropActivity.ALL, UCropActivity.ALL, UCropActivity.ALL)

        setToolbarTitle(toolbarTitle)
        setToolbarColor(style.toolbarColor)
        setToolbarWidgetColor(style.toolbarWidgetColor)
        setToolbarCancelDrawable(R.drawable.ms_close)
        setToolbarCropDrawable(R.drawable.ms_check)
        setActiveControlsWidgetColor(style.accentColor)
        setStatusBarLight(style.lightStatusBar)
        setNavigationBarLight(false)
        setRootViewBackgroundColor(Color.BLACK)
        setLogoColor(style.accentColor)

        setDimmedLayerColor(Color.argb(184, 0, 0, 0))
        setShowCropFrame(true)
        setCropFrameColor(Color.argb(235, 255, 255, 255))
        setCropFrameStrokeWidth((2f * density).roundToInt())
        setShowCropGrid(true)
        setCropGridRowCount(2)
        setCropGridColumnCount(2)
        setCropGridColor(Color.argb(112, 255, 255, 255))
        setCropGridStrokeWidth(max(1, density.roundToInt()))

        setCircleDimmedLayer(false)
        setFreeStyleCropEnabled(true)
        setHideBottomControls(false)
    }

    return UCrop.of(sourceUri, outputUri)
        .withAspectRatio(outputSize.width.toFloat(), outputSize.height.toFloat())
        .withMaxResultSize(outputSize.width, outputSize.height)
        .withOptions(options)
        .getIntent(context)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
}

private data class PixelSize(val width: Int, val height: Int)

private fun Context.deviceScreenPixelSize(): PixelSize {
    val windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
    val (width, height) = windowManager.maximumWindowMetrics.bounds.let { it.width() to it.height() }

    return PixelSize(
        width = width.coerceAtLeast(UCrop.MIN_SIZE),
        height = height.coerceAtLeast(UCrop.MIN_SIZE),
    )
}
