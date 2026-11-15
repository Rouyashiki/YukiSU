package org.rouyashiki.yukisu.ui.screen.yukizygisk

import android.app.Application
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36], application = Application::class)
class ZygiskStatusParsingTest {
    private fun field(value: Any, name: String): Any? =
        value.javaClass.getDeclaredField(name).apply { isAccessible = true }.get(value)

    private fun modules(capability: Boolean, records: String): List<*> {
        val parse = Class.forName("org.rouyashiki.yukisu.ui.screen.yukizygisk.YukiZygiskScreenKt")
            .getDeclaredMethod("parseYzStatus", String::class.java).apply { isAccessible = true }
        val status = requireNotNull(parse.invoke(null, """
            {"zygisk_module_monitor":true,"zygisk_module_outcomes":$capability,
             "modules":["zygisk_vector"],
             "zygote_monitor":[{"pid":17,"generation":3,"abi":"arm64-v8a","state":"injected"}],
             "runtime":[$records]}
        """.trimIndent()))
        val zygote = (field(status, "zygotes") as List<*>).single()!!
        return field(zygote, "modules") as List<*>
    }

    private fun record(generation: Int = 3, abi: String = "arm64-v8a", process: String = "running") = """
        {"pid":17,"generation":$generation,"abi":"$abi","kind":"zygote",
         "module":"zygisk_vector","state":"failed","process_state":"$process",
         "load_success_seen":true,"load_failure_seen":true}
    """.trimIndent()

    @Test
    fun mixedOutcomeRemainsVisibleInTheCurrentZygote() {
        val rows = modules(true, listOf(record(), record(2), record(abi = "armeabi-v7a"),
            record(process = "exited")).joinToString(","))
        val row = rows.single()!!
        assertEquals("zygisk_vector", field(row, "name"))
        assertTrue((field(row, "outcome") as ZygiskLoadOutcome).partial)
    }

    @Test
    fun oldGenerationOtherAbiAndExitedReportsAreNotCurrent() {
        assertTrue(modules(true, listOf(record(2), record(abi = "armeabi-v7a"),
            record(process = "exited")).joinToString(",")).isEmpty())
    }

    @Test
    fun legacyFailureRemainsVisibleWithoutInventingSuccess() {
        val row = modules(false, record()).single()!!
        val outcome = field(row, "outcome") as ZygiskLoadOutcome
        assertTrue(outcome.failure)
        assertFalse(outcome.success)
        assertFalse(outcome.complete)
    }
}
