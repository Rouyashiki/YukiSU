package org.rouyashiki.yukisu

import android.os.Parcelable
import kotlinx.parcelize.Parcelize

@Parcelize
data class BackupPackage(
    val packageName: String,
    val uid: Int,
    val versionCode: Long = 0,
    val lastUpdateTime: Long = 0,
    val currentSigners: List<String> = emptyList(),
    val signingHistory: List<String> = emptyList(),
    val status: Int = UNQUERIED,
    val label: String = packageName
) : Parcelable {
    val userId: Int get() = uid / 100000

    fun accepts(certificates: List<String>): Boolean {
        if (status != OK || certificates.isEmpty()) return false
        return if (certificates.size == 1 && currentSigners.size == 1) {
            certificates.single() in signingHistory || certificates == currentSigners
        } else {
            certificates.toSet() == currentSigners.toSet()
        }
    }

    companion object {
        const val UNQUERIED = 0
        const val OK = 1
        const val MISSING = 2
        const val UNAVAILABLE = 3
        const val PAGE_SIZE = 64
    }
}
