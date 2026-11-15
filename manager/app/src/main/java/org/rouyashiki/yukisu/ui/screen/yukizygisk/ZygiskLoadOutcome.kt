package org.rouyashiki.yukisu.ui.screen.yukizygisk

internal data class ZygiskLoadOutcome(
    val success: Boolean = false,
    val failure: Boolean = false,
    val complete: Boolean = false,
) {
    val partial: Boolean get() = success && failure

    fun merge(other: ZygiskLoadOutcome) = ZygiskLoadOutcome(
        success = success || other.success,
        failure = failure || other.failure,
        complete = complete && other.complete,
    )

    companion object {
        fun fromRecord(state: String, success: Boolean?, failure: Boolean?) =
            if (success != null && failure != null) {
                ZygiskLoadOutcome(success, failure, complete = true)
            } else {
                ZygiskLoadOutcome(success = state == "injected", failure = state == "failed")
            }
    }
}
