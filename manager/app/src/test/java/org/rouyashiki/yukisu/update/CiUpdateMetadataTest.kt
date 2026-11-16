package org.rouyashiki.yukisu.update

import android.app.Application
import android.content.Context
import java.lang.reflect.InvocationTargetException
import org.json.JSONObject
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.util.ReflectionHelpers
import org.rouyashiki.yukisu.KernelSUApplication
import org.rouyashiki.yukisu.ksuApp
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class CiUpdateMetadataTest {
    @Before
    fun prepareMessageResources() {
        ksuApp = KernelSUApplication().apply {
            ReflectionHelpers.callInstanceMethod<Void>(
                this,
                "attachBaseContext",
                ReflectionHelpers.ClassParameter.from(
                    Context::class.java,
                    RuntimeEnvironment.getApplication(),
                ),
            )
        }
    }

    @Test
    fun acceptsMetadataFromTheCurrentRepository() {
        val run = parseMetadata("Rouyashiki/YukiSU")

        assertEquals(123L, run.runId)
        assertEquals(10_800, run.versionCode)
        assertEquals("a".repeat(40), run.commitSha)
        assertEquals("b".repeat(64), run.apkSha256)
        assertEquals(1_024L, run.apkSize)
    }

    @Test
    fun rejectsMetadataFromTheFormerRepositoryName() {
        assertRejected("Anatdx/YukiSU")
    }

    @Test
    fun rejectsMetadataFromAnotherRepository() {
        assertRejected("example/YukiSU")
    }

    private fun parseMetadata(repository: String): CiRun {
        val body = JSONObject()
            .put("schema_version", 1)
            .put("repository", repository)
            .put("workflow", "build-manager.yml")
            .put("branch", "main")
            .put("run_id", 123L)
            .put("version_code", 10_800)
            .put("commit_sha", "a".repeat(40))
            .put("commit_message", "Update metadata fixture")
            .put("apk_sha256", "b".repeat(64))
            .put("apk_size", 1_024L)
            .toString()
        return CiUpdateManager::class.java.getDeclaredMethod(
            "parseSignedCiMetadata", String::class.java,
        ).apply { isAccessible = true }.invoke(CiUpdateManager, body) as CiRun
    }

    private fun assertRejected(repository: String) {
        val failure = assertFailsWith<InvocationTargetException> { parseMetadata(repository) }
        assertIs<IllegalStateException>(failure.targetException)
    }
}
