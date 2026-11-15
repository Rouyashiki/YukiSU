package org.rouyashiki.yukisu.ui

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class AndroidUserIdsTest {
    class User(@JvmField val id: Int)
    class Users(private val users: List<User>?) {
        fun getAliveUsers(): List<User>? = users
    }
    class UnavailableUsers {
        fun getAliveUsers(): List<User> = error("User service unavailable")
    }

    @Test fun includesIndependentUsersAndProfiles() {
        assertEquals(listOf(0, 10, 11, 999), queryAliveUserIds(Users(listOf(User(999), User(10), User(0), User(11)))))
    }

    @Test fun failedEnumerationCannotBecomeAPartialBackup() {
        assertFailsWith<IllegalStateException> { queryAliveUserIds(UnavailableUsers()) }
        assertFailsWith<IllegalStateException> { queryAliveUserIds(Any()) }
        assertFailsWith<IllegalStateException> { queryAliveUserIds(Users(null)) }
        assertFailsWith<IllegalStateException> { queryAliveUserIds(Users(emptyList())) }
    }

    @Test fun rejectsInvalidOrRepeatedUserIdentities() {
        assertFailsWith<IllegalStateException> { queryAliveUserIds(Users(listOf(User(0), User(-1)))) }
        assertFailsWith<IllegalStateException> { queryAliveUserIds(Users(listOf(User(0), User(0)))) }
    }
}
