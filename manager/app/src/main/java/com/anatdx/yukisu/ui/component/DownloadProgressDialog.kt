package com.anatdx.yukisu.ui.component

import android.text.format.Formatter
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.LinearWavyProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.anatdx.yukisu.ui.util.DownloadProgress

@Composable
fun DownloadProgressDialog(
    title: String,
    message: String,
    progress: DownloadProgress,
    onCancel: () -> Unit,
) {
    val context = LocalContext.current
    val downloaded = Formatter.formatShortFileSize(context, progress.downloadedBytes)
    val status = progress.totalBytes?.takeIf { it > 0L }?.let { totalBytes ->
        val percent = ((progress.downloadedBytes * 100L) / totalBytes).coerceIn(0L, 100L)
        "$percent% · $downloaded / ${Formatter.formatShortFileSize(context, totalBytes)}"
    } ?: downloaded

    YukiAlertDialog(
        onDismissRequest = onCancel,
        title = { Text(title) },
        text = {
            Column(Modifier.fillMaxWidth()) {
                Text(message)
                Spacer(Modifier.height(16.dp))
                YukiDownloadProgressIndicator(
                    progress = progress.fraction?.let { fraction -> { fraction } },
                )
                Spacer(Modifier.height(8.dp))
                Text(status)
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onCancel) {
                Text(androidx.compose.ui.res.stringResource(android.R.string.cancel))
            }
        },
        properties = DialogProperties(
            dismissOnBackPress = false,
            dismissOnClickOutside = false,
        ),
    )
}

@Composable
fun YukiDownloadProgressIndicator(progress: (() -> Float)? = null) {
    val modifier = Modifier.fillMaxWidth().widthIn(min = 240.dp)
    if (progress == null) {
        LinearWavyProgressIndicator(modifier = modifier)
    } else {
        LinearWavyProgressIndicator(progress = progress, modifier = modifier)
    }
}
