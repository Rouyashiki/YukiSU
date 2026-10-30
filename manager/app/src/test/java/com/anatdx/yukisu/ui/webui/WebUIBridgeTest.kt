package com.anatdx.yukisu.ui.webui

import android.webkit.JavascriptInterface
import com.anatdx.yukisu.ui.webui.file.KsuIO
import com.anatdx.yukisu.ui.webui.pm.KsuPm
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull

class WebUIBridgeTest {
    @Test
    fun bothWebViewBridgesExposeFileAndPackageApis() {
        for (bridge in listOf(WebViewInterface::class.java, WebUIXBridge::class.java)) {
            for ((name, type) in mapOf("io" to KsuIO::class.java, "pm" to KsuPm::class.java)) {
                val method = bridge.getMethod(name)
                assertEquals(type, method.returnType)
                assertNotNull(method.getAnnotation(JavascriptInterface::class.java))
            }
        }
    }
}
