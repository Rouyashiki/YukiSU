package com.anatdx.yukisu.ui.util

import android.content.Context
import android.widget.Toast
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.edit
import com.anatdx.yukisu.Natives
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ksuApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

private const val SETTINGS_PREFS = "settings"
private const val KEY_USE_SOFT_REBOOT = "soft_reboot"
private val softRebootScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
private var refreshJob: Job? = null
private var rebootJob: Job? = null

data class SoftRebootStatus(
    val known: Boolean = false,
    val supported: Boolean = false,
    val running: Boolean = false,
    val blockedReason: String = "",
    val result: String = "",
    val error: String = "",
    val pending: Int = 0,
    val rebootId: String = "",
)

var softRebootStatus by mutableStateOf(SoftRebootStatus())
    private set

private suspend fun querySoftRebootStatus(): SoftRebootStatus = withContext(Dispatchers.IO) {
    runCatching {
        val root = JSONObject(ksudReadString("soft-reboot-status --json"))
        check(root.getInt("interface_version") == 1)
        val last = root.optJSONObject("last") ?: JSONObject()
        SoftRebootStatus(
            known = true,
            supported = root.getBoolean("supported"),
            running = root.getBoolean("running"),
            blockedReason = root.optString("blocked_reason"),
            result = last.optString("result"),
            error = last.optString("error"),
            pending = last.optInt("pending"),
            rebootId = "${last.optString("boot_id")}:${last.optLong("pid")}",
        )
    }.getOrElse { SoftRebootStatus(known = true) }
}

fun refreshSoftRebootStatus() {
    if (refreshJob?.isActive == true || rebootJob?.isActive == true) return
    refreshJob = softRebootScope.launch { softRebootStatus = querySoftRebootStatus() }
}

fun softRebootBlockedMessage(context: Context = ksuApp): String = when {
    !softRebootStatus.known -> context.getString(R.string.soft_reboot_checking)
    softRebootStatus.running -> context.getString(R.string.soft_reboot_accepted)
    softRebootStatus.blockedReason.isNotBlank() ->
        context.getString(R.string.soft_reboot_unavailable_reason, softRebootStatus.blockedReason)
    else -> context.getString(R.string.soft_reboot_unavailable)
}

fun softRebootResultMessage(
    context: Context = ksuApp,
    status: SoftRebootStatus = softRebootStatus,
): String? = when {
    status.running -> context.getString(R.string.soft_reboot_accepted)
    status.result == "completed" -> context.getString(R.string.soft_reboot_completed)
    status.result == "completed_pending" ->
        context.resources.getQuantityString(
            R.plurals.soft_reboot_pending, status.pending, status.pending,
        )
    status.result == "failed" -> buildString {
        append(context.getString(R.string.soft_reboot_failed, status.error))
        if (status.pending > 0) {
            append('\n')
            append(context.resources.getQuantityString(
                R.plurals.soft_reboot_pending, status.pending, status.pending,
            ))
        }
    }
    else -> null
}

fun requestSoftReboot() {
    if (rebootJob?.isActive == true) return
    refreshJob?.cancel()
    rebootJob = softRebootScope.launch {
        softRebootStatus = querySoftRebootStatus()
        if (!softRebootStatus.supported) {
            Toast.makeText(ksuApp, softRebootBlockedMessage(), Toast.LENGTH_LONG).show()
            return@launch
        }
        val accepted = withContext(Dispatchers.IO) {
            runCatching { execKsud("soft-reboot", newShell = true) }.getOrDefault(false)
        }
        if (accepted) {
            softRebootStatus = softRebootStatus.copy(supported = false, running = true)
            Toast.makeText(ksuApp, R.string.soft_reboot_accepted, Toast.LENGTH_LONG).show()
        } else {
            softRebootStatus = querySoftRebootStatus()
            Toast.makeText(
                ksuApp,
                if (softRebootStatus.result == "failed") softRebootResultMessage()
                    else ksuApp.getString(R.string.soft_reboot_rejected),
                Toast.LENGTH_LONG,
            ).show()
        }
    }
}

fun isSoftRebootEnabled(context: Context = ksuApp): Boolean =
    context.getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE)
        .getBoolean(KEY_USE_SOFT_REBOOT, false)

fun setSoftRebootEnabled(context: Context, enabled: Boolean) {
    context.getSharedPreferences(SETTINGS_PREFS, Context.MODE_PRIVATE).edit {
        putBoolean(KEY_USE_SOFT_REBOOT, enabled)
    }
}

fun isSoftRebootPreferred(context: Context = ksuApp): Boolean =
    !isSoftRebootBlockedByKasumi() &&
        (runCatching { Natives.isLateLoadMode }.getOrDefault(false) || isSoftRebootEnabled(context))

fun isSoftRebootBlockedByKasumi(): Boolean = !softRebootStatus.supported
