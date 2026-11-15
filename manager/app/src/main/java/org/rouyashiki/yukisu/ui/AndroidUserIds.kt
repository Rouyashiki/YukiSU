package org.rouyashiki.yukisu.ui

internal fun queryAliveUserIds(userManager: Any): List<Int> = try {
    val users = userManager.javaClass.getMethod("getAliveUsers").invoke(userManager) as? List<*>
    check(!users.isNullOrEmpty()) { "Cannot enumerate Android users" }
    val ids = users.map { user ->
        checkNotNull(user) { "Missing Android user" }
        user.javaClass.getField("id").getInt(user).also {
            check(it >= 0) { "Invalid Android user ID" }
        }
    }
    check(ids.distinct().size == ids.size) { "Duplicate Android user ID" }
    ids.sorted()
} catch (error: ReflectiveOperationException) {
    throw IllegalStateException("Cannot enumerate Android users", error)
}
