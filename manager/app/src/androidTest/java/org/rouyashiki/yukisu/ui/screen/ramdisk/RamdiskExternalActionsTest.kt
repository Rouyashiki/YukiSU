package org.rouyashiki.yukisu.ui.screen.ramdisk

import android.content.*
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.test.platform.app.InstrumentationRegistry
import com.anatdx.yukifb.model.*
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test

class RamdiskExternalActionsTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = instrumentation.targetContext
    private val entry = FileEntry(EntryId("file"), null, "init.txt", FileEntryType.REGULAR_FILE)
    private val bytes = "Saved ramdisk content\u0000\u0001\u00ff".encodeToByteArray()
    private val reader = RamdiskContentReader { _, _, consume -> consume(bytes, bytes.size) }

    @Test fun externalRecipientCanReadCompletedCopyButCannotWrite() = runBlocking<Unit> {
        val root = File(context.cacheDir, "ramdisk_external_test/${System.nanoTime()}")
        val received = CountDownLatch(1)
        var result: Intent? = null
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) { result = intent; received.countDown() }
        }
        ContextCompat.registerReceiver(context, receiver, IntentFilter("org.rouyashiki.yukisu.ui.screen.ramdisk.EXPORT_RECEIVED"), ContextCompat.RECEIVER_EXPORTED)
        try {
            val service = RamdiskExternalActions(context, { entry }, reader, RamdiskExternalFiles(root)) { chooser ->
                assertEquals(Intent.ACTION_CHOOSER, chooser.action)
                val intent = IntentCompat.getParcelableExtra(chooser, Intent.EXTRA_INTENT, Intent::class.java)!!
                assertEquals(Intent.ACTION_VIEW, intent.action)
                assertEquals("text/plain", intent.type)
                assertEquals(0, intent.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                assertNotEquals(0, intent.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION)
                assertEquals(intent.data, intent.clipData!!.getItemAt(0).uri)
                intent.component = ComponentName(instrumentation.context.packageName, RamdiskExportRecipientActivity::class.java.name)
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                intent.putExtra("resultPackage", context.packageName)
                context.startActivity(intent)
            }
            service.openExternally(entry.id) {}
            assertTrue(received.await(15, TimeUnit.SECONDS))
            assertArrayEquals(bytes, result!!.getByteArrayExtra("bytes"))
            assertTrue(result!!.getBooleanExtra("writeDenied", false))
            assertNotEquals(android.os.Process.myUid(), result!!.getIntExtra("uid", -1))
            val completed = root.walkTopDown().single { it.isFile }
            assertArrayEquals(bytes, completed.readBytes())
        } finally {
            context.unregisterReceiver(receiver)
            root.deleteRecursively()
        }
    }

    @Test fun shareUsesReadOnlyStreamAndRetainsHandedOffCopy() = runBlocking<Unit> {
        val root = File(context.cacheDir, "ramdisk_external_test/${System.nanoTime()}")
        try {
            var launched = false
            val service = RamdiskExternalActions(context, { entry.copy(mimeType = "application/x-elf") }, reader, RamdiskExternalFiles(root)) { chooser ->
                val intent = IntentCompat.getParcelableExtra(chooser, Intent.EXTRA_INTENT, Intent::class.java)!!
                assertEquals(Intent.ACTION_SEND, intent.action)
                assertEquals("application/x-elf", intent.type)
                assertEquals(intent.data, IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, android.net.Uri::class.java))
                assertEquals(Intent.FLAG_GRANT_READ_URI_PERMISSION, intent.flags)
                launched = true
            }
            service.share(entry.id) {}
            assertTrue(launched)
            assertArrayEquals(bytes, root.walkTopDown().single { it.isFile }.readBytes())
        } finally { root.deleteRecursively() }
    }

    @Test fun chooserLaunchFailureAndCancellationDeleteCopiesWithoutLateLaunch() = runBlocking<Unit> {
        val root = File(context.cacheDir, "ramdisk_external_test/${System.nanoTime()}")
        try {
            val files = RamdiskExternalFiles(root)
            val failed = RamdiskExternalActions(context, { entry }, reader, files) { throw ActivityNotFoundException() }
            try { failed.share(entry.id) {}; fail("Expected launch failure") } catch (_: ActivityNotFoundException) { }
            assertTrue(root.listFiles()!!.isEmpty())
            val started = CompletableDeferred<Unit>()
            var launched = false
            val slow = RamdiskExternalActions(context, { entry }, { _, _, _ -> started.complete(Unit); awaitCancellation() }, files) { launched = true }
            val job = launch { slow.openExternally(entry.id) {} }
            started.await()
            job.cancelAndJoin()
            assertFalse(launched)
            assertTrue(root.listFiles()!!.isEmpty())
        } finally { root.deleteRecursively() }
    }

    @Test fun unknownMimeWithoutAHandlerFailsBeforeLaunchingAndCleansCopy() = runBlocking<Unit> {
        val root = File(context.cacheDir, "ramdisk_external_test/${System.nanoTime()}")
        try {
            var queried = false
            var launched = false
            val service = RamdiskExternalActions(context, { entry.copy(name = "unknown") }, reader, RamdiskExternalFiles(root), canHandle = {
                assertEquals("application/octet-stream", it.type)
                queried = true
                false
            }) { launched = true }
            try { service.openExternally(entry.id) {}; fail("Expected no handler") } catch (_: ActivityNotFoundException) { }
            assertTrue(queried)
            assertFalse(launched)
            assertTrue(root.listFiles()!!.isEmpty())
        } finally { root.deleteRecursively() }
    }
}
