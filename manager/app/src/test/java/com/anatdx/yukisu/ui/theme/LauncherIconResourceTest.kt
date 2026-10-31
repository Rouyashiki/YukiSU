package com.anatdx.yukisu.ui.theme

import android.app.Application
import android.graphics.drawable.AdaptiveIconDrawable
import android.util.TypedValue
import com.anatdx.yukisu.R
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34, 36], application = Application::class)
class LauncherIconResourceTest {
    @Test
    fun launcherAndShortcutIconRemainAdaptiveAtEveryDensity() {
        val context = RuntimeEnvironment.getApplication()
        for (density in listOf(120, 160, 213, 240, 320, 480, 640)) {
            val value = TypedValue()
            context.resources.getValueForDensity(R.mipmap.ic_launcher, density, value, true)
            assertTrue(value.string.toString().endsWith("ic_launcher.xml"), "Density $density: ${value.string}")
            val icon = assertIs<AdaptiveIconDrawable>(
                context.resources.getDrawableForDensity(R.mipmap.ic_launcher, density, context.theme),
            )
            assertNotNull(icon.foreground)
            assertNotNull(icon.background)
            assertNotNull(icon.monochrome)
        }
    }
}
