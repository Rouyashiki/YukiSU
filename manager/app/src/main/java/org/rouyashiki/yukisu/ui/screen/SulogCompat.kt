package org.rouyashiki.yukisu.ui.screen

import android.os.SystemClock
import org.rouyashiki.yukisu.BuildConfig
import org.rouyashiki.yukisu.ui.util.shellArg
import org.rouyashiki.yukisu.ui.util.runCmd
import com.topjohnwu.superuser.Shell
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

private const val SULOG_DIR = "/data/adb/ksu/log"
private const val SULOG_FILE_GLOB = "$SULOG_DIR/sulog-*.log"
private const val SULOG_STATE_PATH = "/data/adb/ksu/sulogd.state"
private const val SULOG_FILE_PREFIX = "sulog-"
private const val SULOG_FILE_SUFFIX = ".log"
private const val NS_PER_MILLISECOND = 1_000_000L
private const val MIN_VALID_WALL_TIME_MILLIS = 946_684_800_000L
private val sulogFileNameRegex = Regex("""$SULOG_FILE_PREFIX(\d{4}-\d{2}-\d{2})(?:-(\d+))?$SULOG_FILE_SUFFIX""")

private val upstreamSulogTimestampFormatter: DateTimeFormatter =
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss", Locale.US)

data class SulogLogSource(
    val path: String,
    val name: String,
) {
    val displayName: String
        get() = name.removePrefix(SULOG_FILE_PREFIX).removeSuffix(SULOG_FILE_SUFFIX)
}

enum class SulogLogSourceCleanAction {
    Clear,
    Delete,
}

private data class SulogSessionAnchor(
    val bootId: String?,
    val wallTimeMillis: Long?,
    val elapsedRealtimeMillis: Long?,
)

private data class ParsedSulogLine(
    val rawLine: String,
    val fields: Map<String, String>,
)

private fun parseSulogFileNames(fileNames: List<String>): List<String> {
    return fileNames
        .mapNotNull { name ->
            val match = sulogFileNameRegex.matchEntire(name) ?: return@mapNotNull null
            val date = LocalDate.parse(match.groupValues[1])
            val rotation = match.groupValues[2].takeIf { it.isNotEmpty() }?.toIntOrNull() ?: 0
            Triple(name, date, rotation)
        }
        .sortedWith(
            compareByDescending<Triple<String, LocalDate, Int>> { it.second }
                .thenByDescending { it.third }
        )
        .map { it.first }
}

fun listSulogSources(shell: Shell): List<SulogLogSource> {
    val fileNames = runCmd(
        shell,
        "ls -1 $SULOG_FILE_GLOB 2>/dev/null | sed 's#.*/##'"
    ).lineSequence()
        .map { it.trim() }
        .filter { it.isNotBlank() }
        .toList()

    return parseSulogFileNames(fileNames).map { name ->
        SulogLogSource(
            path = "$SULOG_DIR/$name",
            name = name,
        )
    }
}

fun resolveSelectedSulogSource(
    sources: List<SulogLogSource>,
    preferredPath: String,
    activePath: String,
    previousActivePath: String = "",
): SulogLogSource {
    val followActive = preferredPath.isBlank() ||
        (preferredPath.isNotBlank() && previousActivePath.isNotBlank() && preferredPath == previousActivePath)
    return when {
        sources.isEmpty() -> SulogLogSource(path = "", name = "")
        followActive -> sources.firstOrNull { it.path == activePath }
            ?: sources.firstOrNull { it.path == preferredPath }
            ?: sources.first()
        preferredPath.isNotBlank() -> sources.firstOrNull { it.path == preferredPath }
            ?: sources.firstOrNull { it.path == activePath }
            ?: sources.first()
        else -> sources.firstOrNull { it.path == activePath } ?: sources.first()
    }
}

fun resolveSulogSourceCleanAction(
    selectedPath: String,
    activePath: String,
): SulogLogSourceCleanAction {
    return if (selectedPath.isNotBlank() && selectedPath == activePath) {
        SulogLogSourceCleanAction.Clear
    } else {
        SulogLogSourceCleanAction.Delete
    }
}

fun readActiveSulogPath(shell: Shell): String {
    return runCmd(
        shell,
        "awk -F= '\$1==\"path\"{print substr(\$0, index(\$0, \"=\")+1)}' $SULOG_STATE_PATH 2>/dev/null | head -n 1"
    ).lineSequence().firstOrNull { it.startsWith("/") }.orEmpty()
}

fun buildSulogSourceSignature(
    shell: Shell,
    sources: List<SulogLogSource>,
    source: SulogLogSource,
    activePath: String,
): String {
    if (source.path.isBlank()) {
        return "missing|0 0|$activePath|${sources.joinToString(separator = ",") { it.name }}"
    }
    val stat = runCmd(
        shell,
        "stat -c '%Y %s' ${shellArg(source.path)} 2>/dev/null || echo '0 0'"
    ).trim()
    val fileListSignature = sources.joinToString(separator = ",") { it.name }
    return "${source.path}|$stat|$activePath|$fileListSignature"
}

fun readSulogSourceStat(shell: Shell, path: String): String {
    if (path.isBlank()) return "0 0"
    return runCmd(
        shell,
        "stat -c '%Y %s' ${shellArg(path)} 2>/dev/null || echo '0 0'"
    ).trim()
}

fun buildVisibleSulogSourceSignature(
    shell: Shell,
    sources: List<SulogLogSource>,
    source: SulogLogSource,
    activePath: String,
): String {
    val fileListSignature = sources.joinToString(separator = ",") { it.name }
    if (source.path.isBlank()) {
        return "missing|$activePath|$fileListSignature"
    }
    val tailContent = runCmd(
        shell,
        "tail -n 256 ${shellArg(source.path)} 2>/dev/null || echo ''"
    )
    val marker = extractLatestVisibleSulogMarker(tailContent)
    return "${source.path}|$activePath|$fileListSignature|$marker"
}

fun parseSulogEntries(logContent: String, useCurrentClockFallback: Boolean): List<LogEntry> {
    if (logContent.isBlank()) return emptyList()

    val currentTimeMillis = System.currentTimeMillis()
    val elapsedRealtimeMillis = SystemClock.elapsedRealtime()
    val zoneId = ZoneId.systemDefault()
    val parsedLines = logContent.lineSequence()
        .mapNotNull { line ->
            if (line.isBlank()) return@mapNotNull null
            val fields = parseKeyValueLine(line)
            if (fields.isEmpty()) return@mapNotNull null
            ParsedSulogLine(rawLine = line, fields = fields)
        }
        .toList()
    if (parsedLines.isEmpty()) return emptyList()

    val preferredAnchor = parsedLines
        .asSequence()
        .mapNotNull { parseSessionAnchor(it.fields) }
        .lastOrNull { it.hasValidWallClock() }
    val entries = mutableListOf<LogEntry>()
    var sessionAnchor: SulogSessionAnchor? = null

    for ((line, fields) in parsedLines) {
        parseSessionAnchor(fields)?.let { sessionAnchor = it }
        val effectiveAnchor = when {
            sessionAnchor.hasValidWallClock() -> sessionAnchor
            preferredAnchor != null &&
                (sessionAnchor?.bootId == null ||
                    preferredAnchor.bootId == null ||
                    sessionAnchor.bootId == preferredAnchor.bootId) -> preferredAnchor
            else -> sessionAnchor
        }
        parseUpstreamSulogLine(
            line = line,
            fields = fields,
            sessionAnchor = effectiveAnchor,
            currentTimeMillis = currentTimeMillis,
            elapsedRealtimeMillis = elapsedRealtimeMillis,
            zoneId = zoneId,
            useCurrentClockFallback = useCurrentClockFallback,
        )?.let(entries::add)
    }

    return entries
}

private fun parseUpstreamSulogLine(
    line: String,
    fields: Map<String, String>,
    sessionAnchor: SulogSessionAnchor?,
    currentTimeMillis: Long,
    elapsedRealtimeMillis: Long,
    zoneId: ZoneId,
    useCurrentClockFallback: Boolean,
): LogEntry? {
    val rawType = fields["type"].orEmpty()
    val type = when (rawType) {
        "root_execve" -> LogType.ROOT_EXECVE
        "sucompat" -> LogType.SUCOMPAT
        "ioctl_grant_root" -> LogType.IOCTL_GRANT_ROOT
        "daemon_start", "daemon_restart", "daemon_time_sync" -> LogType.DAEMON_EVENT
        "dropped" -> LogType.DROPPED
        else -> LogType.UNKNOWN
    }

    val timestamp = parseUpstreamSulogTimestamp(
        timestampNs = fields["ts_ns"],
        sessionAnchor = sessionAnchor,
        currentTimeMillis = currentTimeMillis,
        elapsedRealtimeMillis = elapsedRealtimeMillis,
        zoneId = zoneId,
        useCurrentClockFallback = useCurrentClockFallback,
    ).orEmpty()

    val uid = fields["uid"] ?: fields["euid"] ?: ""
    val pid = fields["pid"] ?: ""
    val comm = when (type) {
        LogType.ROOT_EXECVE -> fields["comm"] ?: fields["file"] ?: rawType.ifBlank { "root_execve" }
        LogType.SUCOMPAT -> fields["comm"] ?: "sucompat"
        LogType.IOCTL_GRANT_ROOT -> fields["comm"] ?: "ioctl_grant_root"
        LogType.DAEMON_EVENT -> rawType.ifBlank { "daemon_event" }
        LogType.DROPPED -> "dropped"
        else -> fields["comm"] ?: rawType.ifBlank { "unknown" }
    }

    val details = when (type) {
        LogType.ROOT_EXECVE -> buildList {
            fields["file"]?.takeIf { it.isNotBlank() }?.let { add("File: $it") }
            fields["argv"]?.takeIf { it.isNotBlank() }?.let { add("Argv: $it") }
            fields["retval"]?.let { add("Result: ${formatRetval(it)}") }
        }.joinToString(", ")

        LogType.SUCOMPAT -> buildList {
            fields["file"]?.takeIf { it.isNotBlank() }?.let { add("File: $it") }
            fields["argv"]?.takeIf { it.isNotBlank() }?.let { add("Argv: $it") }
            fields["retval"]?.let { add("Result: ${formatRetval(it)}") }
        }.joinToString(", ")

        LogType.IOCTL_GRANT_ROOT -> buildList {
            fields["retval"]?.let { add("Result: ${formatRetval(it)}") }
            fields["euid"]?.let { add("EUID: $it") }
        }.joinToString(", ")

        LogType.DAEMON_EVENT -> buildList {
            fields["boot_id"]?.let { add("Boot ID: $it") }
            fields["restart"]?.let { add("Restart: #$it") }
            fields["reason"]?.let { add("Reason: $it") }
        }.joinToString(", ")

        LogType.DROPPED -> buildList {
            fields["dropped"]?.let { add("Dropped: $it") }
            fields["first_seq"]?.let { add("First Seq: $it") }
            fields["last_seq"]?.let { add("Last Seq: $it") }
        }.joinToString(", ")

        else -> fields.entries.joinToString(", ") { (key, value) -> "$key=$value" }
    }

    return LogEntry(
        timestamp = timestamp,
        type = type,
        uid = uid,
        comm = comm,
        details = details,
        pid = pid,
        rawLine = line,
        isViewerSelfNoise = isSulogViewerSelfNoise(fields),
        isCurrentManagerCommand = isCurrentManagerSulogCommand(fields),
    )
}

private fun parseSessionAnchor(fields: Map<String, String>): SulogSessionAnchor? {
    val type = fields["type"]
    if (type != "daemon_start" && type != "daemon_restart" && type != "daemon_time_sync") {
        return null
    }

    return SulogSessionAnchor(
        bootId = fields["boot_id"],
        wallTimeMillis = fields["wall_time_ms"]?.toLongOrNull(),
        elapsedRealtimeMillis = fields["elapsed_ms"]?.toLongOrNull(),
    )
}

private fun SulogSessionAnchor?.hasValidWallClock(): Boolean {
    val anchor = this ?: return false
    val wallTimeMillis = anchor.wallTimeMillis ?: return false
    val elapsedRealtimeMillis = anchor.elapsedRealtimeMillis ?: return false
    return wallTimeMillis >= MIN_VALID_WALL_TIME_MILLIS && wallTimeMillis >= elapsedRealtimeMillis
}

private fun formatRetval(value: String): String {
    val retval = value.toIntOrNull() ?: return value
    return if (retval == 0) "Success" else "Exit $retval"
}

private fun parseKeyValueLine(line: String): Map<String, String> {
    return buildMap {
        var index = 0
        while (index < line.length) {
            while (index < line.length && line[index].isWhitespace()) {
                index++
            }
            if (index >= line.length) break

            val keyStart = index
            while (index < line.length && line[index] != '=' && !line[index].isWhitespace()) {
                index++
            }
            if (index >= line.length || line[index] != '=') {
                while (index < line.length && !line[index].isWhitespace()) {
                    index++
                }
                continue
            }

            val key = line.substring(keyStart, index)
            index++
            val (value, nextIndex) = if (index < line.length && line[index] == '"') {
                parseQuotedSulogValue(line, index + 1)
            } else {
                parseUnquotedSulogValue(line, index)
            }

            if (key.isNotEmpty()) {
                put(key, value)
            }
            index = nextIndex
        }
    }
}

private fun extractLatestVisibleSulogMarker(logContent: String): String {
    if (logContent.isBlank()) return "empty"
    val latestVisible = logContent.lineSequence()
        .mapNotNull { line ->
            if (line.isBlank()) return@mapNotNull null
            val fields = parseKeyValueLine(line)
            if (fields.isEmpty() || isSulogViewerSelfNoise(fields)) {
                return@mapNotNull null
            }
            fields["seq"]?.let { seq -> "seq:$seq" } ?: "line:${line.hashCode()}"
        }
        .lastOrNull()
    return latestVisible ?: "empty"
}

private fun isSulogViewerSelfNoise(fields: Map<String, String>): Boolean {
    if (fields["type"] != "root_execve") return false
    if (fields["comm"] != "sh") return false

    val file = fields["file"].orEmpty()
    val argv = fields["argv"].orEmpty()
    return when (file) {
        "/system/bin/ls" -> argv.startsWith("ls -1 ") && argv.contains("$SULOG_DIR/")
        "/system/bin/sed" -> argv == "sed s#.*/##" ||
            (argv.startsWith("sed -n ") && argv.contains("$SULOG_DIR/"))
        "/system/bin/awk" -> argv.contains(SULOG_STATE_PATH)
        "/system/bin/head" -> argv == "head -n 1"
        "/system/bin/stat" -> argv.startsWith("stat -c ") && argv.contains("$SULOG_DIR/")
        "/system/bin/wc" -> argv == "wc -l"
        "/system/bin/tail" -> argv.startsWith("tail -n ") && argv.contains("$SULOG_DIR/")
        else -> false
    }
}

private fun isCurrentManagerSulogCommand(fields: Map<String, String>): Boolean {
    if (fields["type"] != "root_execve") return false
    return containsManagerAppReference(fields["file"]) || containsManagerAppReference(fields["argv"])
}

private fun containsManagerAppReference(value: String?): Boolean {
    val text = value.orEmpty()
    if (text.isBlank()) return false
    return text.contains(BuildConfig.APPLICATION_ID)
}

private fun parseQuotedSulogValue(line: String, startIndex: Int): Pair<String, Int> {
    val value = StringBuilder()
    var index = startIndex
    while (index < line.length) {
        when (val ch = line[index]) {
            '"' -> return value.toString() to (index + 1)
            '\\' -> index = appendEscapedSulogChar(line, index, value)
            else -> {
                value.append(ch)
                index++
            }
        }
    }
    return value.toString() to index
}

private fun parseUnquotedSulogValue(line: String, startIndex: Int): Pair<String, Int> {
    var index = startIndex
    while (index < line.length && !line[index].isWhitespace()) {
        index++
    }
    return line.substring(startIndex, index) to index
}

private fun appendEscapedSulogChar(line: String, slashIndex: Int, value: StringBuilder): Int {
    val nextIndex = slashIndex + 1
    if (nextIndex >= line.length) {
        value.append('\\')
        return nextIndex
    }

    return when (val escaped = line[nextIndex]) {
        '\\' -> {
            value.append('\\')
            nextIndex + 1
        }

        '"' -> {
            value.append('"')
            nextIndex + 1
        }

        'n' -> {
            value.append('\n')
            nextIndex + 1
        }

        'r' -> {
            value.append('\r')
            nextIndex + 1
        }

        't' -> {
            value.append('\t')
            nextIndex + 1
        }

        'x' -> appendHexEscapedSulogChar(line, nextIndex, value)

        else -> {
            value.append(escaped)
            nextIndex + 1
        }
    }
}

private fun appendHexEscapedSulogChar(line: String, xIndex: Int, value: StringBuilder): Int {
    val hexStart = xIndex + 1
    val hexEnd = hexStart + 2
    if (hexEnd <= line.length) {
        val code = line.substring(hexStart, hexEnd).toIntOrNull(16)
        if (code != null) {
            value.append(code.toChar())
            return hexEnd
        }
    }

    value.append('x')
    return xIndex + 1
}

private fun parseUpstreamSulogTimestamp(
    timestampNs: String?,
    sessionAnchor: SulogSessionAnchor?,
    currentTimeMillis: Long,
    elapsedRealtimeMillis: Long,
    zoneId: ZoneId,
    useCurrentClockFallback: Boolean,
): String? {
    val timestampNanos = timestampNs?.toLongOrNull() ?: return null
    if (timestampNanos < 0) return null

    val bootTimeMillis = when {
        sessionAnchor?.wallTimeMillis != null && sessionAnchor.elapsedRealtimeMillis != null -> {
            sessionAnchor.wallTimeMillis - sessionAnchor.elapsedRealtimeMillis
        }
        useCurrentClockFallback && elapsedRealtimeMillis >= 0 -> {
            currentTimeMillis - elapsedRealtimeMillis
        }
        else -> return null
    }
    if (bootTimeMillis < 0) return null

    val eventTimeMillis = bootTimeMillis + (timestampNanos / NS_PER_MILLISECOND)
    if (eventTimeMillis < 0) return null

    return Instant.ofEpochMilli(eventTimeMillis)
        .atZone(zoneId)
        .format(upstreamSulogTimestampFormatter)
}
