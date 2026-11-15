package org.rouyashiki.yukisu.ui.screen.partition

import android.annotation.SuppressLint
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.core.content.edit
import org.rouyashiki.yukisu.ui.util.SnackbarController
import org.rouyashiki.yukisu.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private const val PARTITION_MANAGER_PREFS = "partition_manager_prefs"
private const val KEY_BACKUP_DIRECTORY = "backup_directory"

internal val BOOT_CRITICAL_PARTITIONS = setOf(
    "boot",
    "init_boot",
    "vendor_boot",
    "vendor_kernel_boot",
    "dtbo",
    "vbmeta",
    "recovery",
)

data class SlotInfo(
    val isAbDevice: Boolean,
    val currentSlot: String?,
    val otherSlot: String?,
)

data class PartitionInfo(
    val name: String,
    val blockDevice: String,
    val type: String,
    val size: Long,
    val isLogical: Boolean,
    val isDangerous: Boolean = false,
    val excludeFromBatch: Boolean = false,
)

internal fun partitionSlotSuffix(blockDevice: String): String? {
    val deviceName = blockDevice.trimEnd('/').substringAfterLast('/')
    if (deviceName.length < 3) return null
    return deviceName
        .takeLast(2)
        .lowercase(Locale.ROOT)
        .takeIf { it == "_a" || it == "_b" }
}

internal data class PartitionLoadSnapshot(
    val slotInfo: SlotInfo?,
    val selectedSlot: String?,
    val commonPartitions: List<PartitionInfo>,
    val allPartitions: List<PartitionInfo>,
)

internal data class StagedFlashImage(
    val partition: PartitionInfo,
    val slot: String?,
    val displayName: String,
    val cacheFile: File,
) {
    val size: Long
        get() = cacheFile.length()
}

internal data class Ak3PackageInfo(
    val kernelName: String,
    val devices: List<String>,
    val packageSlotPolicy: String?,
)

internal data class StagedAk3Package(
    val displayName: String,
    val cacheFile: File,
    val info: Ak3PackageInfo,
) {
    val size: Long
        get() = cacheFile.length()
}

fun formatSize(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return String.format(Locale.US, "%.2f KB", kb)
    val mb = kb / 1024.0
    if (mb < 1024) return String.format(Locale.US, "%.2f MB", mb)
    val gb = mb / 1024.0
    if (gb < 1024) return String.format(Locale.US, "%.2f GB", gb)
    return String.format(Locale.US, "%.2f TB", gb / 1024.0)
}

internal fun totalPartitionSize(partitions: Collection<PartitionInfo>): Long {
    return partitions.fold(0L) { total, partition ->
        if (partition.size <= 0L || Long.MAX_VALUE - total < partition.size) {
            if (partition.size <= 0L) total else Long.MAX_VALUE
        } else {
            total + partition.size
        }
    }
}

internal class InsufficientCacheSpaceException(
    val requiredBytes: Long,
    val availableBytes: Long,
) : IOException()

@SuppressLint("UsableSpace")
internal fun availableStorageBytes(path: String): Long {
    if (path.isBlank()) return 0L
    var candidate: File? = File(path.trim())
    while (candidate != null && !candidate.exists()) {
        candidate = candidate.parentFile
    }
    return candidate?.takeIf { it.isDirectory }?.usableSpace ?: 0L
}

@SuppressLint("UsableSpace")
internal fun stageFlashImage(
    context: Context,
    uri: Uri,
    partition: PartitionInfo,
    slot: String?,
): StagedFlashImage {
    val metadata = context.contentResolver.query(
        uri,
        arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
        null,
        null,
        null,
    )?.use { cursor ->
        if (!cursor.moveToFirst()) return@use null
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
        val name = if (nameIndex >= 0) cursor.getString(nameIndex) else null
        val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) {
            cursor.getLong(sizeIndex)
        } else {
            -1L
        }
        name to size
    }
    val displayName = metadata?.first?.takeIf { it.isNotBlank() } ?: "partition.img"
    val reportedSize = metadata?.second ?: -1L
    val cacheAvailable = context.cacheDir.usableSpace
    if (reportedSize > cacheAvailable) {
        throw InsufficientCacheSpaceException(reportedSize, cacheAvailable)
    }

    val cacheFile = File.createTempFile("partition_flash_", ".img", context.cacheDir)
    try {
        val input = context.contentResolver.openInputStream(uri)
            ?: throw IOException("The selected document cannot be opened")
        input.use { source ->
            cacheFile.outputStream().buffered().use { target ->
                source.copyTo(target, DEFAULT_BUFFER_SIZE)
            }
        }
        return StagedFlashImage(
            partition = partition,
            slot = slot,
            displayName = displayName,
            cacheFile = cacheFile,
        )
    } catch (error: Throwable) {
        cacheFile.delete()
        throw error
    }
}

@SuppressLint("UsableSpace")
internal fun stageAk3Package(
    context: Context,
    uri: Uri,
): Pair<String, File> {
    val metadata = context.contentResolver.query(
        uri,
        arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
        null,
        null,
        null,
    )?.use { cursor ->
        if (!cursor.moveToFirst()) return@use null
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
        val name = if (nameIndex >= 0) cursor.getString(nameIndex) else null
        val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) {
            cursor.getLong(sizeIndex)
        } else {
            -1L
        }
        name to size
    }
    val displayName = metadata?.first?.takeIf { it.isNotBlank() } ?: "AnyKernel3.zip"
    val reportedSize = metadata?.second ?: -1L
    val cacheAvailable = context.cacheDir.usableSpace
    if (reportedSize > cacheAvailable) {
        throw InsufficientCacheSpaceException(reportedSize, cacheAvailable)
    }

    val cacheFile = File.createTempFile("ak3_flash_", ".zip", context.cacheDir)
    try {
        val input = context.contentResolver.openInputStream(uri)
            ?: throw IOException("The selected document cannot be opened")
        input.use { source ->
            cacheFile.outputStream().buffered().use { target ->
                source.copyTo(target, DEFAULT_BUFFER_SIZE)
            }
        }
        if (cacheFile.length() <= 0L) {
            throw IOException("The selected package is empty")
        }
        return displayName to cacheFile
    } catch (error: Throwable) {
        cacheFile.delete()
        throw error
    }
}

@SuppressLint("UsableSpace")
suspend fun handlePartitionBackup(
    context: Context,
    partition: PartitionInfo,
    slot: String?,
    backupDirectory: String,
    snackbarHost: SnackbarController,
) {
    val directoryPath = backupDirectory.trim()
    if (directoryPath.isBlank()) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_backup_directory_empty))
        }
        return
    }

    val backupDir = File(directoryPath)
    if (!withContext(Dispatchers.IO) { ensureBackupDirectory(backupDir) }) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_backup_directory_create_failed,
                    backupDir.absolutePath,
                )
            )
        }
        return
    }

    val available = backupDir.usableSpace
    if (partition.size > 0L && available < partition.size) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_insufficient_space,
                    formatSize(partition.size),
                    formatSize(available),
                )
            )
        }
        return
    }

    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    val outputFile = uniqueFile(backupDir, "${partition.name}_$timestamp", ".img")
    val logs = mutableListOf<String>()
    val commandSucceeded = PartitionManagerHelper.backupPartition(
        context = context,
        partition = partition.name,
        outputPath = outputFile.absolutePath,
        slot = slot,
        onStdout = { line -> logs.add(line) },
        onStderr = { line -> logs.add("ERROR: $line") },
    )
    val actualSize = outputFile.length()
    val sizeVerified = partition.size <= 0L || actualSize == partition.size
    if (!commandSucceeded || !sizeVerified) {
        withContext(Dispatchers.IO) { outputFile.delete() }
    }

    withContext(Dispatchers.Main) {
        when {
            commandSucceeded && sizeVerified -> snackbarHost.showSnackbar(
                context.getString(R.string.partition_backup_success, outputFile.absolutePath)
            )
            commandSucceeded -> snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_backup_size_mismatch,
                    formatSize(partition.size),
                    formatSize(actualSize),
                )
            )
            logs.isNotEmpty() -> snackbarHost.showSnackbar(
                context.getString(R.string.partition_backup_failed, logs.last())
            )
            else -> snackbarHost.showSnackbar(
                context.getString(R.string.partition_backup_failed_check_log)
            )
        }
    }
}

@SuppressLint("UsableSpace")
suspend fun handleBatchBackup(
    context: Context,
    selectedPartitionNames: Set<String>,
    allPartitions: List<PartitionInfo>,
    slot: String?,
    backupDirectory: String,
    snackbarHost: SnackbarController,
    onProgress: (current: Int, total: Int, partition: String) -> Unit = { _, _, _ -> },
) {
    val partitionsToBackup = allPartitions
        .filter { it.name in selectedPartitionNames && !it.excludeFromBatch }
        .distinctBy { it.name }
    if (partitionsToBackup.isEmpty()) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_no_selection))
        }
        return
    }

    val directoryPath = backupDirectory.trim()
    if (directoryPath.isBlank()) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(context.getString(R.string.partition_backup_directory_empty))
        }
        return
    }

    val rootBackupDir = File(directoryPath)
    if (!withContext(Dispatchers.IO) { ensureBackupDirectory(rootBackupDir) }) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_backup_directory_create_failed,
                    rootBackupDir.absolutePath,
                )
            )
        }
        return
    }

    val required = totalPartitionSize(partitionsToBackup)
    val available = rootBackupDir.usableSpace
    if (required > 0L && available < required) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_insufficient_space,
                    formatSize(required),
                    formatSize(available),
                )
            )
        }
        return
    }

    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    val backupDir = uniqueDirectory(rootBackupDir, "partition_backup_$timestamp")
    if (!withContext(Dispatchers.IO) { ensureBackupDirectory(backupDir) }) {
        withContext(Dispatchers.Main) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_backup_directory_create_failed,
                    backupDir.absolutePath,
                )
            )
        }
        return
    }

    val manifestEntries = mutableListOf<String>()
    val failedPartitions = mutableListOf<String>()
    var successCount = 0
    for ((index, partition) in partitionsToBackup.withIndex()) {
        withContext(Dispatchers.Main) {
            onProgress(index + 1, partitionsToBackup.size, partition.name)
        }

        val outputFile = File(backupDir, "${partition.name}.img")
        val logs = mutableListOf<String>()
        val commandSucceeded = PartitionManagerHelper.backupPartition(
            context = context,
            partition = partition.name,
            outputPath = outputFile.absolutePath,
            slot = slot,
            onStdout = { line -> logs.add(line) },
            onStderr = { line -> logs.add("ERROR: $line") },
        )
        val actualSize = outputFile.length()
        val sizeVerified = partition.size <= 0L || actualSize == partition.size
        val succeeded = commandSucceeded && sizeVerified
        if (succeeded) {
            successCount++
        } else {
            failedPartitions.add(partition.name)
            withContext(Dispatchers.IO) { outputFile.delete() }
        }
        manifestEntries += listOf(
            partition.name,
            if (partition.isLogical) "logical" else "physical",
            partition.size.toString(),
            actualSize.toString(),
            if (succeeded) "ok" else "failed",
        ).joinToString("\t")
    }

    withContext(Dispatchers.IO) {
        File(backupDir, "manifest.tsv").writeText(
            buildString {
                appendLine("# YukiSU partition backup")
                appendLine("# created=$timestamp")
                appendLine("# slot=${slot ?: "none"}")
                appendLine("partition\ttype\texpected_bytes\tactual_bytes\tstatus")
                manifestEntries.forEach(::appendLine)
            }
        )
    }

    withContext(Dispatchers.Main) {
        if (failedPartitions.isEmpty()) {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_batch_backup_complete,
                    successCount,
                    backupDir.absolutePath,
                )
            )
        } else {
            snackbarHost.showSnackbar(
                context.getString(
                    R.string.partition_batch_backup_partial,
                    successCount,
                    failedPartitions.size,
                    failedPartitions.joinToString(),
                )
            )
        }
    }
}

fun defaultPartitionBackupDirectory(): String {
    return Environment.getExternalStoragePublicDirectory(
        Environment.DIRECTORY_DOWNLOADS
    ).absolutePath
}

fun loadPartitionBackupDirectory(context: Context): String {
    return context
        .getSharedPreferences(PARTITION_MANAGER_PREFS, Context.MODE_PRIVATE)
        .getString(KEY_BACKUP_DIRECTORY, defaultPartitionBackupDirectory())
        ?: defaultPartitionBackupDirectory()
}

fun savePartitionBackupDirectory(context: Context, path: String) {
    context.getSharedPreferences(PARTITION_MANAGER_PREFS, Context.MODE_PRIVATE).edit {
        putString(KEY_BACKUP_DIRECTORY, path)
    }
}

fun ensureBackupDirectory(directory: File): Boolean {
    return (directory.exists() && directory.isDirectory) || directory.mkdirs()
}

fun resolveBackupDirectoryPath(uri: Uri): String? {
    val treeDocumentId = runCatching {
        DocumentsContract.getTreeDocumentId(uri)
    }.getOrNull() ?: return null

    if (treeDocumentId.startsWith("raw:")) {
        return treeDocumentId.removePrefix("raw:").takeIf { it.isNotBlank() }
    }

    val splitIndex = treeDocumentId.indexOf(':')
    val volume = if (splitIndex >= 0) treeDocumentId.substring(0, splitIndex) else treeDocumentId
    val relativePath = if (splitIndex >= 0) treeDocumentId.substring(splitIndex + 1) else ""
    val root = when (volume) {
        "primary" -> Environment.getExternalStorageDirectory()
        "home" -> File(
            Environment.getExternalStorageDirectory(),
            Environment.DIRECTORY_DOCUMENTS,
        )
        else -> File("/storage", volume).takeIf { it.exists() && it.isDirectory }
    } ?: return null

    return runCatching {
        val canonicalRoot = root.canonicalFile
        val resolved = if (relativePath.isBlank()) {
            canonicalRoot
        } else {
            File(canonicalRoot, relativePath).canonicalFile
        }
        resolved.absolutePath.takeIf {
            resolved == canonicalRoot || resolved.toPath().startsWith(canonicalRoot.toPath())
        }
    }.getOrNull()
}

private fun uniqueFile(directory: File, baseName: String, extension: String): File {
    var candidate = File(directory, "$baseName$extension")
    var index = 2
    while (candidate.exists()) {
        candidate = File(directory, "${baseName}_$index$extension")
        index++
    }
    return candidate
}

private fun uniqueDirectory(directory: File, baseName: String): File {
    var candidate = File(directory, baseName)
    var index = 2
    while (candidate.exists()) {
        candidate = File(directory, "${baseName}_$index")
        index++
    }
    return candidate
}
