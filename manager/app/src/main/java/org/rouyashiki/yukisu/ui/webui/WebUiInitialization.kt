package org.rouyashiki.yukisu.ui.webui

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withTimeoutOrNull

internal suspend fun initializeWebUi(
    timeoutMillis: Long = 15_000,
    onFailure: (Exception) -> Unit = {},
    connect: suspend () -> Boolean,
): Boolean = try {
    withTimeoutOrNull(timeoutMillis) { connect() } ?: false
} catch (e: CancellationException) {
    throw e
} catch (e: Exception) {
    onFailure(e)
    false
}
