package org.rouyashiki.yukisu.ui.screen.feature

import android.app.Application
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.rouyashiki.yukisu.R

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class SuPathSettingTest {
    @get:Rule
    val compose = createComposeRule()

    private val requested = "/system/bin/custom-su"
    private val refresh = mutableIntStateOf(0)
    private val saves = CopyOnWriteArrayList<String>()
    private val reads = AtomicInteger()
    private val resources get() = RuntimeEnvironment.getApplication().resources

    private fun submit(reply: String = "none", readAfterSave: (Int) -> String?) {
        val settled = CompletableDeferred<Unit>()
        val query = FeatureQuery(blocksWrites = false) {
            if (saves.isEmpty()) DEFAULT_SU_PATH else readAfterSave(reads.incrementAndGet())
        }
        compose.setContent {
            MaterialTheme {
                SuPathSetting(true, refresh.intValue, { if (!it) settled.complete(Unit) }, query) { path ->
                    saves.add(path)
                    """{"version":1,"error":"$reply"}""".toByteArray()
                }
            }
        }
        waitForText(DEFAULT_SU_PATH)
        compose.onNodeWithText(DEFAULT_SU_PATH).performClick()
        compose.onNode(hasSetTextAction()).performTextReplacement(requested)
        compose.onNodeWithText(resources.getString(R.string.app_profile_template_save)).performClick()
        compose.waitUntil(5_000) { settled.isCompleted }
    }

    private fun waitForText(text: String) = compose.waitUntil(5_000) {
        compose.onAllNodesWithText(text).fetchSemanticsNodes().isNotEmpty()
    }

    @Test
    fun acknowledgedPathRecoversAfterOneMissingRead() {
        submit { if (it == 1) null else requested }
        compose.onNodeWithText(requested).assertIsEnabled()
        compose.onNode(hasSetTextAction()).assertDoesNotExist()
        assertEquals(2, reads.get())
        assertEquals(listOf(requested), saves.toList())
    }

    @Test
    fun acknowledgedPathStaysUnknownUntilARefreshConfirmsIt() {
        submit { if (it <= 2) null else requested }
        compose.onNodeWithText(resources.getString(R.string.feature_state_unavailable)).assertIsNotEnabled()
        compose.onNodeWithText(DEFAULT_SU_PATH).assertDoesNotExist()
        compose.onNode(hasSetTextAction()).assertDoesNotExist()
        assertEquals(2, reads.get())
        compose.runOnIdle { refresh.intValue++ }
        waitForText(requested)
        compose.onNodeWithText(requested).assertIsEnabled()
        assertEquals(listOf(requested), saves.toList())
    }

    @Test
    fun conflictingPathReportsFailureAndKeepsTheDialogOpen() {
        submit { DEFAULT_SU_PATH }
        compose.onNodeWithText(resources.getString(R.string.setting_change_failed)).assertExists()
        compose.onNode(hasSetTextAction()).assertExists()
        assertEquals(1, reads.get())
        assertEquals(listOf(requested), saves.toList())
    }

    @Test
    fun daemonRejectionKeepsItsSpecificError() {
        submit("path_exists") { null }
        compose.onNodeWithText(resources.getString(R.string.su_compat_path_error_exists)).assertExists()
        compose.onNode(hasSetTextAction()).assertExists()
        compose.onNodeWithText(resources.getString(R.string.cancel)).performClick()
        compose.onNodeWithText(DEFAULT_SU_PATH).assertIsEnabled()
        assertEquals(1, reads.get())
        assertEquals(listOf(requested), saves.toList())
    }
}
