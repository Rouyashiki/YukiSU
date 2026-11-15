package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.IKsuInterface
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

internal object AllowlistPackageReader {
    suspend fun read(
        service: IKsuInterface,
        signaturesFor: (AllowlistPlan.Inventory) -> Set<Int>
    ): AllowlistPlan.Inventory {
        val snapshot = service.beginBackupSnapshot()
        try {
            val packages = mutableListOf<BackupPackage>()
            while (true) {
                currentCoroutineContext().ensureActive()
                val page = service.getBackupPackages(snapshot, packages.size, BackupPackage.PAGE_SIZE)
                check(page.size <= BackupPackage.PAGE_SIZE) { "Invalid package page" }
                packages += page
                if (page.size < BackupPackage.PAGE_SIZE) break
            }
            val required = signaturesFor(AllowlistPlan.Inventory(packages))
            val indices = packages.indices.filter { packages[it].uid in required }
            indices.chunked(BackupPackage.PAGE_SIZE).forEach { batch ->
                currentCoroutineContext().ensureActive()
                val signed = service.getBackupSignatures(snapshot, batch.toIntArray())
                check(signed.size == batch.size) { "Incomplete signature query" }
                batch.zip(signed).forEach { (index, app) ->
                    check(packages[index].uid == app.uid && packages[index].packageName == app.packageName) {
                        "Package changed during query"
                    }
                    packages[index] = app
                }
            }
            return AllowlistPlan.Inventory(packages)
        } finally {
            runCatching { service.closeBackupSnapshot(snapshot) }
        }
    }

    suspend fun rules(service: IKsuInterface, keys: Set<String>): Map<String, String> {
        val result = mutableMapOf<String, String>()
        keys.toList().chunked(4).forEach { batch ->
            currentCoroutineContext().ensureActive()
            val rules = service.getBackupProfileRules(batch.toTypedArray())
            check(rules.size == batch.size) { "Incomplete rule query" }
            result.putAll(batch.zip(rules))
        }
        return result
    }
}
