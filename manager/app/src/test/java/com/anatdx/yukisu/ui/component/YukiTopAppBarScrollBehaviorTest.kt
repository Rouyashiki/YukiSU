package com.anatdx.yukisu.ui.component

import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.material3.TopAppBarState
import androidx.compose.runtime.AbstractApplier
import androidx.compose.runtime.Composition
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Recomposer
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.unit.Velocity
import com.anatdx.yukisu.ui.theme.LocalCompactWindow
import kotlinx.coroutines.test.runTest
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.test.Test
import kotlin.test.assertEquals

@OptIn(ExperimentalMaterial3Api::class)
class YukiTopAppBarScrollBehaviorTest {
    @Test
    fun compactHeaderLeavesScrollToContentBeforeItsFirstMeasurement() {
        withCompactScrollBehavior { behavior ->
            assertEquals(-Float.MAX_VALUE, behavior.state.heightOffsetLimit)
            val connection = behavior.nestedScrollConnection
            repeat(4) {
                val delta = Offset(0f, -80f)
                assertEquals(Offset.Zero, connection.onPreScroll(delta, NestedScrollSource.UserInput))
                assertEquals(
                    Offset.Zero,
                    connection.onPostScroll(delta, Offset.Zero, NestedScrollSource.UserInput),
                )
            }
            assertEquals(0f, behavior.state.heightOffset)
            assertEquals(-320f, behavior.state.contentOffset)
        }
    }

    @Test
    fun compactHeaderDoesNotSettleAnOffsetRestoredFromALargeHeader() = runTest {
        val state = TopAppBarState(-144f, -72f, -256f)
        withCompactScrollBehavior(state) { behavior ->
            val connection = behavior.nestedScrollConnection
            assertEquals(
                Offset.Zero,
                connection.onPreScroll(Offset(0f, -80f), NestedScrollSource.UserInput),
            )
            assertEquals(
                Velocity.Zero,
                connection.onPostFling(Velocity.Zero, Velocity(0f, -1000f)),
            )
            assertEquals(-72f, state.heightOffset)
        }
    }

    private inline fun withCompactScrollBehavior(
        state: TopAppBarState? = null,
        block: (TopAppBarScrollBehavior) -> Unit,
    ) {
        val recomposer = Recomposer(EmptyCoroutineContext)
        val composition = Composition(NoOpApplier(), recomposer)
        lateinit var behavior: TopAppBarScrollBehavior
        try {
            composition.setContent {
                CompositionLocalProvider(
                    LocalCompactWindow provides true,
                ) {
                    behavior = if (state == null) {
                        rememberYukiTopAppBarScrollBehavior()
                    } else {
                        rememberYukiTopAppBarScrollBehavior(state)
                    }
                }
            }
            block(behavior)
        } finally {
            composition.dispose()
            recomposer.cancel()
        }
    }

    private class NoOpApplier : AbstractApplier<Unit>(Unit) {
        override fun onClear() = Unit
        override fun insertTopDown(index: Int, instance: Unit) = error("Unexpected UI node")
        override fun insertBottomUp(index: Int, instance: Unit) = error("Unexpected UI node")
        override fun move(from: Int, to: Int, count: Int) = error("Unexpected UI node")
        override fun remove(index: Int, count: Int) = error("Unexpected UI node")
    }
}
