package org.rouyashiki.yukisu.ui.util

import android.content.Context
import android.os.Build
import android.system.Os
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.ui.screen.getManagerVersion
import com.topjohnwu.superuser.Shell
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import java.util.UUID

private const val YUKIZYGISK_STATE_DIR = "/data/adb/ksu/yukizygisk"
private const val YUKIZYGISK_CONFIG_PATH = "$YUKIZYGISK_STATE_DIR/yzconfig.json"
private const val YUKIZYGISK_DIAGNOSTICS_DIR = "$YUKIZYGISK_STATE_DIR/diagnostics"
private const val YUKIZYGISK_LEGACY_LOG_DIR = "$YUKIZYGISK_STATE_DIR/log"
private const val YUKIZYGISK_REPORT_ARCHIVE_NAME = "yukizygisk.tar.gz"

private val yukiZygiskLegacyLogNames = listOf(
    "zygiskd64.log",
    "zygiskd64.old.log",
    "zygiskd32.log",
    "zygiskd32.old.log",
)

internal data class YukiZygiskReportEvidence(
    val currentDiagnostics: Boolean = false,
    val oldDiagnostics: Boolean = false,
    val legacyLogs: Boolean = false,
)

internal fun shouldCollectYukiZygiskReport(
    featureEnabled: Boolean?,
    evidence: YukiZygiskReportEvidence,
): Boolean = featureEnabled == true ||
    evidence.currentDiagnostics ||
    evidence.oldDiagnostics ||
    evidence.legacyLogs

internal fun outerBugreportArtifactReference(name: String, collected: Boolean): String =
    if (collected) "outer bugreport/$name" else "unavailable"

private fun hasNonEmptyRegularFile(shell: Shell, path: String): Boolean = shell.newJob()
    .add(
        "file=${shellArg(path)}; " +
            "[ ! -L \"\$file\" ] && [ -f \"\$file\" ] && [ -s \"\$file\" ]",
    )
    .exec()
    .isSuccess

private fun hasYukiZygiskGenerationEvidence(shell: Shell, path: String): Boolean = shell.newJob()
    .add(
        "generation=${shellArg(path)}; found=0; " +
            "if [ ! -L \"\$generation\" ] && [ -d \"\$generation\" ]; then " +
            "for file in \"\$generation/evidence\" \"\$generation/early_linker.json\" " +
            "\"\$generation/linker64.json\" \"\$generation/linker32.json\" " +
            "\"\$generation/capture.json\" \"\$generation/logs/\"* " +
            "\"\$generation/tombstones/\"* \"\$generation/pstore/\"*; do " +
            "if [ ! -L \"\$file\" ] && [ -f \"\$file\" ] && [ -s \"\$file\" ]; then " +
            "found=1; break; fi; done; fi; [ \"\$found\" -eq 1 ]",
    )
    .exec()
    .isSuccess

private fun findYukiZygiskReportEvidence(shell: Shell): YukiZygiskReportEvidence {
    val legacyLogs = yukiZygiskLegacyLogNames.any { name ->
        hasNonEmptyRegularFile(shell, "$YUKIZYGISK_LEGACY_LOG_DIR/$name")
    }
    return YukiZygiskReportEvidence(
        currentDiagnostics = hasYukiZygiskGenerationEvidence(
            shell,
            "$YUKIZYGISK_DIAGNOSTICS_DIR/current",
        ),
        oldDiagnostics = hasYukiZygiskGenerationEvidence(
            shell,
            "$YUKIZYGISK_DIAGNOSTICS_DIR/old",
        ),
        legacyLogs = legacyLogs,
    )
}

private fun copyYukiZygiskDiagnostics(shell: Shell, reportDir: File): Boolean {
    val destination = File(reportDir, "diagnostics")
    return shell.newJob()
        .add(
            "source=${shellArg(YUKIZYGISK_DIAGNOSTICS_DIR)}; " +
                "destination=${shellArg(destination.absolutePath)}; " +
                "[ ! -L \"\$source\" ] && [ -d \"\$source\" ] && " +
                "mkdir -p \"\$destination\" && cp -a \"\$source/.\" \"\$destination/\"",
        )
        .exec()
        .isSuccess
}

private fun copyLegacyYukiZygiskLogs(shell: Shell, reportDir: File): Boolean {
    val destination = File(reportDir, "logs")
    val sources = yukiZygiskLegacyLogNames.joinToString(" ") { name ->
        shellArg("$YUKIZYGISK_LEGACY_LOG_DIR/$name")
    }
    return shell.newJob()
        .add(
            "destination=${shellArg(destination.absolutePath)}; " +
                "mkdir -p \"\$destination\"; copied=0; " +
                "for source in $sources; do " +
                "if [ ! -L \"\$source\" ] && [ -f \"\$source\" ] && [ -s \"\$source\" ]; then " +
                "cp -p \"\$source\" \"\$destination\"/ && copied=1; " +
                "fi; done; [ \"\$copied\" -eq 1 ]",
        )
        .exec()
        .isSuccess
}

private fun deduplicateCrashFiles(
    shell: Shell,
    reportDir: File,
    archiveFile: File,
    directoryName: String,
): Int {
    if (!archiveFile.isFile || archiveFile.length() == 0L) return 0
    require(directoryName == "tombstones" || directoryName == "pstore")

    val diagnosticsDir = File(reportDir, "diagnostics")
    val scratchFile = File(
        checkNotNull(reportDir.parentFile),
        ".yukizygisk_compare_${UUID.randomUUID()}",
    )
    val result = shell.newJob()
        .add(
            "archive=${shellArg(archiveFile.absolutePath)}; " +
                "diagnostics=${shellArg(diagnosticsDir.absolutePath)}; " +
                "scratch=${shellArg(scratchFile.absolutePath)}; removed=0; " +
                "for generation in current old; do " +
                "directory=\"\$diagnostics/\$generation/$directoryName\"; " +
                "[ ! -L \"\$directory\" ] && [ -d \"\$directory\" ] || continue; " +
                "for stored in \"\$directory\"/*; do " +
                "[ ! -L \"\$stored\" ] && [ -f \"\$stored\" ] || continue; " +
                "name=\${stored##*/}; " +
                "if tar -xOzf \"\$archive\" \"./\$name\" > \"\$scratch\" 2>/dev/null && " +
                "cmp -s \"\$scratch\" \"\$stored\"; then " +
                "if rm -f -- \"\$stored\"; then removed=\$((removed + 1)); fi; " +
                "fi; done; rmdir \"\$directory\" 2>/dev/null || true; done; " +
                "rm -f -- \"\$scratch\"; printf '%s\\n' \"\$removed\"",
        )
        .exec()
    return if (result.isSuccess) result.out.lastOrNull()?.trim()?.toIntOrNull() ?: 0 else 0
}

private fun archiveYukiZygiskReport(shell: Shell, reportDir: File, bugreportDir: File) {
    val cacheDir = checkNotNull(bugreportDir.parentFile)
    val archiveFile = File(bugreportDir, YUKIZYGISK_REPORT_ARCHIVE_NAME)
    val temporaryArchive = File.createTempFile(".yukizygisk_", ".tar.gz", cacheDir)
    var archiveReady = false
    try {
        val result = shell.newJob()
            .add(
                "tar czf ${shellArg(temporaryArchive.absolutePath)} " +
                    "-C ${shellArg(reportDir.absolutePath)} . && " +
                    "tar tzf ${shellArg(temporaryArchive.absolutePath)} >/dev/null && " +
                    "chmod 0644 ${shellArg(temporaryArchive.absolutePath)} && " +
                    "mv -f -- ${shellArg(temporaryArchive.absolutePath)} " +
                    shellArg(archiveFile.absolutePath),
            )
            .exec()
        archiveReady = result.isSuccess && archiveFile.isFile && archiveFile.length() > 0L
        check(archiveReady) { "Failed to create YukiZygisk bugreport archive" }
    } finally {
        val cleanup = buildList {
            add(temporaryArchive)
            if (!archiveReady) add(archiveFile)
        }.joinToString(" ") { shellArg(it.absolutePath) }
        shell.newJob().add("rm -rf -- $cleanup").exec()
    }
}

private fun populateYukiZygiskReport(
    shell: Shell,
    reportDir: File,
    bugreportDir: File,
    featureEnabled: Boolean?,
    evidence: YukiZygiskReportEvidence,
    tombstonesCollected: Boolean,
    pstoreCollected: Boolean,
) {
    val statusFile = File(reportDir, "status.json")
    val configFile = File(reportDir, "config.json")

    val statusCollected = shell.newJob()
        .add("${ksudCmd("yzctl status --json")} > ${shellArg(statusFile.absolutePath)}")
        .exec()
        .isSuccess && statusFile.isFile && statusFile.length() > 0L
    val configCollected = shell.newJob()
        .add("cp $YUKIZYGISK_CONFIG_PATH ${shellArg(configFile.absolutePath)}")
        .exec()
        .isSuccess && configFile.isFile && configFile.length() > 0L
    val diagnosticsCollected = copyYukiZygiskDiagnostics(shell, reportDir)
    val legacyLogsCollected = copyLegacyYukiZygiskLogs(shell, reportDir)
    val tombstoneDuplicatesRemoved = if (tombstonesCollected) {
        deduplicateCrashFiles(
            shell,
            reportDir,
            File(bugreportDir, "tombstones.tar.gz"),
            "tombstones",
        )
    } else {
        0
    }
    val pstoreDuplicatesRemoved = if (pstoreCollected) {
        deduplicateCrashFiles(
            shell,
            reportDir,
            File(bugreportDir, "pstore.tar.gz"),
            "pstore",
        )
    } else {
        0
    }

    val results = linkedMapOf(
        "status.json" to statusCollected,
        "config.json" to configCollected,
        "diagnostics" to diagnosticsCollected,
        "legacy-logs" to legacyLogsCollected,
    )

    PrintWriter(FileWriter(File(reportDir, "collection.txt"))).use { writer ->
        val featureStatus = when (featureEnabled) {
            true -> "enabled"
            false -> "disabled"
            null -> "unavailable"
        }
        writer.println("feature: $featureStatus")
        writer.println("diagnostics/current/evidence: ${if (evidence.currentDiagnostics) "present" else "absent"}")
        writer.println("diagnostics/old/evidence: ${if (evidence.oldDiagnostics) "present" else "absent"}")
        writer.println("legacy-logs/evidence: ${if (evidence.legacyLogs) "present" else "absent"}")
        results.forEach { (artifact, collected) ->
            writer.println("$artifact: ${if (collected) "collected" else "unavailable"}")
        }
        writer.println(
            "tombstones: " +
                outerBugreportArtifactReference("tombstones.tar.gz", tombstonesCollected),
        )
        writer.println("tombstone-duplicates-removed: $tombstoneDuplicatesRemoved")
        writer.println(
            "pstore: " + outerBugreportArtifactReference("pstore.tar.gz", pstoreCollected),
        )
        writer.println("pstore-duplicates-removed: $pstoreDuplicatesRemoved")
    }
}

private fun collectYukiZygiskReport(
    shell: Shell,
    bugreportDir: File,
    featureEnabled: Boolean?,
    evidence: YukiZygiskReportEvidence,
    tombstonesCollected: Boolean,
    pstoreCollected: Boolean,
) {
    val reportDir = File(
        checkNotNull(bugreportDir.parentFile),
        "yukizygisk_${UUID.randomUUID()}",
    )
    check(reportDir.mkdir()) { "Failed to create YukiZygisk staging directory" }
    try {
        populateYukiZygiskReport(
            shell,
            reportDir,
            bugreportDir,
            featureEnabled,
            evidence,
            tombstonesCollected,
            pstoreCollected,
        )
        archiveYukiZygiskReport(shell, reportDir, bugreportDir)
    } finally {
        shell.newJob().add("rm -rf -- ${shellArg(reportDir.absolutePath)}").exec()
    }
}

private fun buildBugreportFile(context: Context, bugreportDir: File, shell: Shell): File {
    val processFile = File(bugreportDir, "process.txt")
    val dmesgFile = File(bugreportDir, "dmesg.txt")
    val logcatFile = File(bugreportDir, "logcat.txt")
    val tombstonesFile = File(bugreportDir, "tombstones.tar.gz")
    val dropboxFile = File(bugreportDir, "dropbox.tar.gz")
    val pstoreFile = File(bugreportDir, "pstore.tar.gz")
    val diagFile = File(bugreportDir, "diag.tar.gz")
    val oplusFile = File(bugreportDir, "oplus.tar.gz")
    val bootlogFile = File(bugreportDir, "bootlog.tar.gz")
    val mountsFile = File(bugreportDir, "mounts.txt")
    val fileSystemsFile = File(bugreportDir, "filesystems.txt")
    val adbFileTree = File(bugreportDir, "adb_tree.txt")
    val adbFileDetails = File(bugreportDir, "adb_details.txt")
    val ksuFileSize = File(bugreportDir, "ksu_size.txt")
    val appListFile = File(bugreportDir, "packages.txt")
    val propFile = File(bugreportDir, "props.txt")
    val allowListFile = File(bugreportDir, "allowlist.bin")
    val procModules = File(bugreportDir, "proc_modules.txt")
    val bootConfig = File(bugreportDir, "boot_config.txt")
    val kernelConfig = File(bugreportDir, "defconfig.gz")
    val kallsyms = File(bugreportDir, "kallsyms.txt")

    val yukiZygiskEnabled = getFeatureValueOrNull(Natives.FEATURE_YUKIZYGISK)
    val yukiZygiskEvidence = findYukiZygiskReportEvidence(shell)

    // busybox ps has very few features for embed devices
    shell.newJob().add("toybox ps -T -A -w -o PID,TID,UID,COMM,CMDLINE,CMD,LABEL,STAT,WCHAN > ${shellArg(processFile.absolutePath)}").exec()
    shell.newJob().add("dmesg -r > ${shellArg(dmesgFile.absolutePath)}").exec()
    shell.newJob().add("logcat -b all -v uid -d > ${shellArg(logcatFile.absolutePath)}").exec()
    val tombstonesCollected = shell.newJob()
        .add("tar -czf ${shellArg(tombstonesFile.absolutePath)} -C /data/tombstones .")
        .exec()
        .isSuccess && tombstonesFile.isFile && tombstonesFile.length() > 0L
    shell.newJob().add("tar -czf ${shellArg(dropboxFile.absolutePath)} -C /data/system/dropbox .").exec()
    val pstoreCollected = shell.newJob()
        .add("tar -czf ${shellArg(pstoreFile.absolutePath)} -C /sys/fs/pstore .")
        .exec()
        .isSuccess && pstoreFile.isFile && pstoreFile.length() > 0L
    shell.newJob().add("tar -czf ${shellArg(diagFile.absolutePath)} -C /data/vendor/diag . --exclude=./minidump.gz").exec()
    shell.newJob().add("tar -czf ${shellArg(oplusFile.absolutePath)} -C /mnt/oplus/op2/media/log/boot_log/ .").exec()
    shell.newJob().add("tar -czf ${shellArg(bootlogFile.absolutePath)} -C /data/adb/ksu/log .").exec()

    shell.newJob().add("cat /proc/1/mountinfo > ${shellArg(mountsFile.absolutePath)}").exec()
    shell.newJob().add("cat /proc/filesystems > ${shellArg(fileSystemsFile.absolutePath)}").exec()
    shell.newJob().add("busybox tree /data/adb > ${shellArg(adbFileTree.absolutePath)}").exec()
    shell.newJob().add("ls -alRZ /data/adb > ${shellArg(adbFileDetails.absolutePath)}").exec()
    shell.newJob().add("du -sh /data/adb/ksu/* > ${shellArg(ksuFileSize.absolutePath)}").exec()
    shell.newJob().add("cp /data/system/packages.list ${shellArg(appListFile.absolutePath)}").exec()
    shell.newJob().add("getprop > ${shellArg(propFile.absolutePath)}").exec()
    shell.newJob().add("cp /data/adb/ksu/.allowlist ${shellArg(allowListFile.absolutePath)}").exec()
    shell.newJob().add("cp /proc/modules ${shellArg(procModules.absolutePath)}").exec()
    shell.newJob().add("cp /proc/bootconfig ${shellArg(bootConfig.absolutePath)}").exec()
    shell.newJob().add("cp /proc/config.gz ${shellArg(kernelConfig.absolutePath)}").exec()
    shell.newJob().add("ORIG=\$(cat /proc/sys/kernel/kptr_restrict); echo 1 > /proc/sys/kernel/kptr_restrict; cat /proc/kallsyms > ${shellArg(kallsyms.absolutePath)}; echo \$ORIG > /proc/sys/kernel/kptr_restrict").exec()

    if (shouldCollectYukiZygiskReport(yukiZygiskEnabled, yukiZygiskEvidence)) {
        collectYukiZygiskReport(
            shell,
            bugreportDir,
            yukiZygiskEnabled,
            yukiZygiskEvidence,
            tombstonesCollected,
            pstoreCollected,
        )
    }

    val selinux = getSELinuxLabel()

    val buildInfo = File(bugreportDir, "basic.txt")
    PrintWriter(FileWriter(buildInfo)).use { pw ->
        pw.println("Kernel: ${System.getProperty("os.version")}")
        pw.println("BRAND: " + Build.BRAND)
        pw.println("MODEL: " + Build.MODEL)
        pw.println("PRODUCT: " + Build.PRODUCT)
        pw.println("MANUFACTURER: " + Build.MANUFACTURER)
        pw.println("SDK: " + Build.VERSION.SDK_INT)
        pw.println("PREVIEW_SDK: " + Build.VERSION.PREVIEW_SDK_INT)
        pw.println("FINGERPRINT: " + Build.FINGERPRINT)
        pw.println("DEVICE: " + Build.DEVICE)
        pw.println("Manager: " + getManagerVersion(context))
        pw.println("SELinux: $selinux")

        val uname = Os.uname()
        pw.println("EffectiveKernelRelease: ${uname.release}")
        getUtsViewOriginalReleaseForLog()?.takeIf { it.isNotBlank() }?.let {
            pw.println("OriginalKernelRelease: $it")
        }
        pw.println("KernelVersion: ${uname.version}")
        pw.println("Machine: ${uname.machine}")
        pw.println("Nodename: ${uname.nodename}")
        pw.println("Sysname: ${uname.sysname}")

        val ksuKernel = Natives.version
        pw.println("KernelSU: $ksuKernel")
        val safeMode = Natives.isSafeMode
        pw.println("SafeMode: $safeMode")
        val yukiZygiskStatus = when (yukiZygiskEnabled) {
            true -> "enabled"
            false -> "disabled"
            null -> "unavailable"
        }
        pw.println("YukiZygisk: $yukiZygiskStatus")
        pw.println("LKM: true")
    }

    val modulesFile = File(bugreportDir, "modules.json")
    modulesFile.writeText(listModules())

    val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm_ss_SSS")
    val current = LocalDateTime.now().format(formatter)

    val targetFile = File.createTempFile("YukiSU_bugreport_${current}_", ".tar.gz", context.cacheDir)

    val archive = shell.newJob()
        .add("tar czf ${shellArg(targetFile.absolutePath)} -C ${shellArg(bugreportDir.absolutePath)} . && chmod 0644 ${shellArg(targetFile.absolutePath)}")
        .exec()
    if (!archive.isSuccess || !targetFile.isFile || targetFile.length() == 0L) {
        shell.newJob()
            .add("rm -rf -- ${shellArg(bugreportDir.absolutePath)} ${shellArg(targetFile.absolutePath)}")
            .exec()
        error("Failed to create bugreport archive")
    }
    return targetFile
}

@Synchronized
fun getBugreportFile(context: Context): File {
    val shell = getRootShell(true)
    val bugreportDir = File(context.cacheDir, "bugreport_${UUID.randomUUID()}")
    check(bugreportDir.mkdir()) { "Failed to create bugreport staging directory" }
    return try {
        buildBugreportFile(context, bugreportDir, shell)
    } finally {
        shell.newJob().add("rm -rf -- ${shellArg(bugreportDir.absolutePath)}").exec()
    }
}
