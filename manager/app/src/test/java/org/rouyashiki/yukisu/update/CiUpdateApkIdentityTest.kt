package org.rouyashiki.yukisu.update

import android.app.Application
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.content.pm.Signature
import android.content.pm.SigningInfo
import android.os.Bundle
import java.io.File
import java.lang.reflect.InvocationTargetException
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.shadow.api.Shadow
import org.rouyashiki.yukisu.BuildConfig
import kotlin.test.assertFailsWith
import kotlin.test.assertIs

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = Application::class)
class CiUpdateApkIdentityTest {
    private val context get() = RuntimeEnvironment.getApplication()
    private lateinit var apk: File
    private lateinit var archive: PackageInfo
    private lateinit var run: CiRun

    @Before
    fun prepareCandidate() {
        val packages = shadowOf(context.packageManager)
        val installed = packages.getInternalMutablePackageInfo(context.packageName)
        installed.signingInfo = signingInfo("01")
        run = CiRun(BuildConfig.CI_RUN_ID + 1, (installed.longVersionCode + 1).toInt(), "test", "test")
        archive = PackageInfo().apply {
            packageName = context.packageName
            longVersionCode = run.versionCode.toLong()
            signingInfo = signingInfo("01")
            applicationInfo = ApplicationInfo().apply {
                packageName = context.packageName
                metaData = Bundle().apply {
                    putString("${context.packageName}.CI_RUN_ID", "run-${run.runId}")
                }
            }
        }
        apk = File(context.cacheDir, "candidate.apk").apply { writeBytes(byteArrayOf(0)) }
        packages.setPackageArchiveInfo(apk.absolutePath, archive)
    }

    @Test
    fun acceptsAnUpdateForTheCurrentPackageAndSigner() {
        verify()
    }

    @Test
    fun rejectsAnUpdateForAnotherPackage() {
        archive.packageName = "com.example.other"
        assertRejected()
    }

    @Test
    fun rejectsAnUpdateSignedWithAnotherCertificate() {
        archive.signingInfo = signingInfo("02")
        assertRejected()
    }

    @Test
    fun requiresTheCurrentRunMetadata() {
        archive.applicationInfo!!.metaData.clear()
        assertRejected()
    }

    private fun verify() {
        CiUpdateManager::class.java.getDeclaredMethod(
            "verifyApk", Context::class.java, CiRun::class.java, File::class.java,
        ).apply { isAccessible = true }.invoke(CiUpdateManager, context, run, apk)
    }

    private fun assertRejected() {
        val failure = assertFailsWith<InvocationTargetException> { verify() }
        assertIs<IllegalStateException>(failure.targetException)
    }

    private fun signingInfo(signature: String): SigningInfo =
        Shadow.newInstanceOf(SigningInfo::class.java).also {
            shadowOf(it).setSignatures(arrayOf(Signature(signature)))
        }
}
