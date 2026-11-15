package org.rouyashiki.yukisu.ui.screen.ramdisk

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import com.anatdx.yukifb.backend.FileExternalOpenCapability
import com.anatdx.yukifb.backend.FileShareCapability
import com.anatdx.yukifb.model.EntryId
import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import org.rouyashiki.yukisu.BuildConfig
import java.io.File
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext

internal class RamdiskExternalActions(
    private val context: Context,
    private val stat: suspend (EntryId) -> FileEntry,
    private val reader: RamdiskContentReader,
    private val files: RamdiskExternalFiles = RamdiskExternalFiles(File(context.cacheDir, "ramdisk_external")),
    private val canHandle: (Intent) -> Boolean = { context.packageManager.queryIntentActivities(it, 0).isNotEmpty() },
    private val launch: (Intent) -> Unit = context::startActivity,
) : FileShareCapability, FileExternalOpenCapability {
    override fun canShare(entry: FileEntry) = entry.type == FileEntryType.REGULAR_FILE
    override fun canOpenExternally(entry: FileEntry) = entry.type == FileEntryType.REGULAR_FILE
    override suspend fun share(entryId: EntryId, onProgress: (FileByteProgress) -> Unit) = open(entryId, true, onProgress)
    override suspend fun openExternally(entryId: EntryId, onProgress: (FileByteProgress) -> Unit) = open(entryId, false, onProgress)

    private suspend fun open(entryId: EntryId, share: Boolean, progress: (FileByteProgress) -> Unit) {
        val entry = stat(entryId)
        val file = files.prepare(entry, reader, progress)
        var handedOff = false
        try {
            withContext(Dispatchers.Main.immediate) {
                currentCoroutineContext().ensureActive()
                val uri = FileProvider.getUriForFile(context, "${BuildConfig.APPLICATION_ID}.fileprovider", file)
                val type = entry.mimeType?.takeUnless { it == "application/octet-stream" }
                    ?: MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase(Locale.ROOT))
                    ?: "application/octet-stream"
                val intent = ramdiskExternalIntent(uri, type, share)
                if (!canHandle(intent)) throw android.content.ActivityNotFoundException()
                currentCoroutineContext().ensureActive()
                launch(Intent.createChooser(intent, context.getString(if (share) com.anatdx.yukifb.R.string.yukifb_action_share else com.anatdx.yukifb.R.string.yukifb_action_open_externally)))
                handedOff = true
            }
        } finally {
            // No cancellation point after successful launch may delete a recipient's copy.
            if (!handedOff) withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { files.discard(file) }
        }
    }
}

internal fun ramdiskExternalIntent(uri: Uri, mimeType: String, share: Boolean): Intent =
    Intent(if (share) Intent.ACTION_SEND else Intent.ACTION_VIEW).apply {
        setDataAndType(uri, mimeType)
        clipData = ClipData.newRawUri("file", uri)
        if (share) putExtra(Intent.EXTRA_STREAM, uri)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
