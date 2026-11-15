package org.rouyashiki.yukisu.ui.screen.ramdisk

import com.anatdx.yukifb.hex.backend.BinaryBytes
import kotlinx.coroutines.test.runTest
import java.nio.file.Files
import kotlin.test.*

class RamdiskHexStorageTest {
    @Test fun appendedRangesRemainStableAndCloseRemovesScratchFile() = runTest {
        val directory = Files.createTempDirectory("hex-storage-test").toFile()
        try {
            val storage = RamdiskHexStorage.create(directory)
            val first = storage.append(BinaryBytes.copyOf(byteArrayOf(1, 2, 3)))
            val second = storage.append(BinaryBytes.copyOf(byteArrayOf(4, 5)))
            assertEquals(0L, first)
            assertEquals(3L, second)
            assertContentEquals(byteArrayOf(2, 3, 4, 5), storage.read(1, 4).toByteArray())
            assertContentEquals(byteArrayOf(1, 2, 3), storage.read(first, 3).toByteArray())
            storage.close()
            storage.close()
            assertTrue(directory.listFiles().orEmpty().isEmpty())
        } finally { directory.delete() }
    }
}
