package org.rouyashiki.yukisu.ui.screen.ramdisk

import android.util.Log
import com.anatdx.yukifb.hex.backend.BinaryBytes
import com.anatdx.yukifb.hex.backend.BinaryCommitter
import com.anatdx.yukifb.hex.backend.BinaryDataSource
import com.anatdx.yukifb.hex.backend.BinaryPiece
import com.anatdx.yukifb.hex.backend.BinaryRevisionConflictException
import com.anatdx.yukifb.hex.backend.BinarySaveRequest
import com.anatdx.yukifb.backend.CopyEntriesCapability
import com.anatdx.yukifb.backend.CreateEntryCapability
import com.anatdx.yukifb.backend.DeleteEntriesCapability
import com.anatdx.yukifb.backend.FileBrowserBackend
import com.anatdx.yukifb.backend.FileContentCapability
import com.anatdx.yukifb.backend.FileContentSource
import com.anatdx.yukifb.backend.HardLinkCapability
import com.anatdx.yukifb.backend.MoveEntriesCapability
import com.anatdx.yukifb.backend.RenameEntryCapability
import com.anatdx.yukifb.backend.SymbolicLinkCapability
import com.anatdx.yukifb.backend.UnixMetadataCapability
import com.anatdx.yukifb.model.EntryId
import com.anatdx.yukifb.model.FileEntry
import com.anatdx.yukifb.model.FileEntryType
import com.anatdx.yukifb.model.UnixMetadata
import com.anatdx.yukifb.model.UnixMetadataPatch
import com.anatdx.yukifb.model.FileByteProgress
import com.anatdx.yukifb.model.FileContentChangedException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withContext
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.TimeUnit
import kotlin.math.min

internal class YrcpRamdiskBackend private constructor(
    private val session: YrcpSession,
    private val stagingDirectory: File,
    private val rootDisplayName: String,
    private val mutableDirty: MutableStateFlow<Boolean>,
    val fragment: RamdiskFragmentInfo,
    val rootEntry: FileEntry,
) : FileBrowserBackend,
    CreateEntryCapability,
    RenameEntryCapability,
    DeleteEntriesCapability,
    CopyEntriesCapability,
    MoveEntriesCapability,
    UnixMetadataCapability,
    SymbolicLinkCapability,
    HardLinkCapability,
    FileContentCapability {

    val dirty: StateFlow<Boolean> = mutableDirty.asStateFlow()

    override suspend fun stat(entryId: EntryId): FileEntry =
        session.stat(entryId.toNodeId()).toFileEntry(
            rootNodeId = fragment.rootNodeId,
            rootDisplayName = rootDisplayName,
        )

    override suspend fun list(directoryId: EntryId): List<FileEntry> =
        session.list(directoryId.toNodeId()).map {
            it.toFileEntry(
                rootNodeId = fragment.rootNodeId,
                rootDisplayName = rootDisplayName,
            )
        }

    override suspend fun createFile(parentId: EntryId, name: String): FileEntry {
        val createdId = session.createFile(
            parentId = parentId.toNodeId(),
            name = name,
            permissions = DEFAULT_FILE_PERMISSIONS,
            uid = 0,
            gid = 0,
        )
        markDirty()
        return stat(createdId.toEntryId())
    }

    override suspend fun createDirectory(parentId: EntryId, name: String): FileEntry {
        val createdId = session.createDirectory(
            parentId = parentId.toNodeId(),
            name = name,
            permissions = DEFAULT_DIRECTORY_PERMISSIONS,
            uid = 0,
            gid = 0,
        )
        markDirty()
        return stat(createdId.toEntryId())
    }

    override suspend fun rename(entryId: EntryId, newName: String): FileEntry {
        val entry = stat(entryId)
        val parentId = entry.parentId
            ?: throw IOException("The archive root cannot be renamed")
        session.move(entryId.toNodeId(), parentId.toNodeId(), newName)
        markDirty()
        return stat(entryId)
    }

    override suspend fun delete(entryIds: List<EntryId>) {
        entryIds.forEach { entryId ->
            session.remove(entryId.toNodeId(), recursive = true)
            markDirty()
        }
    }

    override suspend fun copy(entryIds: List<EntryId>, destinationId: EntryId) {
        entryIds.forEach { entryId ->
            val entry = stat(entryId)
            session.copy(entryId.toNodeId(), destinationId.toNodeId(), entry.name)
            markDirty()
        }
    }

    override suspend fun move(entryIds: List<EntryId>, destinationId: EntryId) {
        entryIds.forEach { entryId ->
            val entry = stat(entryId)
            session.move(entryId.toNodeId(), destinationId.toNodeId(), entry.name)
            markDirty()
        }
    }

    override suspend fun updateMetadata(
        entryIds: List<EntryId>,
        patch: UnixMetadataPatch,
    ) {
        entryIds.forEach { entryId ->
            session.updateMetadata(entryId.toNodeId(), patch)
            markDirty()
        }
    }

    override suspend fun createSymbolicLink(
        parentId: EntryId,
        name: String,
        target: String,
    ): FileEntry {
        val createdId = session.createSymbolicLink(
            parentId = parentId.toNodeId(),
            name = name,
            target = target,
            uid = 0,
            gid = 0,
        )
        markDirty()
        return stat(createdId.toEntryId())
    }

    override suspend fun createHardLink(
        parentId: EntryId,
        name: String,
        targetId: EntryId,
    ): FileEntry {
        val createdId = session.createHardLink(
            parentId = parentId.toNodeId(),
            name = name,
            targetId = targetId.toNodeId(),
        )
        markDirty()
        return stat(createdId.toEntryId())
    }

    override suspend fun <T> read(
        entryId: EntryId,
        reader: suspend (InputStream) -> T,
    ): T {
        val entry = stat(entryId)
        val length = entry.size
            ?: throw IOException("The selected entry has no readable content")
        return session.read(entryId.toNodeId(), length, reader)
    }

    override suspend fun replace(entryId: EntryId, source: FileContentSource) {
        val staged = withContext(Dispatchers.IO) {
            File.createTempFile("yrcp_content_", ".tmp", stagingDirectory).also { file ->
                try {
                    source.openStream().use { input ->
                        file.outputStream().buffered().use { output ->
                            copyBounded(input, output, session.maxContentSize)
                        }
                    }
                } catch (error: Throwable) {
                    file.delete()
                    throw error
                }
            }
        }
        try {
            session.replace(entryId.toNodeId(), staged)
            markDirty()
        } finally {
            staged.delete()
        }
    }

    val supportsHex: Boolean get() = session.supportsBinaryEditing

    suspend fun openBinary(entryId: EntryId): BinaryDataSource =
        YrcpBinarySource(session, entryId, session.binaryInfo(entryId.toNodeId()))

    suspend fun readSnapshot(entryId: EntryId, onProgress: (FileByteProgress) -> Unit, consume: (ByteArray, Int) -> Unit) =
        session.readSnapshot(entryId.toNodeId(), onProgress, consume)

    fun binaryCommitter(entryId: EntryId, onCommitted: () -> Unit): BinaryCommitter =
        object : BinaryCommitter {
            override val maxSize: Long get() = session.maxContentSize
            override suspend fun commit(request: BinarySaveRequest): BinaryDataSource {
                val source = request.source as? YrcpBinarySource
                    ?: throw IOException("Binary source does not belong to this session")
                require(source.session === session && source.id == entryId)
                val info = session.applyEdits(entryId.toNodeId(), source.info.revision, request)
                markDirty()
                onCommitted()
                return YrcpBinarySource(session, entryId, info)
            }
        }

    suspend fun readElfHeader(entryId: EntryId): ElfHeaderInfo =
        session.elfHeader(entryId.toNodeId())

    suspend fun importFile(
        parentId: EntryId,
        name: String,
        source: FileContentSource,
    ): FileEntry {
        val created = createFile(parentId, name)
        return try {
            replace(created.id, source)
            stat(created.id)
        } catch (error: Throwable) {
            runCatching { session.remove(created.id.toNodeId(), recursive = false) }
            throw error
        }
    }

    private fun markDirty() {
        mutableDirty.value = true
    }

    companion object {
        suspend fun create(
            session: YrcpSession,
            stagingDirectory: File,
            rootDisplayName: String,
            dirty: MutableStateFlow<Boolean>,
            fragment: RamdiskFragmentInfo,
        ): YrcpRamdiskBackend = withContext(Dispatchers.IO) {
            val root = session.stat(fragment.rootNodeId).toFileEntry(
                rootNodeId = fragment.rootNodeId,
                rootDisplayName = rootDisplayName,
            )
            YrcpRamdiskBackend(
                session = session,
                stagingDirectory = stagingDirectory,
                rootDisplayName = rootDisplayName,
                mutableDirty = dirty,
                fragment = fragment,
                rootEntry = root,
            )
        }
    }
}

internal class YrcpRamdiskImage private constructor(
    private val session: YrcpSession,
    private val mutableDirty: MutableStateFlow<Boolean>,
    val fragments: List<YrcpRamdiskBackend>,
    val outputImage: File,
) {
    val dirty: StateFlow<Boolean> = mutableDirty.asStateFlow()

    suspend fun dump() {
        session.dump()
        check(outputImage.isFile && outputImage.length() > 0L) {
            "ksud reported success without creating the rebuilt image"
        }
        mutableDirty.value = false
    }

    suspend fun close() {
        session.close()
    }

    fun closeNow() {
        session.closeNow()
    }

    companion object {
        suspend fun open(
            ksudPath: String,
            sourceImage: File,
            outputImage: File,
            stagingDirectory: File,
            rootDisplayName: String,
        ): YrcpRamdiskImage = withContext(Dispatchers.IO) {
            val session = YrcpSession.start(
                ksudPath = ksudPath,
                sourceImage = sourceImage,
                outputImage = outputImage,
            )
            try {
                val descriptors = session.listRamdisks()
                check(descriptors.isNotEmpty()) {
                    "ksud did not expose any editable ramdisk fragments"
                }
                val dirty = MutableStateFlow(session.initialDirty)
                val backends = descriptors.map { fragment ->
                    YrcpRamdiskBackend.create(
                        session = session,
                        stagingDirectory = stagingDirectory,
                        rootDisplayName = if (descriptors.size == 1) {
                            rootDisplayName
                        } else {
                            fragment.name
                        },
                        dirty = dirty,
                        fragment = fragment,
                    )
                }
                YrcpRamdiskImage(
                    session = session,
                    mutableDirty = dirty,
                    fragments = backends,
                    outputImage = outputImage,
                )
            } catch (error: Throwable) {
                session.closeNow()
                throw error
            }
        }
    }
}

internal data class RamdiskFragmentInfo(
    val index: Int,
    val rootNodeId: Long,
    val packedSize: Long,
    val vendorType: Long,
    val isVendor: Boolean,
    val name: String,
    val compression: String,
    val boardId: List<Long>,
)

internal data class ElfHeaderInfo(
    val apiVersion: UInt,
    val headerFlags: UInt,
    val fileSize: ULong,
    val ident: ByteArray,
    val elfClass: Int,
    val dataEncoding: Int,
    val identVersion: Int,
    val osAbi: Int,
    val abiVersion: Int,
    val type: Int,
    val machine: Int,
    val elfVersion: UInt,
    val entry: ULong,
    val programHeaderOffset: ULong,
    val sectionHeaderOffset: ULong,
    val flags: UInt,
    val headerSize: Int,
    val programHeaderEntrySize: Int,
    val programHeaderCount: Int,
    val sectionHeaderEntrySize: Int,
    val sectionHeaderCount: Int,
    val sectionNameIndex: Int,
)

internal data class YrcpBinaryInfo(val revision: Long, val size: Long)

private class YrcpBinarySource(
    val session: YrcpSession,
    override val id: EntryId,
    val info: YrcpBinaryInfo,
) : BinaryDataSource {
    override val revision: String = session.binarySessionId + ":" + info.revision
    override val size: Long = info.size
    override suspend fun read(offset: Long, length: Int): BinaryBytes {
        require(offset in 0..size && length in 0..1024 * 1024)
        return BinaryBytes.copyOf(session.readAtRevision(id.toNodeId(), info.revision,
            offset, minOf(length.toLong(), size - offset).toInt()))
    }
}

private class YrcpStatusException(message: String) : IOException(message)

internal class YrcpSession private constructor(
    private val process: Process,
) {
    private val requests = BufferedOutputStream(process.outputStream)
    private val responses = BufferedInputStream(process.inputStream)
    private val mutex = Mutex()
    private val diagnostics = ArrayDeque<String>()
    private var requestId = 1L
    private var closed = false
    val binarySessionId: String = java.util.UUID.randomUUID().toString()
    var supportsBinaryEditing: Boolean = false
        private set

    var rootNodeId: Long = 0
        private set
    var maxContentSize: Long = DEFAULT_MAX_CONTENT_SIZE
        private set
    var initialDirty: Boolean = false
        private set

    private val diagnosticThread = Thread(
        {
            runCatching {
                process.errorStream.bufferedReader().useLines { lines ->
                    lines.forEach { line ->
                        Log.d(TAG, line)
                        synchronized(diagnostics) {
                            if (diagnostics.size == MAX_DIAGNOSTIC_LINES) {
                                diagnostics.removeFirst()
                            }
                            diagnostics.addLast(line)
                        }
                    }
                }
            }
        },
        "yrcp-ksud-stderr",
    ).apply {
        isDaemon = true
        start()
    }

    suspend fun binaryInfo(nodeId: Long): YrcpBinaryInfo =
        requestBytes(OPCODE_BINARY_INFO, payloadOf { writeU64(nodeId) }).decodeBinaryInfo()

    suspend fun readAtRevision(nodeId: Long, revision: Long, offset: Long, length: Int): ByteArray {
        require(length in 0..1024 * 1024 && offset >= 0)
        val bytes = requestBytes(OPCODE_READ_AT_REVISION, payloadOf {
            writeU64(nodeId); writeU64(revision); writeU64(offset); writeU64(length.toLong())
        })
        if (bytes.size != length) throw IOException("Incomplete versioned read")
        return bytes
    }

    suspend fun applyEdits(nodeId: Long, revision: Long, request: BinarySaveRequest): YrcpBinaryInfo {
        require(request.size in 0..maxContentSize && request.pieces.size <= 262_144)
        var payloadSize = 36L
        var outputSize = 0L
        for (piece in request.pieces) {
            require(piece.length > 0 && piece.length <= request.size - outputSize)
            outputSize += piece.length
            val encodedSize = when (piece) {
                is BinaryPiece.Original -> {
                    require(piece.offset >= 0 && piece.offset <= request.source.size &&
                        piece.length <= request.source.size - piece.offset)
                    17L
                }
                is BinaryPiece.Inserted -> {
                    require(piece.offset >= 0 && piece.length <= Long.MAX_VALUE - piece.offset)
                    9L + piece.length
                }
            }
            require(encodedSize <= maxContentSize + MAX_CONTROL_PAYLOAD - payloadSize)
            payloadSize += encodedSize
        }
        require(outputSize == request.size)
        return exchange(OPCODE_APPLY_EDITS, payloadSize, { output ->
            val writer = LittleEndianOutput(output)
            writer.writeU64(nodeId)
            writer.writeU64(revision)
            writer.writeU64(request.source.size)
            writer.writeU64(request.size)
            writer.writeU32(request.pieces.size.toLong())
            for (piece in request.pieces) {
                when (piece) {
                    is BinaryPiece.Original -> {
                        writer.writeByte(0); writer.writeU64(piece.offset); writer.writeU64(piece.length)
                    }
                    is BinaryPiece.Inserted -> {
                        writer.writeByte(1); writer.writeU64(piece.length)
                        var sent = 0L
                        while (sent < piece.length) {
                            val length = minOf(65536L, piece.length - sent).toInt()
                            val bytes = request.readInserted(piece.offset + sent, length)
                            if (bytes.size != length) throw IOException("Incomplete edit storage range")
                            output.write(bytes.toByteArray())
                            sent += length
                        }
                    }
                }
            }
        }) { body ->
            if (body.remaining != 16L) throw IOException("Invalid binary commit response")
            body.readExactly(16).decodeBinaryInfo()
        }
    }

    suspend fun stat(nodeId: Long): YrcpNode =
        requestBytes(OPCODE_STAT, payloadOf { writeU64(nodeId) }).decodeSingleNode()

    suspend fun list(nodeId: Long): List<YrcpNode> =
        requestBytes(OPCODE_LIST, payloadOf { writeU64(nodeId) }).decodeNodeList()

    suspend fun listRamdisks(): List<RamdiskFragmentInfo> =
        requestBytes(OPCODE_LIST_RAMDISKS, ByteArray(0)).decodeRamdiskList()

    suspend fun elfHeader(nodeId: Long): ElfHeaderInfo =
        requestBytes(
            OPCODE_ELF_HEADER,
            payloadOf { writeU64(nodeId) },
        ).decodeElfHeaderInfo()

    suspend fun createFile(
        parentId: Long,
        name: String,
        permissions: Int,
        uid: Long,
        gid: Long,
    ): Long = requestBytes(
        OPCODE_CREATE_FILE,
        payloadOf {
            writeU64(parentId)
            writeU32(permissions.toLong())
            writeU32(uid)
            writeU32(gid)
            writeString(name)
        },
    ).decodeCreatedId()

    suspend fun createDirectory(
        parentId: Long,
        name: String,
        permissions: Int,
        uid: Long,
        gid: Long,
    ): Long = requestBytes(
        OPCODE_CREATE_DIRECTORY,
        payloadOf {
            writeU64(parentId)
            writeU32(permissions.toLong())
            writeU32(uid)
            writeU32(gid)
            writeString(name)
        },
    ).decodeCreatedId()

    suspend fun createSymbolicLink(
        parentId: Long,
        name: String,
        target: String,
        uid: Long,
        gid: Long,
    ): Long = requestBytes(
        OPCODE_CREATE_SYMBOLIC_LINK,
        payloadOf {
            writeU64(parentId)
            writeU32(uid)
            writeU32(gid)
            writeString(name)
            writeString(target)
        },
    ).decodeCreatedId()

    suspend fun createHardLink(
        parentId: Long,
        name: String,
        targetId: Long,
    ): Long = requestBytes(
        OPCODE_CREATE_HARD_LINK,
        payloadOf {
            writeU64(parentId)
            writeU64(targetId)
            writeString(name)
        },
    ).decodeCreatedId()

    suspend fun copy(nodeId: Long, destinationId: Long, name: String): Long =
        requestBytes(
            OPCODE_COPY,
            payloadOf {
                writeU64(nodeId)
                writeU64(destinationId)
                writeString(name)
            },
        ).decodeCreatedId()

    suspend fun move(nodeId: Long, destinationId: Long, name: String) {
        requestBytes(
            OPCODE_MOVE,
            payloadOf {
                writeU64(nodeId)
                writeU64(destinationId)
                writeString(name)
            },
        ).requireEmpty()
    }

    suspend fun remove(nodeId: Long, recursive: Boolean) {
        requestBytes(
            OPCODE_REMOVE,
            payloadOf {
                writeU64(nodeId)
                writeByte(if (recursive) 1 else 0)
            },
        ).requireEmpty()
    }

    suspend fun updateMetadata(nodeId: Long, patch: UnixMetadataPatch) {
        var mask = 0
        if (patch.permissions != null) mask = mask or 1
        if (patch.uid != null) mask = mask or 2
        if (patch.gid != null) mask = mask or 4
        requestBytes(
            OPCODE_UPDATE_METADATA,
            payloadOf {
                writeU64(nodeId)
                writeU32(mask.toLong())
                patch.permissions?.let { writeU32(it.toLong()) }
                patch.uid?.let(::writeU32)
                patch.gid?.let(::writeU32)
            },
        ).requireEmpty()
    }

    suspend fun replace(nodeId: Long, source: File) {
        exchange(
            opcode = OPCODE_REPLACE,
            payloadSize = U64_SIZE.toLong() + source.length(),
            writePayload = { output ->
                LittleEndianOutput(output).writeU64(nodeId)
                source.inputStream().buffered().use { input ->
                    input.copyTo(output)
                }
            },
        ) { body ->
            if (body.remaining != 0L) {
                throw IOException("Expected an empty YRCP response")
            }
        }
    }

    suspend fun <T> read(
        nodeId: Long,
        length: Long,
        reader: suspend (InputStream) -> T,
    ): T {
        require(length >= 0) { "Content length cannot be negative" }
        val payload = payloadOf {
            writeU64(nodeId)
            writeU64(0)
            writeU64(length)
        }
        return exchange(
            opcode = OPCODE_READ,
            payloadSize = payload.size.toLong(),
            writePayload = { it.write(payload) },
        ) { body ->
            reader(body)
        }
    }

    suspend fun readSnapshot(nodeId: Long, onProgress: (FileByteProgress) -> Unit, consume: (ByteArray, Int) -> Unit) = withContext(Dispatchers.IO) {
        val progress = RamdiskProgressReporter(onProgress)
        try {
            if (supportsBinaryEditing) {
                val info = binaryInfo(nodeId)
                progress.update(0, info.size)
                var offset = 0L
                do {
                    currentCoroutineContext().ensureActive()
                    val count = minOf(65536L, info.size - offset).toInt()
                    val bytes = readAtRevision(nodeId, info.revision, offset, count)
                    currentCoroutineContext().ensureActive()
                    if (count > 0) consume(bytes, count)
                    offset += count
                    progress.update(offset, info.size)
                } while (offset < info.size)
            } else {
                // A single READ response is a snapshot, with its actual length in the frame.
                // The session mutex remains held until the response has been consumed or drained.
                readWholeFile(nodeId) { input, size ->
                    progress.update(0, size)
                    val buffer = ByteArray(65536)
                    var copied = 0L
                    while (copied < size) {
                        currentCoroutineContext().ensureActive()
                        val count = input.read(buffer, 0, minOf(buffer.size.toLong(), size - copied).toInt())
                        if (count <= 0) throw EOFException("Incomplete file snapshot")
                        consume(buffer, count)
                        copied += count
                        progress.update(copied, size)
                    }
                }
            }
            currentCoroutineContext().ensureActive()
        } catch (changed: BinaryRevisionConflictException) {
            throw FileContentChangedException(changed)
        }
    }

    suspend fun readWholeFile(nodeId: Long, reader: suspend (InputStream, Long) -> Unit) {
        val payload = payloadOf { writeU64(nodeId); writeU64(0); writeU64(maxContentSize) }
        exchange(OPCODE_READ, payload.size.toLong(), { it.write(payload) }) { body ->
            require(body.remaining <= maxContentSize) { "File snapshot exceeds content limit" }
            reader(body, body.remaining)
        }
    }

    suspend fun dump() {
        requestBytes(OPCODE_DUMP, ByteArray(0)).requireEmpty()
    }

    suspend fun close() {
        if (closed) return
        runCatching {
            requestBytes(OPCODE_CLOSE, ByteArray(0)).requireEmpty()
        }
        closeNow()
        withContext(Dispatchers.IO) {
            if (!process.waitFor(PROCESS_EXIT_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                process.destroyForcibly()
            }
        }
    }

    fun closeNow() {
        if (closed) return
        closed = true
        runCatching { requests.close() }
        runCatching { responses.close() }
        runCatching { process.errorStream.close() }
        if (process.isAlive) process.destroy()
    }

    private suspend fun requestBytes(opcode: Int, payload: ByteArray): ByteArray =
        exchange(
            opcode = opcode,
            payloadSize = payload.size.toLong(),
            writePayload = { it.write(payload) },
        ) { body ->
            if (body.remaining > MAX_CONTROL_PAYLOAD) {
                throw IOException("YRCP response exceeds the control payload limit")
            }
            body.readExactly(body.remaining.toInt())
        }

    private suspend fun <T> exchange(
        opcode: Int,
        payloadSize: Long,
        writePayload: suspend (OutputStream) -> Unit,
        readBody: suspend (LimitedInputStream) -> T,
    ): T = withContext(Dispatchers.IO) {
        mutex.lock()
        var responseConsumed = false
        try {
            checkSession()
            require(payloadSize >= 0) { "YRCP payload size cannot be negative" }
            val currentRequestId = requestId++
            val writer = LittleEndianOutput(requests)
            writer.writeBytes(PROTOCOL_MAGIC)
            writer.writeU16(PROTOCOL_VERSION)
            writer.writeU16(opcode)
            writer.writeU32(currentRequestId)
            writer.writeU64(payloadSize)
            writePayload(requests)
            requests.flush()

            val reader = LittleEndianInput(responses)
            val magic = reader.readExactly(PROTOCOL_MAGIC.size)
            if (!magic.contentEquals(PROTOCOL_MAGIC)) {
                throw IOException("Invalid YRCP response magic")
            }
            val version = reader.readU16()
            val responseOpcode = reader.readU16()
            val responseRequestId = reader.readU32()
            val responsePayloadSize = reader.readU64()
            if (
                version != PROTOCOL_VERSION ||
                responseOpcode != (opcode or RESPONSE_FLAG) ||
                responseRequestId != currentRequestId ||
                responsePayloadSize < U32_SIZE.toLong()
            ) {
                throw IOException("YRCP response header does not match its request")
            }

            val status = reader.readU32()
            val body = LimitedInputStream(
                responses,
                responsePayloadSize - U32_SIZE.toLong(),
            )
            if (status != STATUS_OK.toLong()) {
                body.drain()
                responseConsumed = true
                if (status == 7L) throw BinaryRevisionConflictException()
                throw YrcpStatusException(
                    "ksud ramdisk editor rejected opcode $opcode with status $status" +
                        diagnosticSuffix()
                )
            }
            try {
                readBody(body)
            } finally {
                body.drain()
                responseConsumed = true
            }
        } catch (error: EOFException) {
            closeNow()
            throw IOException("ksud ramdisk editor closed the protocol stream${diagnosticSuffix()}", error)
        } catch (error: Throwable) {
            if (!responseConsumed) closeNow()
            throw error
        } finally {
            mutex.unlock()
        }
    }

    private fun checkSession() {
        check(!closed) { "The ramdisk editor session is already closed" }
        if (!process.isAlive) {
            throw IOException(
                "ksud ramdisk editor exited with code ${process.exitValue()}${diagnosticSuffix()}"
            )
        }
    }

    private fun diagnosticSuffix(): String {
        val line = synchronized(diagnostics) { diagnostics.lastOrNull() }
        return line?.let { ": $it" }.orEmpty()
    }

    private suspend fun hello() {
        val payload = requestBytes(OPCODE_HELLO, ByteArray(0))
        val reader = LittleEndianInput(ByteArrayInputStream(payload))
        val negotiatedVersion = reader.readU32()
        rootNodeId = reader.readU64()
        maxContentSize = reader.readU64()
        reader.readU64() // Maximum entry count; the UI does not need it.
        val capabilities = reader.readU32()
        supportsBinaryEditing = capabilities and (3L shl 16) == (3L shl 16)
        initialDirty = reader.readByte() != 0
        if (negotiatedVersion != PROTOCOL_VERSION.toLong()) {
            throw IOException("Unsupported YRCP version $negotiatedVersion")
        }
        val missing = REQUIRED_CAPABILITIES and capabilities.inv()
        if (missing != 0L) {
            throw IOException("ksud ramdisk editor is missing capabilities 0x${missing.toString(16)}")
        }
        if (reader.available() != 0) {
            throw IOException("Unexpected trailing bytes in the YRCP HELLO response")
        }
    }

    companion object {
        suspend fun start(
            ksudPath: String,
            sourceImage: File,
            outputImage: File,
        ): YrcpSession = withContext(Dispatchers.IO) {
            require(sourceImage.isFile && sourceImage.length() > 0L) {
                "The source boot image is missing or empty"
            }
            outputImage.parentFile?.mkdirs()
            val process = ProcessBuilder(
                ksudPath,
                "boot-ramdisk-editor",
                sourceImage.absolutePath,
                outputImage.absolutePath,
            )
                .redirectErrorStream(false)
                .start()
            connect(process)
        }

        suspend fun connect(process: Process): YrcpSession = YrcpSession(process).also { session ->
            try {
                session.hello()
            } catch (error: Throwable) {
                session.closeNow()
                throw error
            }
        }
    }
}

internal data class YrcpNode(
    val id: Long,
    val parentId: Long,
    val size: Long,
    val inode: Long,
    val mode: Long,
    val uid: Long,
    val gid: Long,
    val hardLinkCount: Long,
    val modifiedSeconds: Long,
    val deviceMajor: Long,
    val deviceMinor: Long,
    val specialDeviceMajor: Long,
    val specialDeviceMinor: Long,
    val synthetic: Boolean,
    val contentKind: YrcpContentKind,
    val name: String,
    val path: String,
    val linkTarget: String?,
) {
    fun toFileEntry(rootNodeId: Long, rootDisplayName: String): FileEntry {
        val type = mode.toFileEntryType()
        val useSpecialDevice = type == FileEntryType.BLOCK_DEVICE ||
            type == FileEntryType.CHARACTER_DEVICE
        return FileEntry(
            id = id.toEntryId(),
            parentId = if (id == rootNodeId) null else parentId.toEntryId(),
            name = if (id == rootNodeId) rootDisplayName else name,
            type = type,
            size = size,
            modifiedAtMillis = modifiedSeconds
                .takeIf { it <= Long.MAX_VALUE / 1_000L }
                ?.times(1_000L),
            mimeType = when (contentKind) {
                YrcpContentKind.UNKNOWN -> null
                YrcpContentKind.ELF -> ELF_MIME_TYPE
            },
            unixMetadata = UnixMetadata(
                permissions = (mode and PERMISSION_MASK).toInt(),
                uid = uid,
                gid = gid,
                inode = inode,
                hardLinkCount = hardLinkCount,
                deviceMajor = if (useSpecialDevice) specialDeviceMajor else deviceMajor,
                deviceMinor = if (useSpecialDevice) specialDeviceMinor else deviceMinor,
            ),
            linkTarget = linkTarget,
        )
    }
}

internal enum class YrcpContentKind {
    UNKNOWN,
    ELF;

    companion object {
        fun fromWire(value: Int): YrcpContentKind =
            when (value) {
                CONTENT_KIND_ELF -> ELF
                else -> UNKNOWN
            }
    }
}

internal fun ByteArray.decodeSingleNode(): YrcpNode {
    val input = LittleEndianInput(ByteArrayInputStream(this))
    return input.readNode().also {
        if (input.available() != 0) throw IOException("Unexpected trailing bytes after node record")
    }
}

internal fun ByteArray.decodeBinaryInfo(): YrcpBinaryInfo {
    if (size != 16) throw IOException("Invalid binary info size")
    val reader = LittleEndianInput(ByteArrayInputStream(this))
    val revision = reader.readU64()
    val length = reader.readU64()
    if (revision == 0L) throw IOException("Invalid binary revision")
    return YrcpBinaryInfo(revision, length)
}

internal fun ByteArray.decodeElfHeaderInfo(): ElfHeaderInfo {
    val input = LittleEndianInput(ByteArrayInputStream(this))
    val schemaVersion = input.readU16()
    val fixedSize = input.readU16()
    if (schemaVersion != ELF_HEADER_SCHEMA_VERSION) {
        throw IOException("Unsupported ELF header schema $schemaVersion")
    }
    if (fixedSize < ELF_HEADER_WIRE_SIZE || fixedSize > size) {
        throw IOException("Invalid ELF header structure size $fixedSize")
    }

    val result = ElfHeaderInfo(
        apiVersion = input.readU32().toUInt(),
        headerFlags = input.readU32().toUInt(),
        fileSize = run {
            input.readU32() // Reserved.
            input.readULong()
        },
        ident = input.readExactly(ELF_IDENT_SIZE),
        elfClass = input.readByte(),
        dataEncoding = input.readByte(),
        identVersion = input.readByte(),
        osAbi = input.readByte(),
        abiVersion = input.readByte(),
        type = run {
            input.readExactly(3) // Reserved.
            input.readU16()
        },
        machine = input.readU16(),
        elfVersion = input.readU32().toUInt(),
        entry = input.readULong(),
        programHeaderOffset = input.readULong(),
        sectionHeaderOffset = input.readULong(),
        flags = input.readU32().toUInt(),
        headerSize = input.readU16(),
        programHeaderEntrySize = input.readU16(),
        programHeaderCount = input.readU16(),
        sectionHeaderEntrySize = input.readU16(),
        sectionHeaderCount = input.readU16(),
        sectionNameIndex = input.readU16(),
    )
    if (fixedSize > ELF_HEADER_WIRE_SIZE) {
        input.readExactly(fixedSize - ELF_HEADER_WIRE_SIZE)
    }
    if (input.available() != 0) {
        throw IOException("Unexpected trailing bytes in ELF header response")
    }
    return result
}

private fun ByteArray.decodeNodeList(): List<YrcpNode> {
    val input = LittleEndianInput(ByteArrayInputStream(this))
    val count = input.readU32()
    if (count > MAX_NODE_COUNT) throw IOException("YRCP node list is too large")
    return List(count.toInt()) { input.readNode() }.also {
        if (input.available() != 0) {
            throw IOException("Unexpected trailing bytes after node list")
        }
    }
}

private fun ByteArray.decodeCreatedId(): Long {
    val input = LittleEndianInput(ByteArrayInputStream(this))
    val id = input.readU64()
    if (input.available() != 0) throw IOException("Unexpected trailing bytes after created node ID")
    return id
}

private fun ByteArray.decodeRamdiskList(): List<RamdiskFragmentInfo> {
    val input = LittleEndianInput(ByteArrayInputStream(this))
    val count = input.readU32()
    if (count == 0L || count > MAX_RAMDISK_COUNT) {
        throw IOException("Invalid YRCP ramdisk fragment count: $count")
    }
    return List(count.toInt()) { expectedIndex ->
        val index = input.readU32()
        if (index != expectedIndex.toLong()) {
            throw IOException("YRCP ramdisk fragments are not in canonical order")
        }
        RamdiskFragmentInfo(
            index = expectedIndex,
            rootNodeId = input.readU64(),
            packedSize = input.readU64(),
            vendorType = input.readU32(),
            isVendor = input.readByte() != 0,
            name = input.readString(),
            compression = input.readString(),
            boardId = List(VENDOR_BOARD_ID_WORD_COUNT) { input.readU32() },
        )
    }.also {
        if (input.available() != 0) {
            throw IOException("Unexpected trailing bytes after ramdisk fragment list")
        }
    }
}

private fun ByteArray.requireEmpty() {
    if (isNotEmpty()) throw IOException("Expected an empty YRCP response")
}

private fun LittleEndianInput.readNode(): YrcpNode =
    YrcpNode(
        id = readU64(),
        parentId = readU64(),
        size = readU64(),
        inode = readU32(),
        mode = readU32(),
        uid = readU32(),
        gid = readU32(),
        hardLinkCount = readU32(),
        modifiedSeconds = readU32(),
        deviceMajor = readU32(),
        deviceMinor = readU32(),
        specialDeviceMajor = readU32(),
        specialDeviceMinor = readU32(),
        synthetic = readByte() != 0,
        contentKind = YrcpContentKind.fromWire(readByte()),
        name = readString(),
        path = readString(),
        linkTarget = readOptionalString(),
    )

private class LittleEndianOutput(
    private val output: OutputStream,
) {
    fun writeByte(value: Int) {
        output.write(value and 0xFF)
    }

    fun writeBytes(value: ByteArray) {
        output.write(value)
    }

    fun writeU16(value: Int) {
        repeat(U16_SIZE) { shift ->
            writeByte(value ushr (shift * 8))
        }
    }

    fun writeU32(value: Long) {
        require(value in 0..UINT32_MAX) { "Value does not fit in u32: $value" }
        repeat(U32_SIZE) { shift ->
            writeByte((value ushr (shift * 8)).toInt())
        }
    }

    fun writeU64(value: Long) {
        require(value >= 0) { "Negative values are not supported by this YRCP client" }
        repeat(U64_SIZE) { shift ->
            writeByte((value ushr (shift * 8)).toInt())
        }
    }

    fun writeString(value: String) {
        val bytes = value.toByteArray(Charsets.UTF_8)
        writeU32(bytes.size.toLong())
        writeBytes(bytes)
    }
}

private class LittleEndianInput(
    private val input: InputStream,
) {
    fun readByte(): Int {
        val value = input.read()
        if (value < 0) throw EOFException()
        return value
    }

    fun readExactly(size: Int): ByteArray =
        ByteArray(size).also { bytes ->
            var offset = 0
            while (offset < bytes.size) {
                val count = input.read(bytes, offset, bytes.size - offset)
                if (count < 0) throw EOFException()
                offset += count
            }
        }

    fun readU16(): Int {
        var value = 0
        repeat(U16_SIZE) { shift ->
            value = value or (readByte() shl (shift * 8))
        }
        return value
    }

    fun readU32(): Long {
        var value = 0L
        repeat(U32_SIZE) { shift ->
            value = value or (readByte().toLong() shl (shift * 8))
        }
        return value
    }

    fun readU64(): Long {
        val value = readULong()
        if (value > Long.MAX_VALUE.toULong()) {
            throw IOException("YRCP u64 value exceeds the signed Android range")
        }
        return value.toLong()
    }

    fun readULong(): ULong {
        var value = 0uL
        repeat(U64_SIZE) { shift ->
            value = value or (readByte().toULong() shl (shift * 8))
        }
        return value
    }

    fun readString(): String {
        val length = readU32()
        if (length > MAX_CONTROL_PAYLOAD) throw IOException("YRCP string is too large")
        return String(readExactly(length.toInt()), Charsets.UTF_8)
    }

    fun readOptionalString(): String? {
        val length = readU32()
        if (length == UINT32_MAX) return null
        if (length > MAX_CONTROL_PAYLOAD) throw IOException("YRCP string is too large")
        return String(readExactly(length.toInt()), Charsets.UTF_8)
    }

    fun available(): Int = input.available()
}

private class LimitedInputStream(
    private val input: InputStream,
    remainingBytes: Long,
) : InputStream() {
    var remaining: Long = remainingBytes
        private set

    override fun read(): Int {
        if (remaining == 0L) return -1
        val value = input.read()
        if (value < 0) throw EOFException()
        remaining--
        return value
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (remaining == 0L) return -1
        val requested = min(length.toLong(), remaining).toInt()
        val count = input.read(buffer, offset, requested)
        if (count < 0) throw EOFException()
        remaining -= count
        return count
    }

    fun readExactly(size: Int): ByteArray =
        ByteArray(size).also { bytes ->
            var offset = 0
            while (offset < bytes.size) {
                val count = read(bytes, offset, bytes.size - offset)
                if (count < 0) throw EOFException()
                offset += count
            }
        }

    fun drain() {
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        while (remaining > 0L) {
            val count = read(buffer, 0, min(buffer.size.toLong(), remaining).toInt())
            if (count < 0) throw EOFException()
        }
    }
}

private inline fun payloadOf(block: LittleEndianOutput.() -> Unit): ByteArray {
    val bytes = ByteArrayOutputStream()
    LittleEndianOutput(bytes).block()
    return bytes.toByteArray()
}

private fun copyBounded(input: InputStream, output: OutputStream, maxBytes: Long) {
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    var total = 0L
    while (true) {
        val count = input.read(buffer)
        if (count < 0) return
        if (total > maxBytes - count) {
            throw IOException("File content exceeds the ksud ramdisk editor limit")
        }
        output.write(buffer, 0, count)
        total += count
    }
}

private fun Long.toEntryId(): EntryId = EntryId(toString())

private fun EntryId.toNodeId(): Long =
    value.toLongOrNull() ?: throw IOException("Invalid CPIO node ID: $value")

private fun Long.toFileEntryType(): FileEntryType =
    when (this and FILE_TYPE_MASK) {
        FILE_TYPE_REGULAR -> FileEntryType.REGULAR_FILE
        FILE_TYPE_DIRECTORY -> FileEntryType.DIRECTORY
        FILE_TYPE_SYMBOLIC_LINK -> FileEntryType.SYMBOLIC_LINK
        FILE_TYPE_BLOCK_DEVICE -> FileEntryType.BLOCK_DEVICE
        FILE_TYPE_CHARACTER_DEVICE -> FileEntryType.CHARACTER_DEVICE
        FILE_TYPE_FIFO -> FileEntryType.FIFO
        FILE_TYPE_SOCKET -> FileEntryType.SOCKET
        else -> FileEntryType.UNKNOWN
    }

private const val TAG = "YrcpRamdiskBackend"
private val PROTOCOL_MAGIC = byteArrayOf('Y'.code.toByte(), 'R'.code.toByte(), 'C'.code.toByte(), 'P'.code.toByte())
private const val PROTOCOL_VERSION = 3
private const val RESPONSE_FLAG = 0x8000
private const val STATUS_OK = 0

private const val OPCODE_HELLO = 1
private const val OPCODE_STAT = 2
private const val OPCODE_LIST = 3
private const val OPCODE_READ = 4
private const val OPCODE_REPLACE = 5
private const val OPCODE_CREATE_FILE = 6
private const val OPCODE_CREATE_DIRECTORY = 7
private const val OPCODE_CREATE_SYMBOLIC_LINK = 8
private const val OPCODE_CREATE_HARD_LINK = 9
private const val OPCODE_COPY = 10
private const val OPCODE_MOVE = 11
private const val OPCODE_REMOVE = 12
private const val OPCODE_UPDATE_METADATA = 13
private const val OPCODE_DUMP = 14
private const val OPCODE_CLOSE = 15
private const val OPCODE_LIST_RAMDISKS = 16
private const val OPCODE_ELF_HEADER = 17

private const val CAPABILITY_READ = 1L shl 0
private const val CAPABILITY_REPLACE = 1L shl 1
private const val CAPABILITY_CREATE_FILE = 1L shl 2
private const val CAPABILITY_CREATE_DIRECTORY = 1L shl 3
private const val CAPABILITY_CREATE_SYMBOLIC_LINK = 1L shl 4
private const val CAPABILITY_CREATE_HARD_LINK = 1L shl 5
private const val CAPABILITY_COPY = 1L shl 6
private const val CAPABILITY_MOVE = 1L shl 7
private const val CAPABILITY_REMOVE = 1L shl 8
private const val CAPABILITY_UPDATE_METADATA = 1L shl 9
private const val CAPABILITY_ATOMIC_DUMP = 1L shl 10
private const val CAPABILITY_RANGED_READ = 1L shl 11
private const val CAPABILITY_MULTIPLE_RAMDISKS = 1L shl 13
private const val CAPABILITY_CONTENT_TYPES = 1L shl 14
private const val CAPABILITY_ELF_HEADER = 1L shl 15
private const val REQUIRED_CAPABILITIES =
    CAPABILITY_READ or CAPABILITY_REPLACE or CAPABILITY_CREATE_FILE or
        CAPABILITY_CREATE_DIRECTORY or CAPABILITY_CREATE_SYMBOLIC_LINK or
        CAPABILITY_CREATE_HARD_LINK or CAPABILITY_COPY or CAPABILITY_MOVE or
        CAPABILITY_REMOVE or CAPABILITY_UPDATE_METADATA or CAPABILITY_ATOMIC_DUMP or
        CAPABILITY_RANGED_READ or CAPABILITY_MULTIPLE_RAMDISKS or
        CAPABILITY_CONTENT_TYPES or CAPABILITY_ELF_HEADER

private const val U16_SIZE = 2
private const val U32_SIZE = 4
private const val U64_SIZE = 8
private const val ELF_HEADER_SCHEMA_VERSION = 1
private const val ELF_HEADER_WIRE_SIZE = 96
private const val ELF_IDENT_SIZE = 16
private const val UINT32_MAX = 0xFFFF_FFFFL
private const val MAX_CONTROL_PAYLOAD = 16L * 1024L * 1024L
private const val MAX_NODE_COUNT = 1_000_000L
private const val MAX_RAMDISK_COUNT = 65_535L
private const val VENDOR_BOARD_ID_WORD_COUNT = 16
private const val DEFAULT_MAX_CONTENT_SIZE = 256L * 1024L * 1024L
private const val MAX_DIAGNOSTIC_LINES = 64
private const val PROCESS_EXIT_TIMEOUT_SECONDS = 2L

private const val DEFAULT_FILE_PERMISSIONS = 0x1A4 // 0644
private const val DEFAULT_DIRECTORY_PERMISSIONS = 0x1ED // 0755
private const val PERMISSION_MASK = 0xFFFL
private const val FILE_TYPE_MASK = 0xF000L
private const val FILE_TYPE_FIFO = 0x1000L
private const val FILE_TYPE_CHARACTER_DEVICE = 0x2000L
private const val FILE_TYPE_DIRECTORY = 0x4000L
private const val FILE_TYPE_BLOCK_DEVICE = 0x6000L
private const val FILE_TYPE_REGULAR = 0x8000L
private const val FILE_TYPE_SYMBOLIC_LINK = 0xA000L
private const val FILE_TYPE_SOCKET = 0xC000L

private const val CONTENT_KIND_ELF = 1
internal const val ELF_MIME_TYPE = "application/x-elf"

private const val OPCODE_BINARY_INFO = 18
private const val OPCODE_READ_AT_REVISION = 19
private const val OPCODE_APPLY_EDITS = 20
