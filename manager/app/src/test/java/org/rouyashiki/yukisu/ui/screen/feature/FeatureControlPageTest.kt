package org.rouyashiki.yukisu.ui.screen.feature

import android.app.Application
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeveloperMode
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isEnabled
import androidx.compose.ui.test.isNotEnabled
import androidx.compose.ui.test.isOff
import androidx.compose.ui.test.isToggleable
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.rouyashiki.yukisu.Natives
import org.rouyashiki.yukisu.R
import org.rouyashiki.yukisu.ui.screen.FeatureControlSetting
import org.rouyashiki.yukisu.ui.screen.SettingsKernelState
import org.rouyashiki.yukisu.ui.screen.rememberSettingsKernelState
import org.rouyashiki.yukisu.ui.screen.rememberSettingsScrollState
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class FeatureControlPageTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun settingsRestoresTheBottomAfterManagerAndLogRowsReturn() = checkSettingsScroll(scrollWhileLoading = false)

    @Test
    fun settingsKeepsANewScrollDuringTheManagerProbe() = checkSettingsScroll(scrollWhileLoading = true)

    private fun checkSettingsScroll(scrollWhileLoading: Boolean) {
        val settingsOpen = mutableStateOf(true)
        val kernel = mutableStateOf<SettingsKernelState?>(SettingsKernelState(manager = true, suLog = true))
        lateinit var scroll: ScrollState
        compose.setContent {
            val holder = rememberSaveableStateHolder()
            if (settingsOpen.value) holder.SaveableStateProvider("settings") {
                val state = kernel.value
                scroll = rememberSettingsScrollState(state)
                Box(Modifier.height(100.dp)) {
                    Column(Modifier.verticalScroll(scroll)) {
                        Spacer(Modifier.height(200.dp))
                        if (state?.manager == true) Spacer(Modifier.height(300.dp))
                        if (state?.suLog == true) Spacer(Modifier.height(100.dp))
                    }
                }
            }
        }
        val bottom = compose.runOnIdle {
            runBlocking { scroll.scrollTo(scroll.maxValue) }
            scroll.value
        }
        compose.runOnIdle { settingsOpen.value = false }
        compose.runOnIdle {
            kernel.value = null
            settingsOpen.value = true
        }
        val expected = compose.runOnIdle {
            assertTrue(scroll.maxValue in 1 until bottom)
            val offset = if (scrollWhileLoading) {
                runBlocking { scroll.scrollTo(scroll.maxValue / 2) }
                scroll.value
            } else bottom
            kernel.value = SettingsKernelState(manager = true)
            offset
        }
        compose.runOnIdle {
            assertTrue(scroll.maxValue in 1 until bottom)
            kernel.value = SettingsKernelState(manager = true, suLog = true)
        }
        compose.runOnIdle { assertEquals(expected, scroll.value) }
    }

    @Test
    fun backAndReentryWorkWhileTheFeatureQueryIsPending() {
        val background = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val queue = FeatureReadQueue(background) { true }
        val calls = AtomicInteger()
        val release = CompletableDeferred<Unit>()
        val query = FeatureQuery {
            calls.incrementAndGet()
            release.await()
            false
        }
        val settingsQuery = FeatureQuery { false to false }
        val open = mutableStateOf(false)
        val resources = RuntimeEnvironment.getApplication().resources
        val back = resources.getString(R.string.back)
        val toggle = resources.getString(R.string.kasumi_feature_title)
        val entry = resources.getString(R.string.feature_control_summary)
        fun awaitSettingsEntry() {
            compose.waitUntil(5_000) { compose.onAllNodesWithText(entry).fetchSemanticsNodes().isNotEmpty() }
        }
        try {
            compose.setContent {
                MaterialTheme {
                    if (open.value) {
                        FeatureControlPage(onBack = { open.value = false }, load = { queue.read(query) })
                    } else {
                        val kernel by rememberSettingsKernelState(queue::isManager, { false }) { queue.read(settingsQuery) }
                        if (kernel?.manager == true) FeatureControlSetting { open.value = true }
                    }
                }
            }
            awaitSettingsEntry()
            compose.onNodeWithText(entry).performClick()
            compose.waitUntil { calls.get() == 1 }
            compose.onNodeWithText(toggle).assertDoesNotExist()
            compose.onNodeWithContentDescription(back).performClick()
            awaitSettingsEntry()
            compose.onNodeWithText(entry).performClick()
            compose.onNodeWithContentDescription(back).assertExists()
            compose.onNodeWithText(toggle).assertDoesNotExist()
            assertEquals(1, calls.get())
            release.complete(Unit)
            compose.waitForIdle()
            compose.onNodeWithContentDescription(back).performClick()
            awaitSettingsEntry()
        } finally {
            release.complete(Unit)
            background.cancel()
        }
    }

    @Test
    fun settingsAndFeaturePageRemainReachableDuringAWrite() {
        val background = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        val queue = FeatureReadQueue(background) { true }
        val writing = CompletableDeferred<Unit>()
        val finish = CompletableDeferred<Unit>()
        val detailReads = AtomicInteger()
        val details = FeatureQuery(blocksWrites = false) {
            detailReads.incrementAndGet()
            false to true
        }
        val open = mutableStateOf(true)
        val resources = RuntimeEnvironment.getApplication().resources
        val back = resources.getString(R.string.back)
        val row = resources.getString(R.string.kasumi_feature_title)
        val entry = resources.getString(R.string.feature_control_summary)
        try {
            compose.setContent {
                if (open.value) {
                    FeatureControlPage(
                        onBack = { open.value = false },
                        load = queue::isManager,
                        readFeature = { FeatureReading(null, "unsupported") },
                        initialization = FeatureQuery(blocksWrites = false) { false },
                    )
                } else {
                    val kernel by rememberSettingsKernelState(queue::isManager, { false }) { queue.read(details) }
                    if (kernel?.manager == true) FeatureControlSetting { open.value = true }
                }
            }
            compose.waitUntil(5_000) { compose.onAllNodesWithText(row).fetchSemanticsNodes().isNotEmpty() }
            background.launch {
                queue.change {
                    writing.complete(Unit)
                    finish.await()
                }
            }
            compose.waitUntil { writing.isCompleted }
            compose.onNodeWithContentDescription(back).performClick()
            compose.waitUntil(5_000) { compose.onAllNodesWithText(entry).fetchSemanticsNodes().isNotEmpty() }
            compose.onNodeWithText(entry).performClick()
            compose.waitUntil(5_000) { compose.onAllNodesWithText(row).fetchSemanticsNodes().isNotEmpty() }
            assertEquals(0, detailReads.get())
            finish.complete(Unit)
            compose.onNodeWithContentDescription(back).performClick()
            compose.waitUntil(5_000) { detailReads.get() > 0 }
        } finally {
            finish.complete(Unit)
            background.cancel()
        }
    }

    @Test
    fun readySwitchIsUsableWhileAnotherFeatureAndKasumiAreLoading() {
        val slowFeature = CompletableDeferred<FeatureReading>()
        val slowKasumi = CompletableDeferred<Boolean>()
        val resources = RuntimeEnvironment.getApplication().resources
        val adb = resources.getString(R.string.settings_adb_root)
        val slow = resources.getString(R.string.hide_bl_title)
        compose.setContent {
            FeatureControlPage(
                onBack = {},
                load = { true },
                readFeature = {
                    when (it) {
                        Natives.FEATURE_HIDE_BOOTLOADER -> slowFeature.await()
                        Natives.FEATURE_ADB_ROOT -> FeatureReading(false, "supported")
                        else -> FeatureReading(null, "unsupported")
                    }
                },
                initialization = FeatureQuery(blocksWrites = false) { slowKasumi.await() },
            )
        }
        compose.onNodeWithText(adb).assertIsEnabled().assertIsOff()
        compose.onNodeWithText(slow).assertExists()
        val disabled = resources.getString(R.string.hide_bl_disabled)
        compose.onNodeWithText(disabled).assertDoesNotExist()
        compose.onAllNodes(isToggleable()).assertCountEquals(1)
        slowFeature.complete(FeatureReading(false, "supported"))
        compose.onNodeWithText(slow).assertIsEnabled().assertIsOff()
        compose.onNodeWithText(disabled).assertExists()
        slowKasumi.complete(true)
    }

    @Test
    fun supportedSwitchRecoversWhenItsRelatedModeProvidesAValue() {
        val resources = RuntimeEnvironment.getApplication().resources
        val title = resources.getString(R.string.su_compat_magisk_title)
        lateinit var magisk: FeatureToggleState
        compose.setContent {
            magisk = rememberFeatureToggleState(Natives.FEATURE_MAGISK_COMPAT, { FeatureReading(null, "supported") })
            FeatureSwitchItem(Icons.Filled.DeveloperMode, title, "", magisk) {}
        }
        compose.onNodeWithText(resources.getString(R.string.feature_state_unavailable)).assertExists()
        compose.runOnIdle { magisk.checked = false }
        compose.onNodeWithText(title).assertIsEnabled().assertIsOff()
    }

    private fun showSuCompatPage(
        initial: Map<Int, Boolean>,
        controller: SuCompatController,
        read: (Int) -> Boolean?,
    ) {
        compose.setContent {
            FeatureControlPage(
                onBack = {},
                load = { true },
                readFeature = { id ->
                    initial[id]?.let { FeatureReading(it, "supported") } ?: FeatureReading(null, "unsupported")
                },
                initialization = FeatureQuery(blocksWrites = false) { true },
                suCompatController = controller,
                readSuCompatValue = read,
            )
        }
    }

    @Test
    fun suModeWaitsForMagiskProbeAndCanSaveWhenUnsupported() = checkMagiskProbe("unsupported")

    @Test
    fun suModeStaysDisabledAfterMagiskQueryFails() = checkMagiskProbe("unknown")

    private fun checkMagiskProbe(status: String) {
        val magiskProbe = CompletableDeferred<FeatureReading>()
        val writes = mutableListOf<Pair<String, Boolean>>()
        var traditional = true
        val controller = SuCompatController { name, enabled ->
            writes.add(name to enabled)
            traditional = enabled
            true
        }
        compose.setContent {
            FeatureControlPage(
                onBack = {},
                load = { true },
                readFeature = { id ->
                    when (id) {
                        Natives.FEATURE_SU_COMPAT -> FeatureReading(true, "supported")
                        Natives.FEATURE_MAGISK_COMPAT -> magiskProbe.await()
                        else -> FeatureReading(null, "unsupported")
                    }
                },
                initialization = FeatureQuery(blocksWrites = false) { false },
                suCompatController = controller,
                readSuCompatValue = { id -> if (id == Natives.FEATURE_SU_COMPAT) traditional else null },
            )
        }
        val resources = RuntimeEnvironment.getApplication().resources
        val title = resources.getString(R.string.su_compat_title)
        val current = hasText(title) and hasText(resources.getString(R.string.su_compat_traditional))
        compose.onNode(current).performScrollTo().assertIsNotEnabled()

        magiskProbe.complete(FeatureReading(null, status))
        if (status == "unknown") {
            val unavailable = resources.getString(R.string.feature_state_unavailable)
            compose.waitUntil(5_000) { compose.onAllNodesWithText(unavailable).fetchSemanticsNodes().isNotEmpty() }
            compose.onNode(current).assertIsNotEnabled()
            assertTrue(writes.isEmpty())
            return
        }
        compose.onNode(current).assertIsEnabled().performClick()
        val off = resources.getString(R.string.su_compat_off)
        compose.onNodeWithText(off).performClick()
        val saved = hasText(title) and hasText(off) and isEnabled()
        compose.waitUntil(5_000) { compose.onAllNodes(saved).fetchSemanticsNodes().isNotEmpty() }
        assertEquals(listOf("su_compat" to false), writes)
    }

    @Test
    fun savedSuModeRecoversAfterOneMissingRead() = checkSuModeReadback(false)

    @Test
    fun savedSuModeStaysUnknownAfterTwoFailedReads() = checkSuModeReadback(null)

    private fun checkSuModeReadback(confirmed: Boolean?) {
        val traditional = Natives.FEATURE_SU_COMPAT
        val ksm = Natives.FEATURE_KASUMI_SUCOMPAT
        val magisk = Natives.FEATURE_MAGISK_COMPAT
        val writes = mutableListOf<Pair<String, Boolean>>()
        val reads = ConcurrentHashMap<Int, Int>()
        var runtime = true
        val controller = SuCompatController { name, enabled ->
            writes.add(name to enabled)
            runtime = enabled
            true
        }
        val initial = mapOf(traditional to true, magisk to false)
        showSuCompatPage(initial, controller) { id ->
            if (writes.isEmpty()) return@showSuCompatPage initial[id]
            val attempt = reads.merge(id, 1, Int::plus)
            when (id) {
                traditional -> when {
                    attempt == 1 -> null
                    confirmed == null -> throw IOException("readback")
                    else -> runtime
                }
                magisk -> false
                else -> null
            }
        }
        val resources = RuntimeEnvironment.getApplication().resources
        val title = resources.getString(R.string.su_compat_title)
        val off = resources.getString(R.string.su_compat_off)
        compose.onNodeWithText(title).performScrollTo().performClick()
        compose.onNodeWithText(off).performClick()
        val summary = if (confirmed == null) resources.getString(R.string.feature_state_unavailable) else off
        val row = hasText(title) and hasText(summary) and if (confirmed == null) isNotEnabled() else isEnabled()
        compose.waitUntil(5_000) {
            reads[magisk] == 1 && compose.onAllNodes(row).fetchSemanticsNodes().isNotEmpty()
        }
        if (confirmed == null) compose.onNode(row).assertIsNotEnabled() else compose.onNode(row).assertIsEnabled()
        compose.onNodeWithText(resources.getString(R.string.setting_change_failed)).assertDoesNotExist()
        assertEquals(listOf("su_compat" to false), writes)
        assertEquals(mapOf(traditional to 2, ksm to 1, magisk to 1), reads)
    }

    @Test
    fun conflictingSuModeRollsBackEvenIfAnotherFieldIsUnknown() = runBlocking {
        val initial = SuCompatSnapshot(traditional = true, ksm = false, magisk = false)
        var runtime = initial
        val writes = mutableListOf<Pair<String, Boolean>>()
        val controller = SuCompatController { name, enabled ->
            writes.add(name to enabled)
            runtime = when (name) {
                "kasumi_sucompat" -> initial.copy(ksm = null)
                "su_compat" -> initial
                else -> error("Unexpected feature $name")
            }
            true
        }
        assertEquals(false to initial, controller.change(SuCompatMode.KSM, null) { runtime })
        assertEquals(listOf("kasumi_sucompat" to true, "su_compat" to true), writes)
    }

    @Test
    fun savedMagiskChangeRetriesOnlyTheMissingValues() {
        val traditional = Natives.FEATURE_SU_COMPAT
        val ksm = Natives.FEATURE_KASUMI_SUCOMPAT
        val magisk = Natives.FEATURE_MAGISK_COMPAT
        val writes = mutableListOf<Pair<String, Boolean>>()
        val reads = ConcurrentHashMap<Int, Int>()
        var runtime = true
        val controller = SuCompatController { name, enabled ->
            writes.add(name to enabled)
            runtime = enabled
            true
        }
        val initial = mapOf(traditional to false, ksm to true, magisk to true)
        showSuCompatPage(initial, controller) { id ->
            if (writes.isEmpty()) return@showSuCompatPage initial[id]
            val attempt = reads.merge(id, 1, Int::plus)
            when {
                id != traditional && attempt == 1 -> null
                id == magisk -> runtime
                else -> initial[id]
            }
        }
        val title = RuntimeEnvironment.getApplication().getString(R.string.su_compat_magisk_title)
        compose.onNodeWithText(title).performScrollTo().performClick()
        val saved = hasText(title) and isEnabled() and isOff()
        compose.waitUntil(5_000) { reads[magisk] == 2 && compose.onAllNodes(saved).fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithText(title).assertIsEnabled().assertIsOff()
        assertEquals(listOf("magisk_compat" to false), writes)
        assertEquals(mapOf(traditional to 1, ksm to 2, magisk to 2), reads)
    }

    @Test
    fun savedFeatureIsConfirmedWhileAnUnrelatedWriteIsPending() {
        val state = FeatureToggleState(true, "supported")
        val firstStarted = CompletableDeferred<Unit>()
        val releaseFirst = CompletableDeferred<Unit>()
        val secondStarted = CompletableDeferred<Unit>()
        val releaseSecond = CompletableDeferred<Unit>()
        val saved = CompletableDeferred<Unit>()
        var value = true
        lateinit var scope: CoroutineScope
        compose.setContent { scope = rememberCoroutineScope() }
        try {
            compose.runOnIdle {
                scope.persistFeature(
                    state, Natives.FEATURE_ADB_ROOT, "adb_root", false,
                    readValue = { value },
                    writeValue = {
                        firstStarted.complete(Unit)
                        releaseFirst.await()
                        value = it
                        true
                    },
                    onSuccess = { saved.complete(Unit) },
                )
            }
            compose.waitUntil(5_000) { firstStarted.isCompleted }
            compose.runOnIdle {
                scope.launch(start = CoroutineStart.UNDISPATCHED) {
                    featureReads.change {
                        secondStarted.complete(Unit)
                        releaseSecond.await()
                    }
                }
                releaseFirst.complete(Unit)
            }
            compose.waitUntil(5_000) { secondStarted.isCompleted }
            compose.waitUntil(5_000) { saved.isCompleted }
            compose.runOnIdle {
                assertEquals(false, state.checked)
                assertEquals(false, state.saving)
            }
        } finally {
            releaseFirst.complete(Unit)
            releaseSecond.complete(Unit)
        }
    }

    @Test
    fun successfulWriteShowsConfirmedOffAfterRetry() = checkReadbackAfterSave(false)

    @Test
    fun successfulWriteStaysUnknownWhenRetryCannotRead() = checkReadbackAfterSave(null)

    private fun checkReadbackAfterSave(confirmed: Boolean?) {
        val state = FeatureToggleState(true, "supported")
        val writes = mutableListOf<Boolean>()
        val restarts = AtomicInteger()
        val reads = AtomicInteger()
        val retryStarted = CompletableDeferred<Unit>()
        val releaseRetry = CompletableDeferred<Boolean?>()
        val result = mutableStateOf<Boolean?>(null)
        val resources = RuntimeEnvironment.getApplication().resources
        val title = resources.getString(R.string.settings_adb_root)
        val unknown = resources.getString(R.string.feature_state_unavailable)
        try {
            compose.setContent {
                val scope = rememberCoroutineScope()
                FeatureSwitchItem(Icons.Filled.DeveloperMode, title, "", state) { enabled ->
                    scope.persistFeature(
                        state, Natives.FEATURE_ADB_ROOT, "adb_root", enabled,
                        writeValue = {
                            writes.add(it)
                            true
                        },
                        afterPersist = {
                            restarts.incrementAndGet()
                            true
                        },
                        readValue = {
                            when (reads.incrementAndGet()) {
                                1 -> true
                                2 -> null
                                else -> {
                                    retryStarted.complete(Unit)
                                    releaseRetry.await()
                                }
                            }
                        },
                        onSuccess = { result.value = true },
                        onFailure = { result.value = false },
                    )
                }
            }
            compose.onNodeWithText(title).performClick()
            compose.waitUntil { retryStarted.isCompleted }
            compose.onNodeWithText(unknown).assertExists()
            compose.onAllNodes(isToggleable()).assertCountEquals(0)
            releaseRetry.complete(confirmed)
            compose.waitUntil { result.value != null }
            assertEquals(true, result.value)
            assertEquals(listOf(false), writes)
            assertEquals(1, restarts.get())
            assertEquals(3, reads.get())
            if (confirmed == null) {
                compose.onNodeWithText(unknown).assertExists()
                compose.onAllNodes(isToggleable()).assertCountEquals(0)
            } else {
                compose.onNodeWithText(title).assertIsOff()
            }
        } finally {
            releaseRetry.complete(null)
        }
    }
}
