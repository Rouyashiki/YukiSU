package com.anatdx.yukisu.ui.component

import android.content.Context
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Help
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.GetApp
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.anatdx.yukisu.ui.util.module.ModuleArchive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.apache.commons.compress.archivers.zip.ZipFile
import ui.screen.partition.PartitionManagerHelper
import ui.screen.partition.formatSize
import ui.screen.partition.stageAk3Package
import java.io.File

enum class ZipType {
    MODULE,
    KERNEL,
    UNKNOWN,
}

data class ZipFileInfo(
    val uri: Uri,
    val type: ZipType,
    val name: String = "",
    val version: String = "",
    val versionCode: String = "",
    val author: String = "",
    val description: String = "",
    val kernelVersion: String = "",
    val supported: String = "",
    val displayName: String = "",
    val stagedPath: String = "",
    val size: Long = 0L,
    val packageSlotPolicy: String = "",
    val isAbDevice: Boolean = false,
    val currentSlot: String? = null,
    val otherSlot: String? = null,
)

data class Ak3FlashOptions(
    val targetSlot: String?,
    val useMkbootfs: Boolean,
)

object ZipFileDetector {

    private fun detectZipType(zip: ZipFile): ZipType {
        var hasModuleProp = false
        var hasToolsFolder = false
        var hasAnykernelSh = false

        for (entry in zip.entries.asSequence()) {
            val entryName = entry.name.replace('\\', '/').trimStart('/').lowercase()
            when {
                entryName == "module.prop" || entryName.endsWith("/module.prop") -> {
                    hasModuleProp = true
                }

                entryName == "tools" ||
                    entryName.startsWith("tools/") ||
                    entryName.contains("/tools/") -> {
                    hasToolsFolder = true
                }

                entryName == "anykernel.sh" || entryName.endsWith("/anykernel.sh") -> {
                    hasAnykernelSh = true
                }
            }
        }

        return when {
            hasModuleProp -> ZipType.MODULE
            hasToolsFolder && hasAnykernelSh -> ZipType.KERNEL
            else -> ZipType.UNKNOWN
        }
    }

    private fun parseModuleInfo(context: Context, uri: Uri, zip: ZipFile): ZipFileInfo {
        val zipInfo = ZipFileInfo(uri = uri, type = ZipType.MODULE)
        return try {
            val props = ModuleArchive.readProperties(zip)
            zipInfo.copy(
                name = props["name"] ?: context.getString(R.string.unknown_module),
                version = props["version"].orEmpty(),
                versionCode = props["versionCode"].orEmpty(),
                author = props["author"].orEmpty(),
                description = props["description"].orEmpty(),
            )
        } catch (_: Exception) {
            // Keep the package recognizable even if optional metadata is malformed.
            zipInfo
        }
    }

    private suspend fun parseKernelInfo(context: Context, uri: Uri): ZipFileInfo {
        val (displayName, cacheFile) = stageAk3Package(context, uri)
        try {
            val packageInfo = PartitionManagerHelper.inspectAk3Package(
                context,
                cacheFile.absolutePath,
            )
            val slotInfo = PartitionManagerHelper.getSlotInfo(context)
            return ZipFileInfo(
                uri = uri,
                type = ZipType.KERNEL,
                name = packageInfo.kernelName,
                supported = packageInfo.devices.joinToString(),
                displayName = displayName,
                stagedPath = cacheFile.absolutePath,
                size = cacheFile.length(),
                packageSlotPolicy = packageInfo.packageSlotPolicy.orEmpty(),
                isAbDevice = slotInfo?.isAbDevice == true,
                currentSlot = slotInfo?.currentSlot,
                otherSlot = slotInfo?.otherSlot,
            )
        } catch (error: Throwable) {
            cacheFile.delete()
            throw error
        }
    }

    suspend fun detectAndParseZipFiles(
        context: Context,
        zipUris: List<Uri>,
    ): List<ZipFileInfo> = withContext(Dispatchers.IO) {
        val detected = zipUris.mapNotNull { uri ->
            val info = runCatching {
                ModuleArchive.withZip(context, uri) { zip ->
                    when (val type = detectZipType(zip)) {
                        ZipType.MODULE -> parseModuleInfo(context, uri, zip)
                        else -> ZipFileInfo(uri = uri, type = type)
                    }
                }
            }.getOrNull() ?: return@mapNotNull null
            when (info.type) {
                ZipType.MODULE -> info
                ZipType.KERNEL -> runCatching {
                    parseKernelInfo(context, uri)
                }.getOrNull()

                ZipType.UNKNOWN -> null
            }
        }
        if (detected.any { it.type == ZipType.KERNEL } &&
            (zipUris.size != 1 || detected.size != 1)
        ) {
            cleanupStagedFiles(detected)
            emptyList()
        } else {
            detected
        }
    }

    fun cleanupStagedFiles(zipFiles: List<ZipFileInfo>) {
        zipFiles.asSequence()
            .filter { it.type == ZipType.KERNEL && it.stagedPath.isNotBlank() }
            .map { File(it.stagedPath) }
            .forEach(File::delete)
    }
}

@Composable
fun InstallConfirmationDialog(
    show: Boolean,
    zipFiles: List<ZipFileInfo>,
    onConfirm: (List<ZipFileInfo>, Ak3FlashOptions?) -> Unit,
    onDismiss: () -> Unit,
) {
    if (!show || zipFiles.isEmpty()) return

    val resources = LocalResources.current
    val kernelFile = zipFiles.singleOrNull { it.type == ZipType.KERNEL }
    var useMkbootfs by rememberSaveable(kernelFile?.stagedPath) {
        mutableStateOf(false)
    }
    var selectedSlot by rememberSaveable(kernelFile?.stagedPath) {
        mutableStateOf(
            kernelFile?.currentSlot
                ?: kernelFile?.otherSlot
        )
    }
    val slotSelectionMissing = kernelFile?.isAbDevice == true && selectedSlot == null
    val slotConflict = kernelFile?.let {
        selectedSlot != null && !isSlotCompatible(
            selectedSlot = selectedSlot,
            currentSlot = it.currentSlot,
            packagePolicy = it.packageSlotPolicy,
        )
    } == true

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        icon = {
            YukiIcon(
                imageVector = if (kernelFile != null) Icons.Default.Memory else Icons.Default.Extension,
                contentDescription = null,
            )
        },
        title = {
            Text(
                text = if (kernelFile != null) {
                    resources.getString(R.string.partition_ak3_preflight)
                } else if (zipFiles.size == 1) {
                    resources.getString(R.string.confirm_installation)
                } else {
                    resources.getString(R.string.confirm_multiple_installation, zipFiles.size)
                },
            )
        },
        text = {
            LazyColumn(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(max = 520.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                items(zipFiles) { zipFile ->
                    InstallItemCard(zipFile = zipFile)
                }

                kernelFile?.let { ak3 ->
                    item {
                        Ak3SlotSelector(
                            zipFile = ak3,
                            selectedSlot = selectedSlot,
                            onSelected = { selectedSlot = it },
                        )
                    }
                    item {
                        Ak3MkbootfsOption(
                            checked = useMkbootfs,
                            enabled = true,
                            onCheckedChange = { useMkbootfs = it },
                        )
                    }
                    if (slotConflict) {
                        item {
                            Text(
                                text = resources.getString(
                                    R.string.partition_ak3_slot_conflict,
                                    ak3.packageSlotPolicy,
                                ),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                    item {
                        Card(
                            colors = CardDefaults.cardColors(
                                containerColor = MaterialTheme.colorScheme.errorContainer,
                            ),
                        ) {
                            Row(
                                modifier = Modifier.padding(12.dp),
                                horizontalArrangement = Arrangement.spacedBy(10.dp),
                                verticalAlignment = Alignment.Top,
                            ) {
                                Icon(
                                    imageVector = Icons.Filled.Warning,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.onErrorContainer,
                                    modifier = Modifier.size(20.dp),
                                )
                                Text(
                                    text = resources.getString(R.string.partition_ak3_trust_warning),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onErrorContainer,
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    onConfirm(
                        zipFiles,
                        kernelFile?.let {
                            Ak3FlashOptions(
                                targetSlot = selectedSlot,
                                useMkbootfs = useMkbootfs,
                            )
                        },
                    )
                },
                enabled = !slotSelectionMissing && !slotConflict,
                colors = if (kernelFile != null) {
                    ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.error,
                        contentColor = MaterialTheme.colorScheme.onError,
                    )
                } else {
                    ButtonDefaults.buttonColors()
                },
            ) {
                Icon(
                    imageVector = if (kernelFile != null) {
                        Icons.Default.Memory
                    } else {
                        Icons.Default.GetApp
                    },
                    contentDescription = null,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    resources.getString(
                        if (kernelFile != null) {
                            R.string.partition_ak3_flash_now
                        } else {
                            R.string.install_confirm
                        },
                    ),
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(resources.getString(android.R.string.cancel))
            }
        },
        modifier = Modifier.widthIn(min = 320.dp, max = 560.dp),
    )
}

private fun isSlotCompatible(
    selectedSlot: String?,
    currentSlot: String?,
    packagePolicy: String,
): Boolean {
    if (selectedSlot == null || currentSlot == null) return true
    return when (packagePolicy.trim().lowercase()) {
        "active" -> selectedSlot == currentSlot
        "inactive" -> selectedSlot != currentSlot
        else -> true
    }
}

@Composable
private fun Ak3SlotSelector(
    zipFile: ZipFileInfo,
    selectedSlot: String?,
    onSelected: (String) -> Unit,
) {
    val resources = LocalResources.current
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            text = resources.getString(R.string.partition_ak3_target_slot),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (!zipFile.isAbDevice) {
            InfoRow(
                label = resources.getString(R.string.partition_info_slot),
                value = "/",
            )
            return@Column
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            listOfNotNull(zipFile.currentSlot, zipFile.otherSlot)
                .distinct()
                .forEach { slot ->
                    FilterChip(
                        selected = slot == selectedSlot,
                        onClick = { onSelected(slot) },
                        label = {
                            Text(
                                if (slot == zipFile.currentSlot) {
                                    resources.getString(
                                        R.string.partition_ak3_current_slot,
                                        slot,
                                    )
                                } else {
                                    slot
                                },
                            )
                        },
                        leadingIcon = if (slot == selectedSlot) {
                            {
                                Icon(
                                    imageVector = Icons.Filled.Check,
                                    contentDescription = null,
                                    modifier = Modifier.size(FilterChipDefaults.IconSize),
                                )
                            }
                        } else {
                            null
                        },
                        shape = if (isExpressiveUi) {
                            CircleShape
                        } else {
                            FilterChipDefaults.shape
                        },
                    )
                }
        }
        zipFile.packageSlotPolicy.takeIf(String::isNotBlank)?.let { policy ->
            InfoRow(
                label = resources.getString(R.string.partition_ak3_package_slot_policy),
                value = policy,
            )
        }
    }
}

@Composable
private fun Ak3MkbootfsOption(
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    val resources = LocalResources.current
    val title = resources.getString(R.string.partition_ak3_use_mkbootfs)
    val summary = resources.getString(R.string.partition_ak3_use_mkbootfs_summary)

    if (isExpressiveUi) {
        Card(
            onClick = { onCheckedChange(!checked) },
            enabled = enabled,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.large,
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainer),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(3.dp),
                ) {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Normal,
                    )
                    Text(
                        text = summary,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                YukiSwitch(
                    checked = checked,
                    onCheckedChange = onCheckedChange,
                    enabled = enabled,
                )
            }
        }
    } else {
        HorizontalDivider(modifier = Modifier.padding(vertical = 2.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            YukiSwitch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled,
            )
        }
    }
}

@Composable
fun InstallItemCard(zipFile: ZipFileInfo) {
    val resources = LocalResources.current

    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.elevatedCardColors(
            containerColor = when (zipFile.type) {
                ZipType.MODULE -> MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.3f)
                ZipType.KERNEL -> MaterialTheme.colorScheme.tertiaryContainer.copy(alpha = 0.3f)
                ZipType.UNKNOWN -> MaterialTheme.colorScheme.surfaceVariant
            },
        ),
        elevation = CardDefaults.elevatedCardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(
                    imageVector = when (zipFile.type) {
                        ZipType.MODULE -> Icons.Default.Extension
                        ZipType.KERNEL -> Icons.Default.Memory
                        ZipType.UNKNOWN -> Icons.AutoMirrored.Filled.Help
                    },
                    contentDescription = null,
                    tint = when (zipFile.type) {
                        ZipType.MODULE -> MaterialTheme.colorScheme.primary
                        ZipType.KERNEL -> MaterialTheme.colorScheme.tertiary
                        ZipType.UNKNOWN -> MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    modifier = Modifier.size(20.dp),
                )
                Spacer(modifier = Modifier.width(8.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = zipFile.name.ifEmpty {
                            when (zipFile.type) {
                                ZipType.MODULE -> resources.getString(R.string.unknown_module)
                                ZipType.KERNEL -> resources.getString(R.string.partition_unknown)
                                ZipType.UNKNOWN -> resources.getString(R.string.unknown_file)
                            }
                        },
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        text = when (zipFile.type) {
                            ZipType.MODULE -> resources.getString(R.string.module_package)
                            ZipType.KERNEL -> resources.getString(R.string.partition_ak3_package)
                            ZipType.UNKNOWN -> resources.getString(R.string.unknown_package)
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            val hasDetails = zipFile.version.isNotEmpty() ||
                zipFile.author.isNotEmpty() ||
                zipFile.description.isNotEmpty() ||
                zipFile.supported.isNotEmpty() ||
                zipFile.displayName.isNotEmpty()
            if (hasDetails) {
                Spacer(modifier = Modifier.height(12.dp))
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f),
                    thickness = 0.5.dp,
                )
                Spacer(modifier = Modifier.height(8.dp))

                if (zipFile.displayName.isNotEmpty()) {
                    InfoRow(
                        label = resources.getString(R.string.partition_ak3_package),
                        value = zipFile.displayName,
                    )
                    InfoRow(
                        label = resources.getString(R.string.partition_image_size),
                        value = formatSize(zipFile.size),
                    )
                }
                if (zipFile.version.isNotEmpty()) {
                    InfoRow(
                        label = resources.getString(R.string.version),
                        value = zipFile.version +
                            if (zipFile.versionCode.isNotEmpty()) {
                                " (${zipFile.versionCode})"
                            } else {
                                ""
                            },
                    )
                }
                if (zipFile.author.isNotEmpty()) {
                    InfoRow(
                        label = resources.getString(R.string.author),
                        value = zipFile.author,
                    )
                }
                if (zipFile.description.isNotEmpty() && zipFile.type == ZipType.MODULE) {
                    InfoRow(
                        label = resources.getString(R.string.description),
                        value = zipFile.description,
                    )
                }
                if (zipFile.type == ZipType.KERNEL) {
                    InfoRow(
                        label = resources.getString(R.string.partition_ak3_devices),
                        value = zipFile.supported.ifBlank {
                            resources.getString(R.string.partition_ak3_not_declared)
                        },
                    )
                }
            }
        }
    }
}

@Composable
fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Text(
            text = "$label:",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.widthIn(min = 60.dp),
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.weight(1f),
        )
    }
}
