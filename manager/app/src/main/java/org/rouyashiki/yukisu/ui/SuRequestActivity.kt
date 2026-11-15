package org.rouyashiki.yukisu.ui

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.os.SystemClock
import android.widget.Toast
import android.view.Window
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.addCallback
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.theme.KernelSUTheme
import kotlin.math.roundToInt
import kotlin.concurrent.thread
import kotlinx.coroutines.delay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class SuRequestActivity : ComponentActivity() {

    private var reqId = 0L
    private var nonce = 0L
    private var requestPackage = ""

    @Volatile
    private var replied = false

    enum class Choice(val wire: Int) {
        ALLOW_FOREVER(1),
        ALLOW_ONCE(2),
        DENY(3),
        DENY_HIDE(4),
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LOCKED
        window.requestFeature(Window.FEATURE_NO_TITLE)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.addFlags(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)
        try {
            window.setHideOverlayWindows(true)
        } catch (_: SecurityException) {
            // Best effort on vendor frameworks.
        }
        super.onCreate(savedInstanceState)
        val screenWidth = windowManager.currentWindowMetrics.bounds.width()
        setFinishOnTouchOutside(false)
        onBackPressedDispatcher.addCallback(this) { onChoice(Choice.DENY) }

        reqId = intent.getLongExtra(EXTRA_REQ_ID, 0L)
        nonce = intent.getLongExtra(EXTRA_NONCE, 0L)
        val uid = intent.getIntExtra(EXTRA_UID, -1)
        val comm = intent.getStringExtra(EXTRA_COMM).orEmpty()

        if (reqId == 0L || nonce == 0L || uid < 0) {
            finish()
            return
        }

        val pkg = runCatching {
            packageManager.getPackagesForUid(uid)?.firstOrNull()
        }.getOrNull()
        requestPackage = pkg.orEmpty()
        val label = pkg?.let { p ->
            runCatching {
                packageManager.getApplicationLabel(
                    packageManager.getApplicationInfo(p, 0)
                ).toString()
            }.getOrNull()
        } ?: comm.ifEmpty { uid.toString() }
        val icon: ImageBitmap? = pkg?.let { p ->
            runCatching {
                packageManager.getApplicationIcon(p).toBitmap(144, 144).asImageBitmap()
            }.getOrNull()
        }

        setContent {
            KernelSUTheme(showBackground = false) {
                Box(
                    modifier = Modifier.padding(12.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    SuRequestCard(
                        appLabel = label,
                        packageName = pkg ?: comm,
                        uid = uid,
                        icon = icon,
                        canPersist = requestPackage.isNotEmpty(),
                        onReady = { Natives.suPromptReady(reqId, nonce) },
                        onStale = ::finishStaleRequest,
                        onChoice = ::onChoice,
                    )
                }
            }
        }
        window.decorView.post {
            window.setLayout(
                (screenWidth * 0.85f).roundToInt(),
                WindowManager.LayoutParams.WRAP_CONTENT,
            )
        }
    }

    private fun onChoice(choice: Choice) {
        if (replied) return
        replied = true
        val wire = choice.wire
        thread {
            val submitted = reply(wire)
            runOnUiThread {
                if (!submitted) showRequestFailure()
                finishAndRemoveTask()
            }
        }
    }

    private fun showRequestFailure() {
        Toast.makeText(this, R.string.su_request_failed, Toast.LENGTH_SHORT).show()
    }

    private fun finishStaleRequest() {
        replied = true
        showRequestFailure()
        finishAndRemoveTask()
    }

    override fun onDestroy() {
        // Fail closed if the window disappears without a choice.
        if (!replied && !isChangingConfigurations) {
            replied = true
            thread { reply(Choice.DENY.wire) }
        }
        super.onDestroy()
    }

    private fun reply(choice: Int): Boolean =
        Natives.submitSuPrompt(reqId, nonce, choice, requestPackage)

    companion object {
        const val EXTRA_REQ_ID = "ksu.req_id"
        const val EXTRA_UID = "ksu.uid"
        const val EXTRA_COMM = "ksu.comm"
        const val EXTRA_NONCE = "ksu.nonce"
    }
}

private const val COUNTDOWN_SECONDS = 10

@Composable
private fun SuRequestCard(
    appLabel: String,
    packageName: String,
    uid: Int,
    icon: ImageBitmap?,
    canPersist: Boolean,
    onReady: () -> Int,
    onStale: () -> Unit,
    onChoice: (SuRequestActivity.Choice) -> Unit,
) {
    var remaining by remember { mutableIntStateOf(COUNTDOWN_SECONDS) }
    var ready by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        val started = SystemClock.elapsedRealtime()
        val budget = withContext(Dispatchers.IO) { onReady() }
        if (budget <= 2000) {
            onStale()
            return@LaunchedEffect
        }
        val deadline = started + minOf(COUNTDOWN_SECONDS * 1000, budget - 2000)
        ready = true
        while (true) {
            val millisLeft = deadline - SystemClock.elapsedRealtime()
            remaining = ((millisLeft.coerceAtLeast(0) + 999) / 1000).toInt()
            if (millisLeft <= 0) break
            delay(minOf(250L, millisLeft))
        }
        onChoice(SuRequestActivity.Choice.DENY)
    }

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .widthIn(max = 420.dp),
        shape = MaterialTheme.shapes.large,
        tonalElevation = 6.dp,
    ) {
        Column(
            modifier = Modifier.padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                if (icon != null) {
                    Image(
                        bitmap = icon,
                        contentDescription = null,
                        modifier = Modifier.size(48.dp),
                    )
                }
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(
                        text = appLabel,
                        style = MaterialTheme.typography.titleLarge,
                    )
                    if (packageName.isNotEmpty() && packageName != appLabel) {
                        Text(
                            text = packageName,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Text(
                        text = "UID $uid",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Text(
                text = stringResource(R.string.su_request_desc),
                style = MaterialTheme.typography.bodyMedium,
            )

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { onChoice(SuRequestActivity.Choice.ALLOW_FOREVER) },
                    modifier = Modifier.weight(1f),
                    enabled = ready && canPersist,
                ) { Text(stringResource(R.string.su_request_allow_forever)) }
                FilledTonalButton(
                    onClick = { onChoice(SuRequestActivity.Choice.ALLOW_ONCE) },
                    modifier = Modifier.weight(1f),
                    enabled = ready,
                ) { Text(stringResource(R.string.su_request_allow_once)) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { onChoice(SuRequestActivity.Choice.DENY) },
                    modifier = Modifier.weight(1f),
                ) { Text("${stringResource(R.string.su_request_deny)}（${remaining}s）") }
                OutlinedButton(
                    onClick = { onChoice(SuRequestActivity.Choice.DENY_HIDE) },
                    modifier = Modifier.weight(1f),
                    enabled = ready && canPersist,
                ) { Text(stringResource(R.string.su_request_deny_hide)) }
            }
        }
    }
}
