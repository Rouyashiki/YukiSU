package com.anatdx.yukisu.ui.viewmodel

import android.app.Application
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class PluginCompatibilityTest {
    @Test
    fun preservesConfigurationKeysAndCallbackMetadata() {
        val plugin = PluginViewModel().parsePlugins(
            """[{"id":"legacy","has_manifest":true,"action_unknown":true,
                "callbacks":["tick.v1","worker:1"],"config":[
                {"key":" 配置: key ","default":4},
                {"key":"${"a".repeat(64)}"},
                {"key":"${"a".repeat(65)}"},
                {"key":"bad/key"}]}]"""
        ).single()
        assertTrue(plugin.actionUnknown)
        assertFalse(plugin.hasAction)
        assertEquals(listOf("tick.v1", "worker:1"), plugin.callbacks)
        assertEquals(listOf(" 配置: key ", "a".repeat(64)), plugin.config.map { it.key })
        assertEquals("4", plugin.config.first().default)
    }

    @Test
    fun unchangedDisplayedDefaultsDoNotBecomeSavedValues() {
        assertEquals(
            PluginConfigChanges(emptyMap(), emptySet()),
            pluginConfigChanges(emptyMap(), emptyMap(), emptySet()),
        )
        val initial = mapOf("count" to "3", "text" to "")
        assertTrue(pluginConfigChanges(initial, initial, emptySet()).updates.isEmpty())
    }

    @Test
    fun restoreDeletesTheKeyInsteadOfPersistingTheDefault() {
        val initial = mapOf("mode" to "manual", "count" to "3")
        val changes = pluginConfigChanges(initial, mapOf("count" to "4"), setOf("mode"))
        val saved = initial.toMutableMap()
        val operations = mutableListOf<String>()
        val failed = applyPluginConfigChanges(
            changes,
            delete = { operations += "delete:$it"; saved.remove(it); true },
            write = { key, value -> operations += "write:$key"; saved[key] = value; true },
        )
        assertTrue(failed.isEmpty())
        assertEquals(listOf("delete:mode", "write:count"), operations)
        assertEquals(mapOf("count" to "4"), saved)
        assertEquals(mapOf("mode" to "manual", "count" to "3"), initial)
    }

    @Test
    fun partialFailureReportsEveryFailedKeyAndContinuesSaving() {
        val writes = mutableListOf<String>()
        val failed = applyPluginConfigChanges(
            PluginConfigChanges(mapOf("bad" to "1", "good" to "2"), setOf("reset")),
            delete = { false },
            write = { key, _ -> writes += key; key == "good" },
        )
        assertEquals(listOf("reset", "bad"), failed)
        assertEquals(listOf("bad", "good"), writes)
    }

    @Test
    fun editingAfterResetBecomesAnUpdate() {
        assertEquals(
            PluginConfigChanges(mapOf("mode" to "auto"), emptySet()),
            pluginConfigChanges(mapOf("mode" to "manual"), mapOf("mode" to "auto"), emptySet()),
        )
    }
}
