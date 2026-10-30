package com.anatdx.yukisu.ui.webui.pm

import android.app.Application
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import com.anatdx.yukisu.ui.viewmodel.SuperUserViewModel.AppInfo
import org.json.JSONArray
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class KsuPmTest {
    private fun app(name: String, uid: Int, system: Boolean = false, special: Boolean = false) = AppInfo(
        label = "Label $uid",
        packageInfo = PackageInfo().apply {
            packageName = name
            versionName = "1.0"
            longVersionCode = 4_000_000_000L
            applicationInfo = ApplicationInfo().apply {
                this.uid = uid
                flags = if (system) ApplicationInfo.FLAG_SYSTEM else 0
            }
        },
        profile = null,
        special = special,
    )

    @Test fun packageListingDeduplicatesUsersAndSkipsSyntheticEntries() {
        val pm = KsuPm { listOf(
            app("user.app", 1010001), app("system.app", 10002, system = true),
            app("user.app", 10001), app("", 1053, special = true),
        ) }
        assertEquals(1, pm.version())
        assertEquals("[\"system.app\",\"user.app\"]", pm.listPackages("all"))
        assertEquals("[\"system.app\"]", pm.listPackages("system"))
        assertEquals("[\"user.app\"]", pm.listPackages("USER"))
    }

    @Test fun packageInfoIncludesEveryUserSortedByUid() {
        val pm = KsuPm { listOf(app("same.app", 1010001), app("same.app", 10001)) }
        val info = JSONArray(pm.getPackagesInfo("[\"same.app\"]"))
        assertEquals(2, info.length())
        assertEquals(10001, info.getJSONObject(0).getInt("uid"))
        assertEquals(0, info.getJSONObject(0).getInt("userId"))
        assertEquals(10, info.getJSONObject(1).getInt("userId"))
        assertEquals(4_000_000_000L, info.getJSONObject(0).getLong("versionCode"))
    }

    @Test fun missingPackagesKeepTheirPositionAndReturnAnError() {
        var reads = 0
        val pm = KsuPm { reads++; listOf(app("present", 10001)) }
        val info = JSONArray(pm.getPackagesInfo("[\"missing\",\"present\"]"))
        assertEquals(1, reads)
        assertEquals("missing", info.getJSONObject(0).getString("packageName"))
        assertTrue(info.getJSONObject(0).has("error"))
        assertEquals("present", info.getJSONObject(1).getString("packageName"))
    }
}
