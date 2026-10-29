package com.anatdx.yukisu.ui.activity.util

import androidx.compose.runtime.RememberObserver
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.GraphicsContext
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.layer.GraphicsLayer
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.layer.setOutline
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import kotlin.math.roundToInt

internal class NavigationBackdrop(private val graphics: GraphicsContext) {
    private data class Backdrop(val entryId: String, val motion: ForwardPageMotion, val layer: GraphicsLayer)

    internal inner class Page internal constructor(val entryId: String, val layer: GraphicsLayer) : RememberObserver {
        var density: Density = Density(1f)
        var layoutDirection = LayoutDirection.Ltr
        var size = IntSize.Zero
        var pose = PredictivePageTransform()
        var outline: Outline? = null
        var recorded = false
        var frozen = false
        var attached = false

        override fun onRemembered() {
            attached = true
            pages[entryId] = this
        }

        override fun onForgotten() = detach()
        override fun onAbandoned() = detach()

        private fun detach() {
            attached = false
            if (pages[entryId] === this) pages.remove(entryId)
            if (!frozen) release(layer)
        }
    }

    private val layers = mutableSetOf<GraphicsLayer>()
    private val pages = mutableMapOf<String, Page>()
    private val retainedPages = mutableSetOf<Page>()
    private val frames = mutableListOf<GraphicsLayer>()
    private var backdrop by mutableStateOf<Backdrop?>(null)
    private var viewport = IntSize.Zero
    private var destination: String? = null

    val entryFinished get() = backdrop?.motion?.finished == true

    fun createPage(entryId: String): Page = Page(entryId, newLayer())

    fun record(page: Page, scope: ContentDrawScope, outline: Outline) {
        if (!page.frozen) {
            page.density = Density(scope.density, scope.fontScale)
            page.layoutDirection = scope.layoutDirection
            page.size = IntSize(scope.size.width.roundToInt(), scope.size.height.roundToInt())
            page.outline = outline
            with(scope) { page.layer.record { this@with.drawContent() } }
            page.recorded = true
        }
        with(scope) { drawLayer(page.layer) }
    }

    fun begin(fromId: String, toId: String, motion: ForwardPageMotion) {
        if (backdrop?.entryId == toId) return
        val page = pages[fromId]
        if (page == null || !page.recorded || page.size != viewport || page.size == IntSize.Zero) {
            motion.handOff()
            clear()
            return
        }
        val pose = page.pose
        val presentedPage = newLayer().apply {
            scaleX = pose.scale
            scaleY = pose.scale
            translationX = page.size.width * pose.offset
            translationY = page.size.height * pose.offsetY
            alpha = pose.alpha
            page.outline?.let(::setOutline)
            clip = true
            record(page.density, page.layoutDirection, page.size) { drawLayer(page.layer) }
        }
        val previous = backdrop?.takeIf { it.entryId == fromId }?.layer
        val frame = newLayer().apply {
            record(page.density, page.layoutDirection, page.size) {
                previous?.let { drawLayer(it) }
                drawLayer(presentedPage)
            }
        }
        // Frames only reference older layers; a retained frame is never recorded again.
        page.frozen = true
        retainedPages += page
        frames += presentedPage
        frames += frame
        backdrop = Backdrop(toId, motion, frame)
    }

    fun draw(scope: DrawScope) {
        backdrop?.let { with(scope) { drawLayer(it.layer) } }
    }

    fun finishIfCovered(gestureActive: Boolean) {
        if (!gestureActive && backdrop?.motion?.finished == true) clear()
    }

    fun onDestinationChanged(entryId: String?) {
        destination = entryId
    }

    fun onPageSettled(entryId: String) {
        if (destination == entryId && backdrop?.entryId != entryId) clear()
    }

    fun resize(size: IntSize) {
        if (viewport != size) {
            stop()
            viewport = size
        }
    }

    fun stop() {
        backdrop?.motion?.handOff()
        clear()
    }

    fun dispose() {
        stop()
        layers.toList().forEach(::release)
        pages.clear()
    }

    private fun clear() {
        backdrop = null
        frames.asReversed().forEach(::release)
        frames.clear()
        retainedPages.forEach {
            it.frozen = false
            if (!it.attached) release(it.layer)
        }
        retainedPages.clear()
    }

    private fun newLayer(): GraphicsLayer = graphics.createGraphicsLayer().also { layers += it }

    private fun release(layer: GraphicsLayer) {
        if (layers.remove(layer)) graphics.releaseGraphicsLayer(layer)
    }
}
