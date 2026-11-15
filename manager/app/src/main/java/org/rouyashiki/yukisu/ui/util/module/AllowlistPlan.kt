package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.BackupPackage
import org.rouyashiki.yukisu.Natives

internal object AllowlistPlan {
    enum class Status { MATCHED, MAPPED, MISSING, SHARED_UID, SIGNATURE, UNREADABLE, PROTECTED, DUPLICATE, RULE_CONFLICT }

    class Inventory(val packages: List<BackupPackage>) {
        val byUid = packages.groupBy { it.uid }
        val byName = packages.associateBy { it.userId to it.packageName }
        private val singleSigners = packages.filter { it.status == BackupPackage.OK && it.currentSigners.size == 1 }
            .flatMap { app -> (app.signingHistory + app.currentSigners).distinct().map { (app.userId to it) to app } }
            .groupBy({ it.first }, { it.second })
        private val multipleSigners = packages.filter { it.status == BackupPackage.OK && it.currentSigners.size > 1 }
            .groupBy { it.userId to it.currentSigners.sorted() }

        init {
            require(byName.size == packages.size) { "Duplicate installed package" }
        }

        fun identifies(profile: Natives.Profile): Boolean = byUid[profile.currentUid]?.let { group ->
            group.any { it.packageName == profile.name } && group.all { it.status == BackupPackage.OK }
        } == true

        fun candidates(entry: AllowlistBackup.Entry, protected: Set<Int>): List<BackupPackage> {
            if (entry.packages.size != 1) return emptyList()
            val user = entry.profile.currentUid / 100000
            if (byName.containsKey(user to entry.packages.single())) return emptyList()
            val signers = entry.signingCertificates[entry.packages.single()].orEmpty()
            val candidates = when (signers.size) {
                0 -> emptyList()
                1 -> singleSigners[user to signers.single()].orEmpty()
                else -> multipleSigners[user to signers.sorted()].orEmpty()
            }
            return candidates.filter { byUid[it.uid]?.size == 1 && it.uid % 100000 !in protected }
                .sortedBy { it.packageName }
        }
    }

    data class Context(
        val document: AllowlistBackup.Document,
        val installed: Inventory,
        val before: Map<Int, Natives.Profile>,
        val oldRules: Map<String, String>,
        val protected: Set<Int>,
        val candidateUsers: Set<Int> = emptySet()
    )

    data class Options(
        val replace: Boolean = false,
        val restoreDefault: Boolean = false,
        val excluded: Set<Int> = emptySet(),
        val mappings: Map<Int, Int> = emptyMap()
    )

    data class Row(
        val source: AllowlistBackup.Entry,
        val target: Natives.Profile?,
        val status: Status,
        val requested: Boolean
    ) {
        val selected: Boolean get() = requested && (status == Status.MATCHED || status == Status.MAPPED)
    }

    data class Plan(
        val context: Context,
        val options: Options,
        val rows: List<Row>,
        val profiles: Map<Int, Natives.Profile>,
        val rules: Map<String, String>,
        val resetUids: Set<Int>,
        val replaceAvailable: Boolean,
        val guardedUids: Set<Int>
    ) {
        val canApply: Boolean get() = (!options.replace || replaceAvailable) && (profiles.isNotEmpty() || rules.isNotEmpty())
        val selectedCount: Int get() = rows.count { it.selected }
        val migratedCount: Int get() = rows.count { it.selected && it.target?.currentUid != it.source.profile.currentUid }
        val pendingCount: Int get() = if (context.document.version == 1) 0 else rows.count {
            it.requested && it.status == Status.MISSING && it.source.packages.size == 1
        }

        fun sameExecution(other: Plan): Boolean =
            options == other.options && rows == other.rows && profiles == other.profiles && rules == other.rules &&
                resetUids == other.resetUids && replaceAvailable == other.replaceAvailable &&
                context.protected == other.context.protected &&
                (guardedUids + other.guardedUids).all { uid ->
                    context.before[uid] == other.context.before[uid] &&
                        context.installed.byUid[uid] == other.context.installed.byUid[uid]
                } && rules.keys.all { context.oldRules[it] == other.context.oldRules[it] }
    }

    fun create(context: Context, options: Options = Options()): Plan {
        AllowlistBackup.validate(context.document)
        val plan = build(context, options)
        return if (options.replace) plan else plan.copy(
            replaceAvailable = build(context, options.copy(replace = true)).replaceAvailable
        )
    }

    private fun build(context: Context, options: Options): Plan {
        val inventory = context.installed
        var rows = context.document.apps.map { entry ->
            val requested = entry.profile.currentUid !in options.excluded
            val user = entry.profile.currentUid / 100000
            val found = entry.packages.mapNotNull { inventory.byName[user to it] }
            fun row(status: Status, target: Natives.Profile? = null) = Row(entry, target, status, requested)
            if (found.isEmpty()) {
                val mapped = options.mappings[entry.profile.currentUid]
                val candidate = mapped?.let { uid -> inventory.byUid[uid]?.singleOrNull() }
                val signers = entry.signingCertificates[entry.profile.name].orEmpty()
                if (entry.packages.size == 1 && candidate != null && candidate.userId == user && candidate.accepts(signers)) {
                    if (candidate.uid % 100000 in context.protected) row(Status.PROTECTED)
                    else row(Status.MAPPED, entry.profile.copy(name = candidate.packageName, currentUid = candidate.uid))
                } else row(Status.MISSING)
            } else if (found.size != entry.packages.size || found.map { it.uid }.distinct().size != 1 ||
                inventory.byUid.getValue(found.first().uid).map { it.packageName }.toSet() != entry.packages.toSet()) {
                row(Status.SHARED_UID)
            } else if (found.first().uid % 100000 in context.protected) {
                row(Status.PROTECTED)
            } else if (found.any { it.status != BackupPackage.OK }) {
                row(Status.UNREADABLE)
            } else if (context.document.version == 2 && found.any { !it.accepts(entry.signingCertificates.getValue(it.packageName)) }) {
                row(Status.SIGNATURE)
            } else {
                row(Status.MATCHED, entry.profile.copy(currentUid = found.first().uid))
            }
        }
        val duplicateUids = rows.filter { it.selected }.groupingBy { it.target!!.currentUid }.eachCount().filterValues { it > 1 }.keys
        rows = rows.map { if (it.selected && it.target!!.currentUid in duplicateUids) it.copy(status = Status.DUPLICATE) else it }

        val ordinary = context.before.filterKeys { it != AllowlistRestore.DEFAULT_UID && it % 100000 !in context.protected }
        val owners = context.before.values.filter { it.currentUid != AllowlistRestore.DEFAULT_UID }.groupBy { it.name }
        val protectedKeys = inventory.packages.filter { it.uid % 100000 in context.protected }.map { it.packageName }.toSet()
        val currentIdentified = ordinary.values.all(inventory::identifies)
        var replacementConflict = false
        var desired = emptyMap<Int, Natives.Profile>()
        var targetRules = emptyMap<String, String>()
        var resets = emptySet<Int>()
        while (true) {
            val selected = rows.filter { it.selected }.map { it.target!! }
            val replace = options.replace && rows.all { it.selected } && currentIdentified && !replacementConflict
            val selectedUids = selected.map { it.currentUid }.toSet()
            resets = if (replace) ordinary.keys - selectedUids else emptySet()
            desired = selected.associateBy { it.currentUid } + resets.associateWith { uid ->
                Natives.Profile(name = ordinary.getValue(uid).name, currentUid = uid)
            }
            val rulesByKey = selected.groupBy { it.name }
            val conflicts = rulesByKey.filterValues { profiles -> profiles.map { it.rules }.distinct().size > 1 }.keys.toMutableSet()
            targetRules = rulesByKey.mapValues { it.value.first().rules }.toMutableMap().apply {
                if (replace) ordinary.values.forEach { putIfAbsent(it.name, "") }
            }
            targetRules.forEach { (key, value) ->
                val previous = context.oldRules.getValue(key)
                if (value != previous && (key in protectedKeys || owners[key].orEmpty().any {
                        it.currentUid !in desired || it.currentUid % 100000 in context.protected
                    })) conflicts += key
            }
            if (conflicts.isEmpty()) break
            val next = rows.map { if (it.selected && it.target!!.name in conflicts) it.copy(status = Status.RULE_CONFLICT) else it }
            if (replace) replacementConflict = true
            if (next == rows) break
            rows = next
        }
        val replaceAvailable = currentIdentified && rows.all { it.selected } && !replacementConflict
        if (options.replace && !replaceAvailable) {
            return Plan(context, options, rows, emptyMap(), emptyMap(), emptySet(), false, emptySet())
        }
        val allDesired = desired.toMutableMap()
        if (options.restoreDefault) allDesired[AllowlistRestore.DEFAULT_UID] = Natives.Profile(
            name = AllowlistRestore.DEFAULT_KEY, currentUid = AllowlistRestore.DEFAULT_UID,
            nonRootUseDefault = false, umountModules = context.document.defaultUmountModules
        )
        val profiles = allDesired.mapValues { it.value.copy(rules = "") }
            .filter { (uid, profile) -> context.before[uid]?.copy(rules = "") != profile }
        val rules = targetRules.filter { (key, value) -> context.oldRules.getValue(key) != value }
        val packageOwners = inventory.packages.groupBy { it.packageName }
        val guards = allDesired.keys + rules.keys.flatMap { key ->
            owners[key].orEmpty().map { it.currentUid } + packageOwners[key].orEmpty().map { it.uid }
        }
        return Plan(context, options, rows, profiles, rules, resets, replaceAvailable, guards)
    }
}
