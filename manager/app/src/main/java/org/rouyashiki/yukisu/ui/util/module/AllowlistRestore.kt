package org.rouyashiki.yukisu.ui.util.module

import org.rouyashiki.yukisu.Natives

internal object AllowlistRestore {
    const val DEFAULT_UID = 9999
    const val DEFAULT_KEY = "$"

    interface Store {
        fun read(uid: Int): Natives.Profile?
        fun write(profile: Natives.Profile): Boolean
        fun readRules(key: String): String
        fun writeRules(key: String, rules: String): Boolean
        fun identitiesMatch(uids: Set<Int>): Boolean
    }

    class Failure(val rollbackFailed: Boolean, cause: Exception) : Exception(
        if (rollbackFailed) "Restore failed and rollback is incomplete" else "Restore failed; previous settings restored", cause
    )

    fun apply(
        plan: AllowlistPlan.Plan,
        store: Store
    ) {
        require(!plan.options.replace || plan.replaceAvailable) { "Incomplete replacement plan" }
        val before = plan.context.before
        val desired = plan.profiles
        require(desired.keys.none { it % 100000 in plan.context.protected }) {
            "Protected manager profile cannot be restored"
        }
        val rules = plan.rules
        val oldRules = plan.context.oldRules
        fun identityMatches(uid: Int) = store.identitiesMatch(if (uid == DEFAULT_UID) emptySet() else setOf(uid))
        val profileOwners = before.values.groupBy { it.name }
        val packageOwners = plan.context.installed.packages.groupBy { it.packageName }
        fun ruleUids(key: String): Set<Int> =
            (profileOwners[key].orEmpty().map { it.currentUid } + packageOwners[key].orEmpty().map { it.uid }).toSet()
        val ruleIdentities = rules.keys.associateWith(::ruleUids)
        check(store.identitiesMatch(plan.guardedUids - DEFAULT_UID)) { "Package identity changed before restore" }
        desired.keys.forEach { uid ->
            check(store.read(uid)?.copy(rules = "") == before[uid]?.copy(rules = "")) { "Profile changed before restore: $uid" }
        }
        rules.keys.forEach { check(store.readRules(it) == oldRules.getValue(it)) { "Rules changed before restore: $it" } }
        val attemptedProfiles = mutableListOf<Int>()
        val attemptedRules = mutableListOf<String>()
        try {
            rules.forEach { (key, value) ->
                check(store.identitiesMatch(ruleIdentities.getValue(key))) { "Package identity changed: $key" }
                check(store.readRules(key) == oldRules.getValue(key)) { "Rules changed during restore: $key" }
                attemptedRules += key
                check(store.writeRules(key, value)) { "Cannot write SELinux rules for $key" }
                check(store.readRules(key) == value) { "SELinux rules verification failed for $key" }
                check(store.identitiesMatch(ruleIdentities.getValue(key))) { "Package identity changed: $key" }
            }
            desired.forEach { (uid, profile) ->
                val target = profile.copy(rules = "")
                if (before[uid]?.copy(rules = "") != target) {
                    check(identityMatches(uid)) { "Package identity changed: $uid" }
                    check(store.read(uid)?.copy(rules = "") == before[uid]?.copy(rules = "")) { "Profile changed during restore: $uid" }
                    attemptedProfiles += uid
                    check(store.write(target)) { "Cannot update UID $uid" }
                    check(store.read(uid)?.copy(rules = "") == target) { "Profile verification failed for UID $uid" }
                    check(identityMatches(uid)) { "Package identity changed: $uid" }
                }
            }
            check(store.identitiesMatch(plan.guardedUids - DEFAULT_UID)) { "Package identity changed during restore" }
        } catch (failure: Exception) {
            var rollbackFailed = false
            attemptedProfiles.asReversed().forEach { uid ->
                // An absent entry behaves like a non-root profile using the global default.
                val previous = before[uid]?.copy(rules = "") ?: Natives.Profile(
                    name = desired.getValue(uid).name, currentUid = uid,
                    nonRootUseDefault = uid != DEFAULT_UID
                )
                val restored = runCatching {
                    identityMatches(uid) && (uid == DEFAULT_UID ||
                        plan.context.installed.byUid[uid].orEmpty().any { it.packageName == previous.name }) &&
                        when (store.read(uid)?.copy(rules = "")) {
                            before[uid]?.copy(rules = "") -> true
                            desired.getValue(uid).copy(rules = "") ->
                                store.write(previous) && store.read(uid)?.copy(rules = "") == previous && identityMatches(uid)
                            else -> false
                        }
                }.getOrDefault(false)
                if (!restored) rollbackFailed = true
            }
            attemptedRules.asReversed().forEach { key ->
                val previous = oldRules.getValue(key)
                val restored = runCatching {
                    store.identitiesMatch(ruleIdentities.getValue(key)) &&
                        when (store.readRules(key)) {
                            previous -> true
                            rules.getValue(key) -> store.writeRules(key, previous) && store.readRules(key) == previous &&
                                store.identitiesMatch(ruleIdentities.getValue(key))
                            else -> false
                        }
                }.getOrDefault(false)
                if (!restored) rollbackFailed = true
            }
            throw Failure(rollbackFailed, failure)
        }
    }
}
