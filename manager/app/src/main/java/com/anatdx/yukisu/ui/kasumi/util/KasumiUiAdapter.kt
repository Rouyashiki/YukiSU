package com.anatdx.yukisu.ui.kasumi.util

import android.util.Log
import com.anatdx.yukisu.Natives
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.Locale

object KasumiUiAdapter {
    enum class KasumiStatus { AVAILABLE, NOT_PRESENT, KERNEL_TOO_OLD, MODULE_TOO_OLD }
    data class FeaturesResult(val bitmask: Int, val names: List<String>)
    data class ModuleInfo(val id: String, val name: String, val mode: String, val strategy: String)
    data class ActiveRule(val type: String, val src: String, val target: String? = null, val isUserDefined: Boolean = false,
        val hideState: Int? = null, val hideError: Int = 0, val bypass: Boolean = false)
    data class PartitionInfo(val name: String)
    data class MountStats(
        val totalMounts: Int, val overlayfsMounts: Int,
    )
    data class SystemInfo(
        val kernel: String, val mountBase: String,
        val activeMounts: List<String>, val kasumiModuleIds: List<String>,
        val kasumiMismatch: Boolean, val mismatchMessage: String?,
        val mountStats: MountStats? = null, val detectedPartitions: List<PartitionInfo> = emptyList(),
        val hooks: String = "", val viewsEnabled: Boolean? = null,
    )
    data class StorageInfo(val size: String, val used: String, val avail: String, val percent: String, val type: String)
    data class KagamiConfig(
        val tempdir: String = "",
        val mountsource: String = "KSU", val debug: Boolean = false, val verbose: Boolean = false,
        val fsType: String = "auto", val enableKernelDebug: Boolean = false,
        val enableStealth: Boolean = true, val enableOverlayXattrHide: Boolean = false,
        val enableMountHide: Boolean = false,
        val enableMapsSpoof: Boolean = false, val enableStatfsSpoof: Boolean = false,
        val enableSelinuxFix: Boolean = false,
        val kasumiEnabled: Boolean = true,
        val mirrorPath: String = "", val partitions: List<String> = emptyList(),
        val mountBackend: String = "auto", val overlayfsEnabled: Boolean = true, val magicMountEnabled: Boolean = true,
        val kernelAvailable: Boolean = false, val externalOwner: String = "",
        val original: JSONObject = JSONObject(), val supported: Set<String> = emptySet(),
    )
    data class UiState(
        val version: String, val status: KasumiStatus, val config: KagamiConfig,
        val modules: List<ModuleInfo>, val system: SystemInfo, val storage: StorageInfo,
        val rules: List<ActiveRule>, val features: FeaturesResult?,
    )

    private fun config(raw: JSONObject, names: Set<String>, available: Boolean, externalOwner: String): KagamiConfig {
        val partitions = raw.optJSONArray("partitions")
        return KagamiConfig(
            tempdir = raw.optString("work_dir", "/dev/kagami").takeUnless { it == "/dev/kagami" }.orEmpty(),
            mountsource = raw.optString("mountsource", "KSU"),
            debug = raw.optBoolean("debug"), verbose = raw.optBoolean("verbose"),
            fsType = raw.optString("fs_type", "auto"), enableKernelDebug = raw.optBoolean("enable_kernel_debug"),
            enableStealth = raw.optBoolean("enable_stealth", true),
            enableOverlayXattrHide = raw.optBoolean("enable_overlay_xattr_hide"),
            enableMountHide = raw.optBoolean("enable_mount_hide"),
            enableMapsSpoof = raw.optBoolean("enable_maps_spoof"),
            enableStatfsSpoof = raw.optBoolean("enable_statfs_spoof"),
            enableSelinuxFix = raw.optBoolean("enable_selinux_fix"),
            kasumiEnabled = raw.optBoolean("kasumi_enabled", true), mirrorPath = raw.optString("mirror_dir"),
            partitions = if (partitions == null) emptyList() else (0 until partitions.length()).map(partitions::getString),
            mountBackend = raw.optString("mount_backend", "auto"),
            overlayfsEnabled = raw.optBoolean("overlayfs_enabled", true), magicMountEnabled = raw.optBoolean("magic_mount_enabled", true),
            kernelAvailable = available, externalOwner = externalOwner,
            original = raw, supported = names,
        )
    }

    private fun rules(raw: String, userRules: Set<String>): List<ActiveRule> = raw.lineSequence().filter(String::isNotBlank).mapNotNull { line ->
        val fields = line.trim().split(Regex("\\s+"), limit = 3)
        val type = fields[0].lowercase(Locale.ROOT)
        if (type == "inject" || type == "kasumi") return@mapNotNull null
        val flags = Regex("\\s+(\\d+)$").find(line)?.groupValues?.get(1)?.toIntOrNull() ?: 0
        val path = if (type == "hide") line.substringAfter(' ', "").replace(Regex("\\s+\\d+$"), "") else fields.getOrElse(1) { line }
        val source = fields.getOrNull(2)?.let { if (type == "add" || type == "merge") it.replace(Regex("\\s+-?\\d+$"), "") else it }
        ActiveRule(type, path, if (type in listOf("add", "merge")) source else null,
            type == "hide" && path in userRules, bypass = flags and 1 != 0)
    }.toList()

    private fun userRules(snapshot: KasumiManager.Snapshot, savedBypass: Boolean = true): List<ActiveRule> {
        val registered = snapshot.system.optJSONArray("user_hide") ?: return snapshot.hideRules.map {
            ActiveRule("hide", it, isUserDefined = true, bypass = savedBypass && it in snapshot.hideBypasses)
        }
        val live = (0 until registered.length()).map(registered::getJSONObject).associateBy { it.getString("path") }
        return (snapshot.hideRules + live.keys).distinct().map { path ->
            val state = live[path]
            ActiveRule("hide", path, isUserDefined = true,
                hideState = when {
                    state == null -> -1
                    state.optInt("management") == 1 -> 4
                    else -> state.optInt("binding")
                }, hideError = state?.optInt("error") ?: 0,
                bypass = if (savedBypass && path in snapshot.hideRules) path in snapshot.hideBypasses else state?.optBoolean("bypass") == true)
        }
    }

    private fun activeRules(snapshot: KasumiManager.Snapshot): List<ActiveRule> {
        val legacy = rules(snapshot.system.optString("rules"), if (snapshot.system.has("user_hide")) emptySet() else snapshot.hideRules.toSet())
        return if (snapshot.system.has("user_hide")) legacy + userRules(snapshot, savedBypass = false) else legacy
    }

    private fun size(value: Long): String {
        val units = listOf("B", "KiB", "MiB", "GiB", "TiB")
        var amount = value.toDouble()
        var unit = 0
        while (amount >= 1024 && unit < units.lastIndex) { amount /= 1024; unit++ }
        return String.format(Locale.ROOT, "%.1f %s", amount, units[unit])
    }

    suspend fun load(): UiState = withContext(Dispatchers.IO) {
        val snapshot = KasumiManager.snapshot()
        val sys = snapshot.system
        val protocol = sys.optInt("kernel_version")
        val expectedProtocol = sys.getInt("protocol_version")
        val status = when {
            snapshot.mounts.available -> KasumiStatus.AVAILABLE
            protocol == 0 -> KasumiStatus.NOT_PRESENT
            protocol < expectedProtocol -> KasumiStatus.KERNEL_TOO_OLD
            else -> KasumiStatus.MODULE_TOO_OLD
        }
        val features = sys.optJSONObject("features") ?: JSONObject()
        val names = features.optJSONArray("names")
        val supported = if (names == null) emptyList() else (0 until names.length()).map(names::getString)
        val viewConfig = config(snapshot.config, supported.toSet(), snapshot.mounts.available, snapshot.mounts.externalOwner)
        val base = sys.optString("mount_base")
        val disk = try {
            val path = base.ifBlank { "/data/adb/ksu/kagami" }
            JSONObject(Natives.kasumiStorageInfo(path.toByteArray(Charsets.UTF_8)).toString(Charsets.UTF_8))
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            Log.w("KasumiUiAdapter", "Storage information unavailable", error)
            JSONObject()
        }
        val total = disk.optLong("size", -1)
        val avail = disk.optLong("avail", -1)
        val used = (total - avail).coerceAtLeast(0)
        val storage = if (total < 0) StorageInfo("—", "—", "—", "—", "unknown") else StorageInfo(
            size(total), size(used), size(avail), "${if (total == 0L) 0 else (100.0 * used / total).toInt()}%", disk.optString("type", "unknown"),
        )
        val active = sys.optJSONArray("active_mounts")
        val mounts = if (active == null) emptyList() else (0 until active.length()).map(active::getString)
        val parts = sys.optJSONArray("detectedPartitions")
        val partitions = if (parts == null) emptyList() else (0 until parts.length()).map {
            PartitionInfo(parts.getJSONObject(it).getString("name"))
        }
        val rawRules = sys.optString("rules")
        val ids = Regex("/data/adb/modules/([^/\\s]+)").findAll(rawRules).map { it.groupValues[1] }.distinct().toList()
        val stats = sys.optJSONObject("mountStats")
        UiState(
            protocol.toString(), status, viewConfig,
            snapshot.mounts.modules.values.map { ModuleInfo(it.id, it.name, it.mode, it.strategy) },
            SystemInfo(sys.optString("kernel"), base, mounts, ids,
                protocol != 0 && protocol != expectedProtocol, null,
                stats?.let { MountStats(it.optInt("total_mounts"), it.optInt("overlayfs_mounts")) }, partitions,
                sys.optString("hooks"), sys.optBoolean("enabled")),
            storage, activeRules(snapshot), FeaturesResult(features.optInt("bitmask"), supported),
        )
    }

    private suspend fun success(action: suspend () -> Unit): Boolean = try {
        action(); true
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        Log.w("KasumiUiAdapter", "Controller operation failed", error)
        false
    }

    suspend fun saveConfig(value: KagamiConfig): KasumiManager.ConfigSaveResult {
        val old = config(value.original, value.supported, value.kernelAvailable, value.externalOwner)
        val updates = JSONObject()
        fun change(key: String, before: Any, after: Any) { if (before != after) updates.put(key, after) }
        if (old.tempdir != value.tempdir) updates.put("work_dir", value.tempdir.ifEmpty { "/dev/kagami" })
        change("mountsource", old.mountsource, value.mountsource)
        change("debug", old.debug, value.debug)
        change("verbose", old.verbose, value.verbose)
        change("fs_type", old.fsType, value.fsType)
        change("enable_kernel_debug", old.enableKernelDebug, value.enableKernelDebug)
        change("enable_stealth", old.enableStealth, value.enableStealth)
        change("enable_overlay_xattr_hide", old.enableOverlayXattrHide, value.enableOverlayXattrHide)
        change("enable_mount_hide", old.enableMountHide, value.enableMountHide)
        change("enable_maps_spoof", old.enableMapsSpoof, value.enableMapsSpoof)
        change("enable_statfs_spoof", old.enableStatfsSpoof, value.enableStatfsSpoof)
        change("enable_selinux_fix", old.enableSelinuxFix, value.enableSelinuxFix)
        change("kasumi_enabled", old.kasumiEnabled, value.kasumiEnabled)
        change("mirror_dir", old.mirrorPath, value.mirrorPath)
        change("mount_backend", old.mountBackend, value.mountBackend)
        change("overlayfs_enabled", old.overlayfsEnabled, value.overlayfsEnabled)
        change("magic_mount_enabled", old.magicMountEnabled, value.magicMountEnabled)
        if (old.partitions != value.partitions) updates.put("partitions", org.json.JSONArray(value.partitions))
        if (updates.length() == 0) return KasumiManager.ConfigSaveResult(false, false, noChanges = true)
        return KasumiManager.saveAndApplyConfig(updates, value.kernelAvailable)
    }

    suspend fun getActiveRules(): List<ActiveRule> = withContext(Dispatchers.IO) {
        val snapshot = KasumiManager.snapshot()
        activeRules(snapshot)
    }
    suspend fun listUserHideRules(): List<String> = KasumiManager.snapshot().hideRules
    suspend fun userHideStates(): List<ActiveRule> {
        val snapshot = KasumiManager.snapshot()
        return userRules(snapshot).filter { it.src in snapshot.hideRules }
    }
    suspend fun retryUserHide(path: String) = success { KasumiManager.retryUserHide(path) }
    suspend fun addUserHideRule(path: String, bypass: Boolean = false) = success { KasumiManager.saveHide(path, bypass = bypass) }
    suspend fun removeUserHideRule(path: String) = success { KasumiManager.saveHide(path, true) }
    suspend fun clearAllRules() = success { KasumiManager.clearRules() }
    suspend fun clearMapsRules() = success { KasumiManager.clearMapsRules() }
    suspend fun addMapsRule(ti: Long, td: Long, si: Long, sd: Long, path: String) = success {
        KasumiManager.addMapsRule(listOf(ti, td, si, sd).map { it.toULong().toString() }, path)
    }
    suspend fun readLog(): String = KasumiManager.readLog()
    suspend fun readKernelLog(): String = withContext(Dispatchers.IO) {
        filterKasumiKernelLog(Natives.kasumiReadLog(true).toString(Charsets.UTF_8))
    }
    suspend fun clearLog() = success { KasumiManager.clearLog() }
    suspend fun retryApply() = success { KasumiManager.applyConfig() }
    suspend fun syncPartitionsWithDaemon(): KagamiConfig? = try {
        KasumiManager.syncPartitions(); load().config
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        null
    }
}
