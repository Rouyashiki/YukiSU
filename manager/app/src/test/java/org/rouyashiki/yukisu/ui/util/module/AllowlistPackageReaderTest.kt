package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.IKsuInterface
import java.lang.reflect.Proxy
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import kotlin.test.*

class AllowlistPackageReaderTest {
    private class Source(val packages: List<BackupPackage>) {
        var fail: String? = null
        var cancel = false
        var closed = false
        val queried = mutableListOf<Int>()
        val pages = mutableListOf<Int>()
        val service = Proxy.newProxyInstance(
            IKsuInterface::class.java.classLoader, arrayOf(IKsuInterface::class.java)
        ) { _, method, args ->
            if (method.name == fail) throw IllegalStateException("Package service failed")
            when (method.name) {
                "beginBackupSnapshot" -> "snapshot"
                "getBackupPackages" -> {
                    if (cancel) throw CancellationException("Cancelled")
                    val start = args[1] as Int
                    val count = args[2] as Int
                    pages += start
                    packages.drop(start).take(count)
                }
                "getBackupSignatures" -> (args[1] as IntArray).map { queried += it; packages[it] }
                "closeBackupSnapshot" -> { closed = true; null }
                "getBackupProfileRules" -> (args[0] as Array<*>).map { "rules for $it" }
                else -> error("Unexpected method: ${method.name}")
            }
        } as IKsuInterface
    }

    @Test fun readsIndependentPagesAndOnlyRequestsSelectedSigningIdentities() = runTest {
        val packages = (0 until 200).map { BackupPackage("test.app$it", 10000 + it) }
        val source = Source(packages)
        val inventory = AllowlistPackageReader.read(source.service) { setOf(10005, 10199) }
        assertEquals(packages, inventory.packages)
        assertEquals(listOf(0, 64, 128, 192), source.pages)
        assertEquals(listOf(5, 199), source.queried)
        assertTrue(source.closed)
    }

    @Test fun enumerationAndSigningTransportFailuresNeverBecomeEmptyResults() = runTest {
        listOf("getBackupPackages", "getBackupSignatures").forEach { method ->
            val source = Source(listOf(BackupPackage("test.app", 10123))).apply { fail = method }
            assertFailsWith<IllegalStateException> { AllowlistPackageReader.read(source.service) { setOf(10123) } }
            assertTrue(source.closed)
        }
    }

    @Test fun perPackageReadFailureRemainsDistinctFromMissingPackage() = runTest {
        val failed = BackupPackage("test.app", 10123, status = BackupPackage.UNAVAILABLE)
        val source = Source(listOf(failed))
        val inventory = AllowlistPackageReader.read(source.service) { setOf(10123) }
        assertEquals(failed, inventory.byName[0 to "test.app"])
        assertNull(inventory.byName[0 to "test.missing"])
    }

    @Test fun cancellationClosesTheOperationSnapshot() = runTest {
        val source = Source(emptyList()).apply { cancel = true }
        assertFailsWith<CancellationException> { AllowlistPackageReader.read(source.service) { emptySet() } }
        assertTrue(source.closed)
    }

    @Test fun batchesRuleReadsWithoutLosingKeys() = runTest {
        val source = Source(emptyList())
        val keys = (0 until 25).map { "test.app$it" }.toSet()
        assertEquals(keys.associateWith { "rules for $it" }, AllowlistPackageReader.rules(source.service, keys))
    }
}
