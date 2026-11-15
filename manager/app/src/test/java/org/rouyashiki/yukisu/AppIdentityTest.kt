package org.rouyashiki.yukisu

import android.app.Activity
import android.app.Application
import android.app.Service
import android.content.BroadcastReceiver
import android.content.ContentProvider
import android.content.Intent
import android.content.pm.PackageManager
import android.view.ContextThemeWrapper
import android.view.LayoutInflater
import android.widget.FrameLayout
import androidx.core.content.FileProvider
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.rouyashiki.yukisu.integrity.KsudIntegrity
import org.rouyashiki.yukisu.integrity.KsudIntegrityReceiver
import org.rouyashiki.yukisu.ui.screen.moreSettings.ScreenRatioOverlayView
import org.rouyashiki.yukisu.ui.screen.moreSettings.ScreenRatioUCropView
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class AppIdentityTest {
    private val context get() = RuntimeEnvironment.getApplication()

    @Suppress("DEPRECATION")
    @Test
    fun manifestComponentsResolveToTheirDeclaredClasses() {
        assertEquals("org.rouyashiki.yukisu", context.packageName)
        val info = context.packageManager.getPackageInfo(
            context.packageName,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_SERVICES or
                PackageManager.GET_RECEIVERS or PackageManager.GET_PROVIDERS or
                PackageManager.MATCH_DISABLED_COMPONENTS,
        )
        val components = listOf(
            info.activities.orEmpty().toList() to Activity::class.java,
            info.services.orEmpty().toList() to Service::class.java,
            info.receivers.orEmpty().toList() to BroadcastReceiver::class.java,
            info.providers.orEmpty().toList() to ContentProvider::class.java,
        )
        components.forEach { (entries, baseClass) ->
            assertTrue(entries.isNotEmpty())
            entries.forEach { entry ->
                val type = Class.forName(entry.name, false, context.classLoader)
                assertTrue(baseClass.isAssignableFrom(type), entry.name)
            }
        }
        val receiver = context.packageManager.queryBroadcastReceivers(
            Intent(KsudIntegrity.ACTION_CHANGED).setPackage(context.packageName),
            0,
        ).single { it.activityInfo.name == KsudIntegrityReceiver::class.java.name }
        assertEquals(
            "${context.packageName}.permission.REPORT_KSUD_INTEGRITY",
            receiver.activityInfo.permission,
        )
    }

    @Suppress("DEPRECATION")
    @Test
    fun fileProviderInitializesUnderTheCurrentApplicationAuthority() {
        val authority = "${context.packageName}.fileprovider"
        val info = requireNotNull(
            context.packageManager.resolveContentProvider(authority, PackageManager.GET_META_DATA),
        )
        assertEquals(FileProvider::class.java.name, info.name)
        assertEquals(context.packageName, info.packageName)
        assertEquals(authority, info.authority)
        assertFalse(info.exported)
        assertTrue(info.grantUriPermissions)
        FileProvider().attachInfo(context, info)
    }

    @Test
    fun cropLayoutInflatesBothCustomViews() {
        val themedContext = ContextThemeWrapper(context, R.style.Theme_YukiSU_ImageCropper)
        val parent = FrameLayout(themedContext)
        val layout = LayoutInflater.from(themedContext).inflate(R.layout.ucrop_activity_photobox, parent, false)
        val crop = assertIs<ScreenRatioUCropView>(layout.findViewById(R.id.ucrop))
        assertIs<ScreenRatioOverlayView>(crop.overlayView)
    }
}
