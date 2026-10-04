package ui.screen.yukizygisk

import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Article
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.filled.Adb
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.outlined.Cancel
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material.icons.outlined.VisibilityOff
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.ButtonGroupDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.ToggleButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.YukiIcon
import com.anatdx.yukisu.ui.component.YukiAlertDialog
import com.anatdx.yukisu.ui.theme.CardConfig
import com.anatdx.yukisu.ui.theme.ExpressiveListGroupMinHeight
import com.anatdx.yukisu.ui.util.rememberSnackbarController
import com.anatdx.yukisu.ui.util.execKsud
import com.anatdx.yukisu.ui.util.getYukiZygiskStatusJson
import com.anatdx.yukisu.ui.util.ksudReadString
import com.anatdx.yukisu.ui.theme.getCardColors
import com.anatdx.yukisu.ui.theme.getCardElevation
import com.anatdx.yukisu.ui.theme.isExpressiveUi
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SwitchSettingItem

private const val TAG = "YukiZygiskScreen"
private val yzConfigWriteMutex = Mutex()

data class YzConfig(
    val yukilinker: Boolean = true,
    val anonymousMemory: Boolean = true,
    val earlyLoad: Boolean = false,
    val denylistMode: Int = 0,
    val dmesgLog: Boolean = false,
    val crashProtection: Boolean = true,
)

private suspend fun readYzConfig(): YzConfig = withContext(Dispatchers.IO) {
    val raw = runCatching {
        ksudReadString("yzctl config get")
    }.getOrNull()
    if (raw.isNullOrBlank()) return@withContext YzConfig()
    try {
        val o = JSONObject(raw)
        YzConfig(
            yukilinker = o.optBoolean("yukilinker", true),
            anonymousMemory = o.optBoolean("anonymous_memory", true),
            earlyLoad = o.optBoolean("early_load", false),
            denylistMode = o.optInt("denylist_mode", 0),
            dmesgLog = o.optBoolean("dmesg_log", false),
            crashProtection = o.optBoolean("crash_protection", true),
        )
    } catch (_: Exception) {
        YzConfig()
    }
}

private suspend fun writeYzConfig(cfg: YzConfig): Boolean = yzConfigWriteMutex.withLock {
    withContext(Dispatchers.IO) {
        runCatching {
            check(execKsud(
                "yzctl config set yukilinker ${cfg.yukilinker}" +
                    " anonymous_memory ${cfg.anonymousMemory} early_load ${cfg.earlyLoad}" +
                    " denylist_mode ${cfg.denylistMode} dmesg_log ${cfg.dmesgLog}" +
                    " crash_protection ${cfg.crashProtection}"
            ))
        }.onFailure { Log.e(TAG, "Failed to apply YukiZygisk configuration", it) }.isSuccess
    }
}

private enum class MonitorState {
    Injected,
    Unsupported32,
    Crashed,
    Failed,
    Unknown,
}

private enum class ProcessState {
    Running,
    Exited,
    Unknown,
}

private enum class NativeMonitorMode {
    Module,
    Process,
}

private data class MonitorDialogState(
    val title: String,
    val message: String,
)

private fun parseMonitorState(value: String): MonitorState = when (value) {
    "injected" -> MonitorState.Injected
    "unsupported32" -> MonitorState.Unsupported32
    "crashed" -> MonitorState.Crashed
    "safemode" -> MonitorState.Crashed
    "failed" -> MonitorState.Failed
    else -> MonitorState.Unknown
}

private fun parseInjectionState(record: JSONObject): MonitorState =
    parseMonitorState(record.optString("injection_state", record.optString("state", "unknown")))

private fun parseProcessState(record: JSONObject): ProcessState =
    when (record.optString("process_state", "")) {
        "running" -> ProcessState.Running
        "exited" -> ProcessState.Exited
        "" -> when {
            record.optString("state") == "exited" -> ProcessState.Exited
            record.optInt("pid", 0) > 0 && record.optString("state") in
                setOf("detected", "redirected", "prepared", "injected", "failed", "safemode") ->
                ProcessState.Running
            else -> ProcessState.Unknown
        }
        else -> ProcessState.Unknown
    }

private data class ZygoteMonitorEntry(
    val pid: Int,
    val name: String,
    val abi: String,
    val state: MonitorState,
    val generation: Int = 0,
    val modules: List<String> = emptyList(),
    val moduleMonitorAvailable: Boolean = false,
)

private data class NativeModuleEntry(
    val name: String,
    val id: String,
    val targetType: String,
    val target: String,
    val state: MonitorState,
)

private data class NativeInjection(
    val pid: Int,
    val generation: Int,
    val process: String,
    val module: String,
    val targetType: String,
    val target: String,
    val abi: String,
    val state: MonitorState,
    val processState: ProcessState,
)

private data class NativeProcessEntry(
    val pid: Int,
    val generation: Int,
    val process: String,
    val abi: String,
    val modules: List<String>,
    val state: MonitorState,
    val processState: ProcessState,
)

private data class NativeProcessIdentity(
    val pid: Int,
    val generation: Int,
    val process: String,
    val abi: String,
)

private data class NativeModuleMonitorEntry(
    val name: String,
    val id: String,
    val scopes: List<NativeModuleScope>,
    val state: MonitorState,
    val crashEvidence: List<CrashEvidence>,
)

private data class NativeModuleScope(
    val targetType: String,
    val target: String,
    val state: MonitorState,
    val targets: List<NativeModuleTarget>,
)

private data class NativeModuleTarget(
    val process: String,
    val pid: Int,
    val generation: Int,
    val abi: String,
    val state: MonitorState,
    val processState: ProcessState,
)

private data class CrashEvidence(
    val module: String,
    val process: String,
    val tombstone: String,
    val frame: String,
    val zygotePid: Int,
    val generation: Int,
    val pid: Int,
    val abi: String,
    val timestamp: String,
)

private fun nativeProcessDisplayName(value: String): String =
    value.substringAfterLast('/').ifBlank { value }

private fun aggregateMonitorState(states: List<MonitorState>): MonitorState = when {
    MonitorState.Crashed in states -> MonitorState.Crashed
    MonitorState.Failed in states -> MonitorState.Failed
    MonitorState.Injected in states -> MonitorState.Injected
    MonitorState.Unsupported32 in states -> MonitorState.Unsupported32
    else -> MonitorState.Unknown
}

private fun processStateLabel(state: ProcessState): Int = when (state) {
    ProcessState.Running -> R.string.yukizygisk_process_running
    ProcessState.Exited -> R.string.yukizygisk_process_exited
    ProcessState.Unknown -> R.string.yukizygisk_process_unknown
}

private fun injectionStateLabel(state: MonitorState): Int = when (state) {
    MonitorState.Injected -> R.string.yukizygisk_native_scope_injected_no_process
    MonitorState.Unsupported32 -> R.string.yukizygisk_native_scope_unsupported
    MonitorState.Crashed -> R.string.yukizygisk_native_scope_crashed
    MonitorState.Failed -> R.string.yukizygisk_native_scope_failed
    MonitorState.Unknown -> R.string.yukizygisk_native_injection_unknown
}

private fun buildNativeModuleRows(
    modules: List<NativeModuleEntry>,
    injections: List<NativeInjection>,
    crashEvidence: List<CrashEvidence>,
): List<NativeModuleMonitorEntry> {
    val injectionsByModule = injections.groupBy { it.module }
    return modules.groupBy { it.id }.map { (id, entries) ->
        val scopes = entries.groupBy { it.targetType to it.target }.map { (_, scopeEntries) ->
            val entry = scopeEntries.first()
            val targets = injectionsByModule[id].orEmpty()
                .filter {
                    it.targetType == entry.targetType && it.target == entry.target && it.pid > 0
                }
                .map {
                    NativeModuleTarget(
                        process = nativeProcessDisplayName(it.process.ifBlank { it.target }),
                        pid = it.pid,
                        generation = it.generation,
                        abi = it.abi,
                        state = it.state,
                        processState = it.processState,
                    )
                }
                .distinctBy { NativeProcessIdentity(it.pid, it.generation, it.process, it.abi) }
                .sortedWith(compareBy<NativeModuleTarget> { it.process }
                    .thenBy { it.pid }.thenBy { it.generation })
            NativeModuleScope(
                targetType = entry.targetType,
                target = entry.target,
                state = aggregateMonitorState(scopeEntries.map { it.state } + targets.map { it.state }),
                targets = targets,
            )
        }
        NativeModuleMonitorEntry(
            name = entries.first().name,
            id = id,
            scopes = scopes,
            state = aggregateMonitorState(scopes.map { it.state }),
            crashEvidence = crashEvidence.filter { it.module == id },
        )
    }
}

private fun buildNativeProcessRows(injections: List<NativeInjection>): List<NativeProcessEntry> =
    injections.groupBy { NativeProcessIdentity(it.pid, it.generation, it.process, it.abi) }
        .map { (_, rows) ->
            val first = rows.first()
            NativeProcessEntry(
                pid = first.pid,
                generation = first.generation,
                process = nativeProcessDisplayName(first.process.ifEmpty { first.target }),
                abi = first.abi,
                modules = rows.map { it.module }.distinct(),
                state = aggregateMonitorState(rows.map { it.state }),
                processState = rows.map { it.processState }.distinct().singleOrNull()
                    ?: ProcessState.Unknown,
            )
        }
        .sortedWith(compareBy<NativeProcessEntry> { it.process }
            .thenBy { it.pid }.thenBy { it.generation })

private data class YzStatus(
    val zygotes: List<ZygoteMonitorEntry>,
    val modules: List<String>,
    val runtime: List<RuntimeMonitorEntry>,
    val nativeModules: List<NativeModuleEntry>,
    val nativeInjections: List<NativeInjection>,
    val crashEvidence: List<CrashEvidence>,
    val suspendedModules: Set<String>,
)

private data class ModuleDisplayEntry(
    val name: String,
    val id: String,
    val abis: List<String>,
    val state: MonitorState,
    val crashEvidence: List<CrashEvidence> = emptyList(),
    val suspended: Boolean = false,
)

private data class RuntimeMonitorEntry(
    val pid: Int,
    val generation: Int,
    val kind: String,
    val abi: String,
    val module: String,
    val state: MonitorState,
    val processState: ProcessState,
)

private fun zygoteModules(
    runtime: List<RuntimeMonitorEntry>,
    pid: Int,
    generation: Int,
    abi: String,
): List<String> = runtime.filter {
    it.kind == "zygote" && it.pid == pid && it.generation == generation &&
        pid > 0 && generation != 0 && it.abi == abi &&
        it.module.isNotBlank() && it.state == MonitorState.Injected &&
        it.processState != ProcessState.Exited
}.map { it.module }.distinct()

private fun parseYzStatus(json: String): YzStatus? = runCatching {
    val o = JSONObject(json)
    val runtime = o.optJSONArray("runtime")?.let { a ->
        (0 until a.length()).map { i ->
            val r = a.getJSONObject(i)
            RuntimeMonitorEntry(
                pid = r.optInt("pid", 0),
                generation = r.optInt("generation", 0),
                kind = r.optString("kind", ""),
                abi = r.optString("abi", "unknown"),
                module = r.optString("module", ""),
                state = parseInjectionState(r),
                processState = parseProcessState(r),
            )
        }
    } ?: emptyList()
    val modules = o.optJSONArray("modules")?.let { a ->
        (0 until a.length()).map { a.getString(it) }
    } ?: emptyList()
    val legacyZygotes = o.optJSONArray("zygotes")?.let { a ->
        (0 until a.length()).map { i ->
            val z = a.getJSONObject(i)
            ZygoteMonitorEntry(
                pid = z.optInt("pid", 0),
                name = z.optString("target").ifBlank { z.optString("name", "zygote") },
                abi = z.optString("abi", "unknown"),
                state = MonitorState.Injected,
                generation = z.optInt("generation", 0),
                modules = zygoteModules(
                    runtime, z.optInt("pid", 0), z.optInt("generation", 0), z.optString("abi"),
                ),
                moduleMonitorAvailable = o.optBoolean("zygisk_module_monitor", false),
            )
        }
    } ?: emptyList()
    val monitoredZygoteArray = o.optJSONArray("zygote_monitor")
    val monitoredZygotes = monitoredZygoteArray?.let { a ->
        (0 until a.length()).map { i ->
            val z = a.getJSONObject(i)
            ZygoteMonitorEntry(
                pid = z.optInt("pid", 0),
                name = z.optString("target").ifBlank { z.optString("name", "zygote") },
                abi = z.optString("abi", "unknown"),
                state = parseInjectionState(z),
                generation = z.optInt("generation", 0),
                modules = zygoteModules(
                    runtime, z.optInt("pid", 0), z.optInt("generation", 0), z.optString("abi"),
                ),
                moduleMonitorAvailable = o.optBoolean("zygisk_module_monitor", false),
            )
        }
    } ?: emptyList()
    val zygotes = if (monitoredZygoteArray != null) monitoredZygotes else legacyZygotes
    val nativeModules = o.optJSONArray("native_modules")?.let { a ->
        (0 until a.length()).map { i ->
            val n = a.getJSONObject(i)
            NativeModuleEntry(
                name = n.optString("name", ""),
                id = n.optString("id", ""),
                targetType = n.optString("target_type", "name"),
                target = n.optString("target", ""),
                state = parseMonitorState(n.optString("state", "unknown")),
            )
        }
    } ?: emptyList()
    val nativeInjections = o.optJSONArray("native_injections")?.let { a ->
        (0 until a.length()).map { i ->
            val n = a.getJSONObject(i)
            NativeInjection(
                pid = n.optInt("pid", 0),
                generation = n.optInt("generation", 0),
                process = n.optString("process", ""),
                module = n.optString("module", ""),
                targetType = n.optString("target_type", "name"),
                target = n.optString("target", ""),
                abi = n.optString("abi", "unknown"),
                state = parseInjectionState(n),
                processState = parseProcessState(n),
            )
        }
    } ?: emptyList()
    val crashEvidence = o.optJSONArray("crash_evidence")?.let { a ->
        (0 until a.length()).mapNotNull { i ->
            val e = a.optJSONObject(i) ?: return@mapNotNull null
            CrashEvidence(
                module = e.optString("module", ""),
                process = e.optString("process", ""),
                tombstone = e.optString("tombstone", ""),
                frame = e.optString("frame", ""),
                zygotePid = e.optInt("zygote_pid", 0),
                generation = e.optInt("generation", 0),
                pid = e.optInt("pid", 0),
                abi = e.optString("abi", ""),
                timestamp = e.optString("timestamp", ""),
            )
        }
    } ?: emptyList()
    YzStatus(
        zygotes,
        modules,
        runtime,
        nativeModules,
        nativeInjections,
        crashEvidence,
        o.optJSONArray("suspended_modules")?.let { a ->
            (0 until a.length()).map { a.getString(it) }.toSet()
        } ?: emptySet(),
    )
}.getOrNull()

private data class YzSnapshot(
    val zygotes: List<ZygoteMonitorEntry>,
    val modules: List<ModuleDisplayEntry>,
    val nativeModules: List<NativeModuleEntry>,
    val nativeInjections: List<NativeInjection>,
    val crashEvidence: List<CrashEvidence>,
)

private const val YZ_POLL_INTERVAL_MS = 2000L

private val YZ_MODULE_ABIS = listOf("arm64-v8a", "armeabi-v7a")

private fun readModuleDisplayName(moduleId: String): String {
    if (moduleId.isBlank()) return moduleId
    return runCatching {
        val propFile = SuFile("/data/adb/modules/$moduleId/module.prop")
        if (!propFile.isFile) return@runCatching moduleId
        SuFileInputStream.open(propFile).bufferedReader().useLines { lines ->
            lines.firstNotNullOfOrNull { line ->
                line.takeIf { it.startsWith("name=") }
                    ?.substringAfter('=')
                    ?.trim()
                    ?.takeIf { it.isNotEmpty() }
            } ?: moduleId
        }
    }.getOrDefault(moduleId)
}

private fun readModuleDisplayNames(moduleIds: Collection<String>): Map<String, String> =
    moduleIds.distinct().associateWith(::readModuleDisplayName)

private fun readModuleArchitectures(moduleId: String): List<String> =
    YZ_MODULE_ABIS.filter { abi ->
        SuFile("/data/adb/modules/$moduleId/zygisk/$abi.so").isFile
    }

private fun readModuleArchitectures(moduleIds: Collection<String>): Map<String, List<String>> =
    moduleIds.distinct().associateWith(::readModuleArchitectures)

private fun zygiskModuleState(
    moduleId: String,
    abis: List<String>,
    zygotes: List<ZygoteMonitorEntry>,
    runtime: List<RuntimeMonitorEntry>,
): MonitorState {
    var injected = false
    var failed = false
    for (abi in abis.filter { it in YZ_MODULE_ABIS }) {
        val matchingZygotes = zygotes.filter { it.abi == abi && it.pid > 0 }
        if (matchingZygotes.isEmpty()) continue
        if (matchingZygotes.any { it.state == MonitorState.Crashed || it.state == MonitorState.Failed }) {
            failed = true
            continue
        }
        val records = runtime.filter {
            it.kind == "zygote" && it.module == moduleId && it.abi == abi &&
                it.processState != ProcessState.Exited &&
                matchingZygotes.any { z ->
                    z.moduleMonitorAvailable && z.generation != 0 &&
                        z.pid == it.pid && z.generation == it.generation
                }
        }
        if (records.any { it.state == MonitorState.Crashed || it.state == MonitorState.Failed }) {
            failed = true
        } else if (records.any { it.state == MonitorState.Injected }) {
            injected = true
        }
    }
    return when {
        failed -> MonitorState.Failed
        injected -> MonitorState.Injected
        else -> MonitorState.Unknown
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun YukiZygiskScreen(navigator: DestinationsNavigator) {
    val topAppBarState = rememberTopAppBarState()
    val scrollBehavior = if (isExpressiveUi) {
        TopAppBarDefaults.exitUntilCollapsedScrollBehavior(topAppBarState)
    } else {
        TopAppBarDefaults.pinnedScrollBehavior(topAppBarState)
    }
    val scope = rememberCoroutineScope()
    val snackBarHost = rememberSnackbarController()

    var config by remember { mutableStateOf(YzConfig()) }
    var monitoredZygotes by remember { mutableStateOf<List<ZygoteMonitorEntry>>(emptyList()) }
    var zygiskModules by remember { mutableStateOf<List<ModuleDisplayEntry>>(emptyList()) }
    var nativeModules by remember { mutableStateOf<List<NativeModuleEntry>>(emptyList()) }
    var nativeInjections by remember { mutableStateOf<List<NativeInjection>>(emptyList()) }
    var crashEvidence by remember { mutableStateOf<List<CrashEvidence>>(emptyList()) }
    var nativeMonitorMode by remember { mutableStateOf(NativeMonitorMode.Module) }
    var monitorDialog by remember { mutableStateOf<MonitorDialogState?>(null) }

    LaunchedEffect(Unit) {
        config = readYzConfig()
    }

    LaunchedEffect(Unit) {
        val moduleNameCache = mutableMapOf<String, String>()
        val moduleAbiCache = mutableMapOf<String, List<String>>()
        while (true) {
            val snapshot = withContext(Dispatchers.IO) {
                val st = getYukiZygiskStatusJson()?.let(::parseYzStatus)
                    ?: return@withContext null
                val moduleIds = st.modules + st.nativeModules.map { it.id } +
                    st.zygotes.flatMap { it.modules }
                val missingModuleIds = moduleIds.distinct().filterNot(moduleNameCache::containsKey)
                if (missingModuleIds.isNotEmpty()) {
                    moduleNameCache.putAll(readModuleDisplayNames(missingModuleIds))
                    moduleAbiCache.putAll(readModuleArchitectures(missingModuleIds))
                }
                val moduleEntries = st.modules.map { id ->
                    ModuleDisplayEntry(
                        name = moduleNameCache[id] ?: id,
                        id = id,
                        abis = moduleAbiCache[id].orEmpty(),
                        state = zygiskModuleState(
                            id, moduleAbiCache[id].orEmpty(), st.zygotes, st.runtime,
                        ),
                        crashEvidence = st.crashEvidence.filter { it.module == id },
                        suspended = id in st.suspendedModules,
                    )
                }
                val zygotes = st.zygotes.map { zygote ->
                    zygote.copy(
                        modules = zygote.modules.map { moduleNameCache[it] ?: it },
                    )
                }
                YzSnapshot(
                    zygotes = zygotes,
                    modules = moduleEntries,
                    nativeModules = st.nativeModules.map { module ->
                        module.copy(name = moduleNameCache[module.id] ?: module.id)
                    },
                    nativeInjections = st.nativeInjections,
                    crashEvidence = st.crashEvidence,
                )
            }
            if (snapshot != null) {
                monitoredZygotes = snapshot.zygotes
                zygiskModules = snapshot.modules
                nativeModules = snapshot.nativeModules
                nativeInjections = snapshot.nativeInjections
                crashEvidence = snapshot.crashEvidence
            } else {
                monitoredZygotes = emptyList()
                zygiskModules = emptyList()
                nativeModules = emptyList()
                nativeInjections = emptyList()
                crashEvidence = emptyList()
            }
            delay(YZ_POLL_INTERVAL_MS)
        }
    }

    val saveFailedMessage = stringResource(R.string.yukizygisk_config_save_failed)
    fun save(newCfg: YzConfig) {
        config = newCfg
        scope.launch {
            if (!writeYzConfig(newCfg)) {
                config = readYzConfig()
                snackBarHost.showSnackbar(saveFailedMessage)
            }
        }
    }

    monitorDialog?.let { dialog ->
        YukiAlertDialog(
            onDismissRequest = { monitorDialog = null },
            title = { Text(dialog.title) },
            text = { Text(dialog.message, modifier = Modifier.verticalScroll(rememberScrollState())) },
            confirmButton = {
                TextButton(onClick = { monitorDialog = null }) {
                    Text(stringResource(R.string.close))
                }
            },
        )
    }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            YukiZygiskTopBar(
                onBack = { navigator.popBackStack() },
                scrollBehavior = scrollBehavior,
            )
        },
        snackbarHost = { SnackbarHost(snackBarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(top = 8.dp),
        ) {
            SettingsCard(title = stringResource(R.string.yukizygisk_injected_zygotes)) {
                if (monitoredZygotes.isEmpty()) {
                    EmptyMonitorGroup(stringResource(R.string.yukizygisk_no_zygotes))
                } else {
                    monitoredZygotes.forEach { zygote ->
                        val dialog = zygoteDialog(zygote)
                        ZygoteMonitorRow(zygote) {
                            monitorDialog = dialog
                        }
                    }
                }
            }

            MonitorCard(title = stringResource(R.string.yukizygisk_modules)) {
                if (zygiskModules.isEmpty()) {
                    EmptyMonitorGroup(stringResource(R.string.yukizygisk_no_modules))
                } else {
                    zygiskModules.forEach { module ->
                        val dialog = zygiskModuleDialog(module)
                        ZygiskModuleRow(module) { monitorDialog = dialog }
                    }
                }
            }

            MonitorCard(
                title = stringResource(R.string.yukizygisk_native_injections),
                trailing = {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.clickable {
                            nativeMonitorMode = when (nativeMonitorMode) {
                                NativeMonitorMode.Module -> NativeMonitorMode.Process
                                NativeMonitorMode.Process -> NativeMonitorMode.Module
                            }
                        },
                    ) {
                        Text(
                            when (nativeMonitorMode) {
                                NativeMonitorMode.Module ->
                                    stringResource(R.string.yukizygisk_native_mode_module)
                                NativeMonitorMode.Process ->
                                    stringResource(R.string.yukizygisk_native_mode_process)
                            },
                            style = MaterialTheme.typography.bodySmall,
                            fontWeight = FontWeight.Light,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        IconButton(
                            onClick = {
                                nativeMonitorMode = when (nativeMonitorMode) {
                                    NativeMonitorMode.Module -> NativeMonitorMode.Process
                                    NativeMonitorMode.Process -> NativeMonitorMode.Module
                                }
                            },
                            modifier = Modifier.size(36.dp),
                        ) {
                            Icon(
                                imageVector = Icons.Filled.SwapHoriz,
                                contentDescription = null,
                                modifier = Modifier.size(20.dp),
                            )
                        }
                    }
                },
            ) {
                val moduleRows = remember(nativeModules, nativeInjections, crashEvidence) {
                    buildNativeModuleRows(nativeModules, nativeInjections, crashEvidence)
                }
                val processRows = remember(nativeInjections) {
                    buildNativeProcessRows(nativeInjections)
                }

                if (nativeMonitorMode == NativeMonitorMode.Module && moduleRows.isEmpty()) {
                    EmptyMonitorGroup(stringResource(R.string.yukizygisk_no_native_modules))
                } else if (nativeMonitorMode == NativeMonitorMode.Process && processRows.isEmpty()) {
                    EmptyMonitorGroup(stringResource(R.string.yukizygisk_no_native_injections))
                } else if (nativeMonitorMode == NativeMonitorMode.Module) {
                    moduleRows.forEach { module ->
                        val dialog = nativeModuleDialog(module)
                        NativeModuleMonitorRow(module) {
                            monitorDialog = dialog
                        }
                    }
                } else {
                    processRows.forEach { process ->
                        val dialog = nativeProcessDialog(process)
                        NativeProcessMonitorRow(process) {
                            monitorDialog = dialog
                        }
                    }
                }
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_module_loading)) {
                SwitchSettingItem(
                    icon = Icons.Outlined.VisibilityOff,
                    title = stringResource(R.string.yukizygisk_anonymous_memory_title),
                    summary = stringResource(R.string.yukizygisk_anonymous_memory_summary),
                    checked = config.anonymousMemory,
                    groupPosition = MoreSettingsItemPosition.First,
                    onChange = { save(config.copy(anonymousMemory = it)) },
                )
                SwitchSettingItem(
                    icon = Icons.Outlined.Link,
                    title = stringResource(R.string.yukizygisk_yukilinker_title),
                    summary = stringResource(R.string.yukizygisk_yukilinker_summary),
                    checked = config.yukilinker,
                    groupPosition = MoreSettingsItemPosition.Middle,
                    onChange = { save(config.copy(yukilinker = it)) },
                )
                SwitchSettingItem(
                    icon = Icons.Outlined.Warning,
                    title = stringResource(R.string.yukizygisk_crash_protection_title),
                    summary = stringResource(R.string.yukizygisk_crash_protection_summary),
                    checked = config.crashProtection,
                    groupPosition = MoreSettingsItemPosition.Middle,
                    onChange = { save(config.copy(crashProtection = it)) },
                )
                SwitchSettingItem(
                    icon = Icons.Filled.Bolt,
                    title = stringResource(R.string.yukizygisk_early_load_title),
                    summary = stringResource(R.string.yukizygisk_early_load_summary),
                    checked = config.earlyLoad,
                    groupPosition = MoreSettingsItemPosition.Last,
                    onChange = { save(config.copy(earlyLoad = it)) },
                )
                DenylistModeSelector(
                    mode = config.denylistMode,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                ) { save(config.copy(denylistMode = it)) }
            }

            SettingsCard(title = stringResource(R.string.yukizygisk_logging_title)) {
                SwitchSettingItem(
                    icon = Icons.AutoMirrored.Filled.Article,
                    title = stringResource(R.string.yukizygisk_log_dmesg_title),
                    summary = stringResource(R.string.yukizygisk_log_dmesg_summary),
                    checked = config.dmesgLog,
                    groupPosition = MoreSettingsItemPosition.Only,
                    onChange = { save(config.copy(dmesgLog = it)) },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun YukiZygiskTopBar(
    onBack: () -> Unit,
    scrollBehavior: androidx.compose.material3.TopAppBarScrollBehavior,
) {
    val title: @Composable () -> Unit = {
        Text(
            text = stringResource(R.string.settings_yukizygisk),
            fontWeight = if (isExpressiveUi) FontWeight.Normal else null,
        )
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back),
            )
        }
    }
    val colors = TopAppBarDefaults.topAppBarColors(
        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow,
    )
    val windowInsets = WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal
    )

    if (isExpressiveUi) {
        LargeFlexibleTopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    } else {
        TopAppBar(
            title = title,
            navigationIcon = navigationIcon,
            colors = colors,
            windowInsets = windowInsets,
            scrollBehavior = scrollBehavior,
        )
    }
}

@Composable
private fun ZygoteMonitorRow(zygote: ZygoteMonitorEntry, onStatusClick: () -> Unit) {
    ListItem(
        modifier = Modifier.monitorGroup(),
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            YukiIcon(
                imageVector = Icons.Filled.Adb,
                contentDescription = null,
                tint = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
                modifier = Modifier
                    .padding(4.dp)
                    .size(28.dp),
            )
        },
        content = {
            Text(
                zygote.name,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.yukizygisk_zygote_detail,
                    zygote.abi,
                    zygote.pid,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            MonitorStateButton(zygote.state, onStatusClick)
        },
    )
}

@Composable
private fun ZygiskModuleRow(module: ModuleDisplayEntry, onStatusClick: () -> Unit) {
    ListItem(
        modifier = Modifier
            .monitorGroup()
            .clickable(onClick = onStatusClick),
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            YukiIcon(
                imageVector = Icons.Filled.Extension,
                contentDescription = null,
                tint = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
                modifier = Modifier
                    .padding(4.dp)
                    .size(28.dp),
            )
        },
        content = {
            Column {
                Text(
                    module.name,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    module.id,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        },
        supportingContent = {
            if (module.suspended || module.crashEvidence.isNotEmpty()) {
                Text(
                    stringResource(if (module.suspended) R.string.yukizygisk_module_suspended
                        else R.string.yukizygisk_module_crash_badge),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.labelMedium,
                )
            }
        },
        trailingContent = {
            if (module.suspended) {
                MonitorStatusButton(
                    icon = Icons.Outlined.Warning,
                    tint = MaterialTheme.colorScheme.error,
                    onClick = onStatusClick,
                    contentDescription = stringResource(R.string.yukizygisk_module_suspended),
                )
            } else {
                MonitorStateButton(
                    if (module.state == MonitorState.Crashed) MonitorState.Failed else module.state,
                    onStatusClick,
                )
            }
        },
    )
}

@Composable
private fun NativeModuleMonitorRow(module: NativeModuleMonitorEntry, onStatusClick: () -> Unit) {
    ListItem(
        modifier = Modifier
            .monitorGroup()
            .clickable(onClick = onStatusClick),
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            YukiIcon(
                imageVector = Icons.Filled.Extension,
                contentDescription = null,
                tint = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
                modifier = Modifier
                    .padding(4.dp)
                    .size(28.dp),
            )
        },
        content = {
            Column {
                Text(
                    module.name,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    module.id,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        },
        supportingContent = {
            if (module.crashEvidence.isNotEmpty()) {
                Text(
                    stringResource(R.string.yukizygisk_module_crash_badge),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.labelMedium,
                )
            }
        },
        trailingContent = {
            MonitorStateButton(
                if (module.state == MonitorState.Crashed) MonitorState.Failed else module.state,
                onStatusClick,
            )
        },
    )
}

@Composable
private fun NativeProcessMonitorRow(process: NativeProcessEntry, onStatusClick: () -> Unit) {
    ListItem(
        modifier = Modifier.monitorGroup(),
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            YukiIcon(
                imageVector = Icons.Filled.Terminal,
                contentDescription = null,
                tint = if (isExpressiveUi) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
                modifier = Modifier
                    .padding(4.dp)
                    .size(28.dp),
            )
        },
        content = {
            Text(
                process.process,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.yukizygisk_native_process_detail,
                    process.abi,
                    process.pid,
                ) + "\n" + stringResource(processStateLabel(process.processState)),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            MonitorStateButton(process.state, onStatusClick)
        },
    )
}

@Composable
private fun MonitorStateButton(state: MonitorState, onClick: () -> Unit) {
    val icon: ImageVector
    val tint: Color
    when (state) {
        MonitorState.Injected -> {
            icon = Icons.Outlined.TaskAlt
            tint = MaterialTheme.colorScheme.primary
        }
        MonitorState.Unsupported32 -> {
            icon = Icons.Outlined.Warning
            tint = MaterialTheme.colorScheme.tertiary
        }
        MonitorState.Crashed -> {
            icon = Icons.Outlined.Warning
            tint = MaterialTheme.colorScheme.error
        }
        MonitorState.Failed -> {
            icon = Icons.Outlined.Cancel
            tint = MaterialTheme.colorScheme.error
        }
        MonitorState.Unknown -> {
            icon = Icons.AutoMirrored.Outlined.HelpOutline
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        }
    }
    MonitorStatusButton(icon, tint, onClick)
}

@Composable
private fun MonitorStatusButton(
    icon: ImageVector,
    tint: Color,
    onClick: () -> Unit,
    contentDescription: String? = null,
) {
    IconButton(
        onClick = onClick,
        modifier = Modifier
            .padding(start = 8.dp)
            .size(40.dp),
    ) {
        YukiIcon(
            imageVector = icon,
            contentDescription = contentDescription,
            tint = tint,
            modifier = Modifier.size(22.dp),
        )
    }
}

@Composable
private fun MonitorCard(
    title: String,
    trailing: @Composable (() -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    if (isExpressiveUi) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 20.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .padding(horizontal = 8.dp),
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Normal,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.weight(1f),
                )
                trailing?.invoke()
            }
            content()
        }
    } else {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 16.dp),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerHigh),
            elevation = getCardElevation(),
            shape = MaterialTheme.shapes.medium,
        ) {
            Column(modifier = Modifier.padding(vertical = 8.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                        .padding(start = 16.dp, end = 8.dp),
                ) {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.weight(1f),
                    )
                    trailing?.invoke()
                }
                content()
            }
        }
    }
}

@Composable
private fun zygoteDialog(zygote: ZygoteMonitorEntry): MonitorDialogState {
    val resources = LocalResources.current
    val message = when (zygote.state) {
        MonitorState.Injected -> ""
        MonitorState.Unsupported32 -> stringResource(R.string.yukizygisk_zygote_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_zygote_crashed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_zygote_failed_message)
        MonitorState.Unknown -> stringResource(R.string.yukizygisk_zygote_unknown_message)
    }
    val modules = if (!zygote.moduleMonitorAvailable) {
        resources.getString(R.string.yukizygisk_zygote_modules_unavailable)
    } else resources.getString(
        R.string.yukizygisk_zygote_modules_detail,
        zygote.modules.size,
        zygote.modules.joinToString("\n").ifBlank {
            resources.getString(R.string.yukizygisk_zygote_no_modules_detail)
        },
    )
    return MonitorDialogState(
        zygote.name,
        listOf(message, modules).filter(String::isNotBlank).joinToString("\n"),
    )
}

@Composable
private fun crashEvidenceMessage(evidence: List<CrashEvidence>): String {
    if (evidence.isEmpty()) return ""
    val resources = LocalResources.current
    val details = evidence.joinToString("\n\n") {
        resources.getString(
            R.string.yukizygisk_module_crash_evidence,
            it.process,
            it.pid,
            it.abi,
            it.timestamp,
            it.tombstone,
        ) + "\n" + it.frame
    }
    return resources.getString(R.string.yukizygisk_module_crash_explanation) + "\n\n" + details
}

@Composable
private fun zygiskModuleDialog(module: ModuleDisplayEntry): MonitorDialogState {
    val resources = LocalResources.current
    val abis = module.abis.joinToString(", ").ifBlank {
        resources.getString(R.string.yukizygisk_module_no_supported_abis)
    }
    val crash = crashEvidenceMessage(module.crashEvidence)
    return MonitorDialogState(
        module.name,
        listOf(
            resources.getString(R.string.yukizygisk_module_supported_abis, abis),
            if (module.suspended) resources.getString(R.string.yukizygisk_module_suspended_detail)
                else "",
            crash,
        ).filter { it.isNotBlank() }.joinToString("\n"),
    )
}

@Composable
private fun nativeProcessDialog(process: NativeProcessEntry): MonitorDialogState {
    val resources = LocalResources.current
    val modules = process.modules.joinToString("\n") {
        resources.getString(R.string.yukizygisk_native_process_module_line, it)
    }
    val base = when (process.state) {
        MonitorState.Injected -> stringResource(R.string.yukizygisk_native_process_injected_message)
        MonitorState.Unsupported32 ->
            stringResource(R.string.yukizygisk_native_process_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_native_process_failed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_native_process_failed_message)
        MonitorState.Unknown -> stringResource(
            if (process.pid > 0) R.string.yukizygisk_native_injection_unknown
            else R.string.yukizygisk_native_process_unknown_message
        )
    }
    val identity = resources.getString(
        R.string.yukizygisk_native_process_detail, process.abi, process.pid
    )
    return MonitorDialogState(
        process.process,
        listOf(base, identity, resources.getString(processStateLabel(process.processState)), modules)
            .filter(String::isNotBlank).joinToString("\n"),
    )
}

@Composable
private fun nativeModuleDialog(module: NativeModuleMonitorEntry): MonitorDialogState {
    val resources = LocalResources.current
    val scopes = module.scopes.joinToString("\n") { scope ->
        val status = resources.getString(
            if (scope.state == MonitorState.Unknown && scope.targets.isEmpty())
                R.string.yukizygisk_native_scope_unobserved
            else injectionStateLabel(scope.state)
        )
        val targets = scope.targets.joinToString("\n") {
            val identity = resources.getString(
                R.string.yukizygisk_native_scope_process, it.process, it.pid
            ) + " · " + it.abi
            resources.getString(
                R.string.yukizygisk_native_target_detail,
                identity,
                resources.getString(injectionStateLabel(it.state)),
                resources.getString(processStateLabel(it.processState)),
            )
        }
        if (targets.isBlank()) {
            if (scope.target.isNotBlank()) "${scope.target}: $status" else status
        } else {
            targets
        }
    }
    val base = if (module.scopes.any { it.targets.isNotEmpty() }) "" else when (module.state) {
        MonitorState.Injected -> stringResource(R.string.yukizygisk_native_module_injected_message)
        MonitorState.Unsupported32 ->
            stringResource(R.string.yukizygisk_native_module_unsupported_message)
        MonitorState.Crashed -> stringResource(R.string.yukizygisk_native_module_failed_message)
        MonitorState.Failed -> stringResource(R.string.yukizygisk_native_module_failed_message)
        MonitorState.Unknown -> stringResource(R.string.yukizygisk_native_module_unknown_message)
    }
    return MonitorDialogState(
        module.name,
        listOf(base, scopes, crashEvidenceMessage(module.crashEvidence))
            .filter(String::isNotBlank).joinToString("\n"),
    )
}

@Composable
private fun Modifier.monitorGroup(): Modifier = if (isExpressiveUi) {
    this
        .fillMaxWidth()
        .padding(
            horizontal = 6.dp,
            vertical = ListItemDefaults.SegmentedGap / 2,
        )
        .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
        .clip(ListItemDefaults.shapes().shape)
        .background(
            MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
        )
} else {
    fillMaxWidth()
}

@Composable
private fun EmptyMonitorGroup(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = if (isExpressiveUi) {
            Modifier
                .fillMaxWidth()
                .padding(
                    horizontal = 6.dp,
                    vertical = ListItemDefaults.SegmentedGap / 2,
                )
                .defaultMinSize(minHeight = 56.dp)
                .clip(MaterialTheme.shapes.large)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
                )
                .padding(horizontal = 16.dp, vertical = 12.dp)
        } else {
            Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DenylistModeSelector(
    mode: Int,
    modifier: Modifier = Modifier,
    onSelect: (Int) -> Unit,
) {
    val options = listOf(
        stringResource(R.string.yukizygisk_denylist_off),
        stringResource(R.string.yukizygisk_denylist_force),
        stringResource(R.string.yukizygisk_denylist_restore),
    )
    if (isExpressiveUi) {
        Row(
            modifier = modifier.selectableGroup(),
            horizontalArrangement = Arrangement.spacedBy(ButtonGroupDefaults.ConnectedSpaceBetween),
        ) {
            options.forEachIndexed { index, label ->
                ToggleButton(
                    checked = mode == index,
                    onCheckedChange = { onSelect(index) },
                    modifier = Modifier
                        .weight(1f)
                        .semantics { role = Role.RadioButton },
                    shapes = when (index) {
                        0 -> ButtonGroupDefaults.connectedLeadingButtonShapes()
                        options.lastIndex -> ButtonGroupDefaults.connectedTrailingButtonShapes()
                        else -> ButtonGroupDefaults.connectedMiddleButtonShapes()
                    },
                ) {
                    Text(label)
                }
            }
        }
    } else {
        SingleChoiceSegmentedButtonRow(modifier = modifier) {
            options.forEachIndexed { index, label ->
                SegmentedButton(
                    selected = mode == index,
                    onClick = { onSelect(index) },
                    shape = SegmentedButtonDefaults.itemShape(index, options.size),
                ) {
                    Text(label)
                }
            }
        }
    }
}
