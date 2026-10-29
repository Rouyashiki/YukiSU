@file:Suppress("UnstableApiUsage")

import groovy.json.JsonOutput
import groovy.json.JsonSlurper
import org.gradle.api.artifacts.component.ModuleComponentIdentifier
import java.io.ByteArrayInputStream
import java.security.MessageDigest
import java.util.zip.ZipInputStream
import com.android.build.api.artifact.ScopedArtifact
import com.android.build.api.artifact.SingleArtifact
import com.android.build.api.instrumentation.AsmClassVisitorFactory
import com.android.build.api.instrumentation.ClassContext
import com.android.build.api.instrumentation.ClassData
import com.android.build.api.instrumentation.FramesComputationMode
import com.android.build.api.instrumentation.InstrumentationParameters
import com.android.build.api.instrumentation.InstrumentationScope
import com.android.build.api.variant.BuiltArtifactsLoader
import com.android.build.api.variant.ScopedArtifacts
import com.android.build.gradle.tasks.PackageAndroidArtifact
import org.gradle.api.DefaultTask
import org.gradle.api.file.Directory
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFile
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.provider.ListProperty
import org.gradle.api.provider.Property
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.Internal
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.OutputFile
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import java.io.File
import java.util.zip.ZipFile
import org.objectweb.asm.ClassReader
import org.objectweb.asm.ClassVisitor
import org.objectweb.asm.FieldVisitor
import org.objectweb.asm.Label
import org.objectweb.asm.MethodVisitor
import org.objectweb.asm.Opcodes

plugins {
    alias(libs.plugins.agp.app)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
    id("kotlin-parcelize")


}

abstract class GenerateOpenSourceLicenses : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val catalog: RegularFileProperty

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val licenseTexts: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val nativeInputs: RegularFileProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val nativeSources: ConfigurableFileCollection

    @get:Input
    abstract val nativeRevisions: MapProperty<String, String>

    @get:Internal
    abstract val repositoryRoot: DirectoryProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val artifacts: ConfigurableFileCollection

    @get:Input
    abstract val coordinates: MapProperty<String, List<String>>

    @get:Input
    abstract val buildType: Property<String>

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @Suppress("UNCHECKED_CAST")
    @TaskAction
    fun generate() {
        fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
            .digest(bytes).joinToString("") { "%02x".format(it) }
        val inputs = JsonSlurper().parse(nativeInputs.get().asFile) as Map<String, List<String>>
        val nativeHashes = inputs.getValue("files").sorted().associateWith { path ->
            val file = repositoryRoot.get().file(path).asFile
            check(file.isFile) { "Missing native license input: $path" }
            sha256(file.readText().replace("\r\n", "\n").toByteArray())
        }
        check(nativeRevisions.get().keys == inputs.getValue("submodules").toSet()) {
            "Missing native submodule revisions"
        }
        val entries = JsonSlurper().parse(catalog.get().asFile) as List<Map<String, Any>>
        val declared = entries.flatMap { it["artifacts"] as? List<String> ?: emptyList() }
        check(declared.all { it.split(':').size == 2 }) {
            "License rules must use group:artifact without a version"
        }
        check(declared.size == declared.toSet().size) { "Duplicate modules in the license catalog" }
        val resolved = coordinates.get().toSortedMap()
        val missing = resolved.keys.filter { it.substringBeforeLast(':') !in declared }
        check(missing.isEmpty()) {
            "Missing licenses for runtime modules: $missing. Add their group:artifact and notices to manager/licenses/components.json."
        }
        check(entries.map { it["id"] }.distinct().size == entries.size) { "Duplicate license component IDs" }
        val output = outputDirectory.get().asFile
        output.deleteRecursively()
        val licenseDir = output.resolve("open_source/texts").apply { mkdirs() }
        fun store(text: String): String {
            check(text.isNotBlank()) { "Empty license text" }
            val normalized = text.replace("\r\n", "\n").trim() + "\n"
            val hash = sha256(normalized.toByteArray())
            licenseDir.resolve("$hash.txt").writeText(normalized)
            return "open_source/texts/$hash.txt"
        }
        fun collectNotices(bytes: ByteArray, prefix: String = ""): List<Pair<String, String>> {
            val notices = mutableListOf<Pair<String, String>>()
            ZipInputStream(ByteArrayInputStream(bytes)).use { zip ->
                var entry = zip.nextEntry
                while (entry != null) {
                    val name = entry.name
                    val leaf = name.substringAfterLast('/').lowercase()
                    if (!entry.isDirectory && Regex("(license|notice|copying|copyright)([._-].*)?").matches(leaf)
                        && !leaf.endsWith(".class")) {
                        val text = zip.readBytes().toString(Charsets.UTF_8)
                        if (text.isNotBlank()) notices += (prefix + name) to store(text)
                    } else if (!entry.isDirectory && name.endsWith(".jar") && prefix.isEmpty()) {
                        notices += collectNotices(zip.readBytes(), "$name!/")
                    }
                    entry = zip.nextEntry
                }
            }
            return notices
        }
        val filesByName = artifacts.files.associateBy { it.name }
        check(filesByName.size == artifacts.files.size) { "Ambiguous runtime artifact filenames" }
        val artifactHashes = resolved.mapValues { (_, names) ->
            names.sorted().map { name ->
                mapOf("file" to name, "sha256" to sha256(filesByName.getValue(name).readBytes()))
            }
        }
        val components = entries.mapNotNull { component ->
            val rules = component["artifacts"] as? List<String> ?: emptyList()
            val modules = resolved.keys.filter { it.substringBeforeLast(':') in rules }
            if (component.containsKey("artifacts") && modules.isEmpty()) return@mapNotNull null
            if (component["buildType"] != null && component["buildType"] != buildType.get()) return@mapNotNull null
            val revision = component["versionFrom"]?.let { nativeRevisions.get().getValue(it as String) }
            val source = component.getValue("source") as String
            check((revision != null) == source.contains("{revision}")) {
                "A versionFrom component must have a {revision} source template: ${component["id"]}"
            }
            val texts = (component["texts"] as List<String>).map { name ->
                val file = licenseTexts.get().asFile.resolve(name)
                check(file.isFile) { "Missing license text: $name" }
                mapOf("title" to name, "path" to store(file.readText()))
            }.toMutableList()
            (component["sourceTexts"] as? List<String> ?: emptyList()).forEach { path ->
                check(path in nativeHashes) { "Untracked license input: $path" }
                val stored = store(repositoryRoot.get().file(path).asFile.readText())
                if (texts.none { it["path"] == stored }) {
                    texts += mapOf("title" to path, "path" to stored)
                }
            }
            (component["mitLicenseFrom"] as? String)?.let { path ->
                check(path in nativeHashes) { "Untracked license input: $path" }
                val comments = Regex("/\\*(.*?)\\*/", RegexOption.DOT_MATCHES_ALL)
                    .findAll(repositoryRoot.get().file(path).asFile.readText())
                    .map { match ->
                        match.groupValues[1].trim { it.isWhitespace() || it == '*' }
                            .lineSequence().map { it.trimStart().removePrefix("*").removePrefix(" ") }
                            .joinToString("\n").trim()
                    }
                    .filter { it.startsWith("Copyright") && it.contains("Permission is hereby granted")
                        && it.contains("THE SOFTWARE IS PROVIDED") }
                    .toList()
                check(comments.size == 1) { "Expected one MIT license comment in $path" }
                val stored = store(comments.single())
                if (texts.none { it["path"] == stored }) {
                    texts += mapOf("title" to path, "path" to stored)
                }
            }
            check(texts.isNotEmpty()) { "Missing license for ${component["id"]}" }
            modules.forEach { coordinate ->
                resolved.getValue(coordinate).sorted().forEach { filename ->
                    collectNotices(filesByName.getValue(filename).readBytes()).forEach { (name, path) ->
                        if (texts.none { it["path"] == path }) {
                            texts += mapOf("title" to "$coordinate — $filename!/$name", "path" to path)
                        }
                    }
                }
            }
            mapOf(
                "id" to component.getValue("id"), "name" to component.getValue("name"),
                "license" to component.getValue("license"), "source" to source.replace("{revision}", revision ?: ""),
                "version" to (revision?.take(12)
                    ?: component["version"] ?: modules.map { it.substringAfterLast(':') }.distinct().joinToString(", ")),
                "artifacts" to modules, "texts" to texts,
            )
        }.sortedBy { (it.getValue("name") as String).lowercase() }
        output.resolve("open_source/components.json").writeText(JsonOutput.prettyPrint(JsonOutput.toJson(components)))
        output.resolve("open_source/inputs.json").writeText(JsonOutput.prettyPrint(JsonOutput.toJson(mapOf(
            "runtimeArtifacts" to artifactHashes,
            "nativeFiles" to nativeHashes,
            "nativeSubmodules" to nativeRevisions.get().toSortedMap(),
        ))))
        logger.lifecycle("License catalog: ${components.size} components, ${coordinates.get().size} runtime artifacts (${buildType.get()})")
    }
}

/** Replaces reflection-heavy library methods without changing their JVM interfaces. */
abstract class WebUiReflectionPatch : AsmClassVisitorFactory<InstrumentationParameters.None> {
    override fun isInstrumentable(classData: ClassData): Boolean =
        classData.className.replace('.', '/') in TARGETS

    override fun createClassVisitor(classContext: ClassContext, nextClassVisitor: ClassVisitor): ClassVisitor =
        object : ClassVisitor(Opcodes.ASM9, nextClassVisitor) {
            private val owner = classContext.currentClassData.className.replace('.', '/')
            private val target = TARGETS.getValue(owner)
            private val fields = linkedMapOf<String, String>()
            private var patched = false

            override fun visitField(access: Int, name: String, descriptor: String, signature: String?, value: Any?): FieldVisitor? {
                if (access and Opcodes.ACC_STATIC == 0) fields[name] = descriptor
                return super.visitField(access, name, descriptor, signature, value)
            }

            override fun visitMethod(access: Int, name: String, descriptor: String, signature: String?, exceptions: Array<out String>?): MethodVisitor? {
                val method = super.visitMethod(access, name, descriptor, signature, exceptions)
                if (name + descriptor != target) return method
                check(!patched) { "Duplicate WebUI patch target: $owner#$target" }
                patched = true
                method.visitCode()
                if (owner == WEB_COLORS) {
                    method.visitVarInsn(Opcodes.ALOAD, 0)
                    method.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/Object", "<init>", "()V", false)
                    method.visitVarInsn(Opcodes.ALOAD, 1)
                    method.visitMethodInsn(Opcodes.INVOKESTATIC, HELPER, "create", "(Landroidx/compose/material3/ColorScheme;)L$SNAPSHOT;", false)
                    method.visitVarInsn(Opcodes.ASTORE, 2)
                    COLOR_FIELDS.forEach { (field, type) ->
                        method.visitVarInsn(Opcodes.ALOAD, 0)
                        method.visitVarInsn(Opcodes.ALOAD, 2)
                        method.visitFieldInsn(Opcodes.GETFIELD, SNAPSHOT, field, type)
                        method.visitFieldInsn(Opcodes.PUTFIELD, owner, field, type)
                    }
                    method.visitInsn(Opcodes.RETURN)
                } else {
                    method.visitVarInsn(Opcodes.ALOAD, 1)
                    method.visitMethodInsn(Opcodes.INVOKESTATIC, HELPER, "asStringMap", "(Ljava/lang/Object;)Ljava/util/Map;", false)
                    method.visitInsn(Opcodes.ARETURN)
                }
                method.visitMaxs(2, 3)
                method.visitEnd()
                return null
            }

            override fun visitEnd() {
                check(patched) { "WebUI dependency changed: missing $owner#$target" }
                if (owner == WEB_COLORS) {
                    check(fields == COLOR_FIELDS) { "WebColors fields changed: $fields" }
                }
                super.visitEnd()
            }
        }

    companion object {
        const val WEB_COLORS = "com/dergoogler/mmrl/webui/model/WebColors"
        const val HELPER = "com/anatdx/yukisu/ui/webui/WebUiColorMaps"
        const val SNAPSHOT = "$HELPER\$Snapshot"
        val TARGETS = mapOf(
            WEB_COLORS to "<init>(Landroidx/compose/material3/ColorScheme;)V",
            "com/dergoogler/mmrl/platform/file/config/ConfigFile" to "asStringMap(Ljava/lang/Object;)Ljava/util/Map;",
        )
        val COLOR_FIELDS = linkedMapOf(
            "colorScheme" to "Landroidx/compose/material3/ColorScheme;",
            "filledTonalButtonColors" to "Landroidx/compose/material3/ButtonColors;",
            "cardColors" to "Landroidx/compose/material3/CardColors;",
            "colorSchemeMap" to "Ljava/util/Map;",
            "filledTonalButtonColorsMap" to "Ljava/util/Map;",
            "cardColorsMap" to "Ljava/util/Map;",
            "allCssColors" to "Ljava/lang/String;",
        )
    }
}

@CacheableTask
abstract class VerifyWebUiReflectionPatchTask : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val classJars: ListProperty<RegularFile>

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val classDirs: ListProperty<Directory>

    @get:OutputFile
    abstract val receipt: RegularFileProperty

    @TaskAction
    fun verify() {
        WebUiReflectionPatch.TARGETS.forEach { (owner, target) ->
            val entry = "$owner.class"
            val bytes = classDirs.get().firstNotNullOfOrNull { dir ->
                dir.asFile.resolve(entry).takeIf { it.isFile }?.readBytes()
            } ?: classJars.get().firstNotNullOfOrNull { jar ->
                ZipFile(jar.asFile).use { zip -> zip.getEntry(entry)?.let { zip.getInputStream(it).use { stream -> stream.readBytes() } } }
            } ?: error("Missing WebUI patch class: $owner")
            var callsHelper = false
            var callsReflection = false
            ClassReader(bytes).accept(object : ClassVisitor(Opcodes.ASM9) {
                override fun visitMethod(access: Int, name: String, descriptor: String, signature: String?, exceptions: Array<out String>?): MethodVisitor? {
                    if (name + descriptor != target) return null
                    return object : MethodVisitor(Opcodes.ASM9) {
                        override fun visitMethodInsn(opcode: Int, owner: String, name: String, descriptor: String, isInterface: Boolean) {
                            callsHelper = callsHelper || owner == WebUiReflectionPatch.HELPER
                            callsReflection = callsReflection || owner.startsWith("kotlin/reflect/") || owner == "kotlin/jvm/internal/Reflection"
                        }
                    }
                }
            }, ClassReader.SKIP_DEBUG or ClassReader.SKIP_FRAMES)
            check(callsHelper && !callsReflection) { "WebUI reflection patch was not applied to $owner#$target" }
        }
        receipt.get().asFile.apply {
            parentFile.mkdirs()
            writeText("WebUI color and configuration mappings use direct field access.\n")
        }
    }
}

@CacheableTask
abstract class CopyRenamedApkTask : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val inputApkFolder: DirectoryProperty

    @get:Internal
    abstract val builtArtifactsLoader: Property<BuiltArtifactsLoader>

    @get:OutputDirectory
    abstract val outputApkFolder: DirectoryProperty

    @get:Input
    abstract val outputFileName: Property<String>

    @TaskAction
    fun copyApk() {
        val builtArtifacts = builtArtifactsLoader.get().load(inputApkFolder.get())
            ?: error("Cannot load APK artifacts")
        val sourceApk = builtArtifacts.elements.singleOrNull()?.outputFile
            ?: error("Expected exactly one APK for ${builtArtifacts.variantName}")
        val destinationFolder = outputApkFolder.get().asFile
        check(destinationFolder.deleteRecursively()) { "Cannot clean $destinationFolder" }
        check(destinationFolder.mkdirs()) { "Cannot create $destinationFolder" }
        val destinationApk = File(destinationFolder, outputFileName.get())
        File(sourceApk).copyTo(destinationApk, overwrite = true)
    }
}

/**
 * Works around an upstream crash in compose-ui 1.12.x:
 *
 *     java.lang.IllegalArgumentException: LayoutNode <id> not found in RectList
 *
 * `RectManager.recalculateRectIfDirty` inserts a node relative to its parent, and every
 * parent-relative RectList operation dereferences the parent's slot to read its rect
 * (`RectList.insertBasedOnParentOffset` indexes `items[parentIndex]` directly). The three
 * `parent.indexInRectList()` call sites are the only ones in RectManager that are not guarded by
 * `inRectList()`, and `LayoutNode.onReuse()` — LazyLayout item recycling — evicts a node from the
 * RectList and only re-inserts it if it is already placed. A child laid out in that window hits
 * the unguarded parent lookup and trips its `requirePrecondition`.
 *
 * The patch returns early when the parent is not tracked. `rectInParentDirty` stays true, so the
 * node is recorded on the next placement pass once the parent is back in the list.
 *
 * Present in ui 1.12.0-alpha01 through 1.13.0-alpha01. Drop this once the guard is upstream.
 */
abstract class RectListParentGuard : AsmClassVisitorFactory<InstrumentationParameters.None> {

    override fun isInstrumentable(classData: ClassData): Boolean =
        classData.className == RECT_MANAGER_NAME

    override fun createClassVisitor(
        classContext: ClassContext,
        nextClassVisitor: ClassVisitor
    ): ClassVisitor = RectManagerVisitor(nextClassVisitor)

    private class RectManagerVisitor(next: ClassVisitor) : ClassVisitor(Opcodes.ASM9, next) {

        private var patched = false

        override fun visitMethod(
            access: Int,
            name: String,
            descriptor: String,
            signature: String?,
            exceptions: Array<out String>?
        ): MethodVisitor {
            val delegate = super.visitMethod(access, name, descriptor, signature, exceptions)
            if (name != TARGET_METHOD || descriptor != TARGET_DESC) return delegate
            patched = true
            return object : MethodVisitor(Opcodes.ASM9, delegate) {
                override fun visitCode() {
                    super.visitCode()
                    val body = Label()
                    visitVarInsn(Opcodes.ALOAD, 0)
                    visitVarInsn(Opcodes.ALOAD, 1)
                    visitMethodInsn(
                        Opcodes.INVOKESTATIC, RECT_MANAGER, GUARD_METHOD, GUARD_DESC, false
                    )
                    visitJumpInsn(Opcodes.IFEQ, body)
                    visitInsn(Opcodes.RETURN)
                    visitLabel(body)
                }
            }
        }

        override fun visitEnd() {
            check(patched) {
                "RectListParentGuard: $RECT_MANAGER_NAME has no $TARGET_METHOD$TARGET_DESC. " +
                    "compose-ui internals changed; re-check the RectList parent-lookup patch."
            }
            emitGuardMethod()
            super.visitEnd()
        }

        /**
         * `parent == null` yields false; otherwise true when the parent's cached slot is unset or
         * no longer holds the parent's id.
         */
        private fun emitGuardMethod() {
            val mv = super.visitMethod(
                Opcodes.ACC_PRIVATE or Opcodes.ACC_STATIC or Opcodes.ACC_SYNTHETIC,
                GUARD_METHOD,
                GUARD_DESC,
                null,
                null
            )
            val tracked = Label()
            val untracked = Label()
            mv.visitCode()

            // LayoutNode parent = node.getParent$ui()
            mv.visitVarInsn(Opcodes.ALOAD, 1)
            mv.visitMethodInsn(
                Opcodes.INVOKEVIRTUAL, LAYOUT_NODE, "getParent\$ui", "()$LAYOUT_NODE_DESC", false
            )
            mv.visitVarInsn(Opcodes.ASTORE, 2)
            mv.visitVarInsn(Opcodes.ALOAD, 2)
            mv.visitJumpInsn(Opcodes.IFNULL, tracked)

            // int cached = parent.getRectListIndex$ui()
            mv.visitVarInsn(Opcodes.ALOAD, 2)
            mv.visitMethodInsn(
                Opcodes.INVOKEVIRTUAL, LAYOUT_NODE, "getRectListIndex\$ui", "()I", false
            )
            mv.visitVarInsn(Opcodes.ISTORE, 3)
            mv.visitVarInsn(Opcodes.ILOAD, 3)
            mv.visitIntInsn(Opcodes.BIPUSH, NOT_FOUND)
            mv.visitJumpInsn(Opcodes.IF_ICMPEQ, untracked)

            // manager.getRects().indexOf(parent.getSemanticsId(), cached)
            mv.visitVarInsn(Opcodes.ALOAD, 0)
            mv.visitMethodInsn(
                Opcodes.INVOKEVIRTUAL, RECT_MANAGER, "getRects", "()$RECT_LIST_DESC", false
            )
            mv.visitVarInsn(Opcodes.ALOAD, 2)
            mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, LAYOUT_NODE, "getSemanticsId", "()I", false)
            mv.visitVarInsn(Opcodes.ILOAD, 3)
            mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, RECT_LIST, "indexOf", "(II)I", false)
            mv.visitIntInsn(Opcodes.BIPUSH, NOT_FOUND)
            mv.visitJumpInsn(Opcodes.IF_ICMPEQ, untracked)

            mv.visitLabel(tracked)
            mv.visitInsn(Opcodes.ICONST_0)
            mv.visitInsn(Opcodes.IRETURN)

            mv.visitLabel(untracked)
            mv.visitInsn(Opcodes.ICONST_1)
            mv.visitInsn(Opcodes.IRETURN)

            mv.visitMaxs(3, 4)
            mv.visitEnd()
        }
    }

    companion object {
        private const val RECT_MANAGER_NAME = "androidx.compose.ui.spatial.RectManager"
        private const val RECT_MANAGER = "androidx/compose/ui/spatial/RectManager"
        private const val RECT_LIST = "androidx/compose/ui/spatial/RectList"
        private const val LAYOUT_NODE = "androidx/compose/ui/node/LayoutNode"

        private const val LAYOUT_NODE_DESC = "L$LAYOUT_NODE;"
        private const val RECT_LIST_DESC = "L$RECT_LIST;"

        private const val TARGET_METHOD = "recalculateRectIfDirty"
        private const val TARGET_DESC = "($LAYOUT_NODE_DESC)V"

        private const val GUARD_METHOD = "yukisuIsParentUntracked"
        private const val GUARD_DESC = "(L$RECT_MANAGER;$LAYOUT_NODE_DESC)Z"

        /** `RectList.NotFound`, i.e. `-(LongsPerItem + 1)`. */
        private const val NOT_FOUND = -4
    }
}

/**
 * Fails the build when the shape [RectListParentGuard] patches no longer matches what compose-ui
 * ships, or when the patch did not make it into the merged classes. Checked against the
 * post-instrumentation class set, so a compose bump that renames, moves, or reshapes any of these
 * symbols breaks loudly instead of silently dropping the workaround.
 */
@CacheableTask
abstract class VerifyRectListPatchTask : DefaultTask() {

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val classJars: ListProperty<RegularFile>

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val classDirs: ListProperty<Directory>

    @get:OutputFile
    abstract val receipt: RegularFileProperty

    @TaskAction
    fun verify() {
        val missing = mutableListOf<String>()
        val found = mutableListOf<String>()

        REQUIRED_MEMBERS.forEach { (internalName, members) ->
            val bytes = readClass(internalName)
            if (bytes == null) {
                missing += "$internalName (class not found)"
                return@forEach
            }
            val actual = methodsOf(bytes)
            members.forEach { member ->
                if (member in actual) found += "$internalName#$member"
                else missing += "$internalName#$member"
            }
        }

        // RectList.NotFound is `-(LongsPerItem + 1)`; the injected guard compares against it as a
        // literal, so a change to either constant has to fail here rather than silently disarm
        // the guard. indexOf(I)I returns NotFound on a miss.
        val rectList = readClass(RECT_LIST)
        if (rectList != null) {
            val constants = intConstantsOf(rectList, "indexOf", "(I)I")
            if (NOT_FOUND !in constants) {
                missing += "$RECT_LIST#indexOf(I)I no longer returns $NOT_FOUND " +
                    "(saw $constants); RectList.NotFound changed"
            } else {
                found += "$RECT_LIST#indexOf(I)I returns $NOT_FOUND"
            }
        }

        if (missing.isNotEmpty()) {
            throw GradleException(
                buildString {
                    appendLine("compose-ui RectList parent-lookup patch is stale.")
                    appendLine()
                    appendLine("Missing or changed:")
                    missing.forEach { appendLine("  - $it") }
                    appendLine()
                    appendLine(
                        "This usually means compose-ui was bumped. Re-read " +
                            "androidx/compose/ui/spatial/RectManager.kt: if the unguarded " +
                            "parent.indexInRectList() calls are gone, delete " +
                            "RectListParentGuard and this task; otherwise update them to match."
                    )
                }
            )
        }

        receipt.get().asFile.apply {
            parentFile.mkdirs()
            writeText(found.joinToString(separator = "\n", postfix = "\n"))
        }
    }

    private fun readClass(internalName: String): ByteArray? {
        val entry = "$internalName.class"
        classDirs.get().forEach { dir ->
            val candidate = dir.asFile.resolve(entry)
            if (candidate.isFile) return candidate.readBytes()
        }
        classJars.get().forEach { jar ->
            ZipFile(jar.asFile).use { zip ->
                val found = zip.getEntry(entry) ?: return@use
                return zip.getInputStream(found).use { it.readBytes() }
            }
        }
        return null
    }

    private fun methodsOf(bytes: ByteArray): Set<String> {
        val methods = mutableSetOf<String>()
        ClassReader(bytes).accept(
            object : ClassVisitor(Opcodes.ASM9) {
                override fun visitMethod(
                    access: Int,
                    name: String,
                    descriptor: String,
                    signature: String?,
                    exceptions: Array<out String>?
                ): MethodVisitor? {
                    methods += name + descriptor
                    return null
                }
            },
            ClassReader.SKIP_CODE or ClassReader.SKIP_DEBUG or ClassReader.SKIP_FRAMES
        )
        return methods
    }

    private fun intConstantsOf(bytes: ByteArray, name: String, descriptor: String): Set<Int> {
        val constants = mutableSetOf<Int>()
        ClassReader(bytes).accept(
            object : ClassVisitor(Opcodes.ASM9) {
                override fun visitMethod(
                    access: Int,
                    methodName: String,
                    methodDescriptor: String,
                    signature: String?,
                    exceptions: Array<out String>?
                ): MethodVisitor? {
                    if (methodName != name || methodDescriptor != descriptor) return null
                    return object : MethodVisitor(Opcodes.ASM9) {
                        override fun visitInsn(opcode: Int) {
                            if (opcode == Opcodes.ICONST_M1) constants += -1
                        }

                        override fun visitIntInsn(opcode: Int, operand: Int) {
                            if (opcode == Opcodes.BIPUSH || opcode == Opcodes.SIPUSH) {
                                constants += operand
                            }
                        }

                        override fun visitLdcInsn(value: Any?) {
                            if (value is Int) constants += value
                        }
                    }
                }
            },
            ClassReader.SKIP_DEBUG or ClassReader.SKIP_FRAMES
        )
        return constants
    }

    companion object {
        private const val RECT_MANAGER = "androidx/compose/ui/spatial/RectManager"
        private const val RECT_LIST = "androidx/compose/ui/spatial/RectList"
        private const val LAYOUT_NODE = "androidx/compose/ui/node/LayoutNode"

        private const val NOT_FOUND = -4

        private val REQUIRED_MEMBERS = mapOf(
            RECT_MANAGER to listOf(
                // patch site
                "recalculateRectIfDirty(L$LAYOUT_NODE;)V",
                // read by the guard
                "getRects()L$RECT_LIST;",
                // proves the instrumentation actually ran on this build
                "yukisuIsParentUntracked(L$RECT_MANAGER;L$LAYOUT_NODE;)Z",
            ),
            RECT_LIST to listOf(
                "indexOf(II)I",
                "indexOf(I)I",
            ),
            LAYOUT_NODE to listOf(
                "getParent\$ui()L$LAYOUT_NODE;",
                "getRectListIndex\$ui()I",
                "getSemanticsId()I",
            ),
        )
    }
}

val managerVersionCode = rootProject.extra["managerVersionCode"] as Int
val managerVersionName = rootProject.extra["managerVersionName"] as String
val ksudBundledVersion = rootProject.extra["ksudBundledVersion"] as String
val androidCmakeVersion = rootProject.extra["androidCmakeVersion"] as String
val ciRunId = System.getenv("GITHUB_RUN_ID")?.toLongOrNull() ?: 0L

fun signingValue(propertyName: String, environmentName: String): String? {
    return providers.gradleProperty(propertyName)
        .orElse(providers.environmentVariable(environmentName))
        .orNull
        ?.takeIf(String::isNotBlank)
}

/**
 * The locales res/ actually translates, for `androidResources.localeFilters`.
 *
 * Without a filter every library we depend on contributes its own translations
 * -- AppCompat alone carries around forty more languages than we do -- so a
 * device set to one of them shows an English app dotted with localized widget
 * labels, and resources.arsc grows by roughly 380 KB that nobody reads.
 *
 * Derived from the directory names so adding a translation stays a one-step
 * change. Only the values-xx and values-xx-rYY forms are recognised, which is
 * how translations actually arrive; a locale contributed as values-b+xx+Yyyy
 * would need this widened, or it would silently not ship.
 */
fun appLocaleFilters(): List<String> {
    val localeQualifier = Regex("""[a-z]{2,3}(-r[A-Z]{2})?""")
    return file("src/main/res").listFiles().orEmpty()
        .filter { it.isDirectory && it.name.startsWith("values-") }
        .map { it.name.substringAfter("values-") }
        .filter(localeQualifier::matches)
        .plus("en")
        .sorted()
}

val signingStoreFile = signingValue("KEYSTORE_FILE", "YUKISU_KEYSTORE")
val signingStorePassword = signingValue("KEYSTORE_PASSWORD", "YUKISU_KEYSTORE_PASSWORD")
val signingKeyAlias = signingValue("KEY_ALIAS", "YUKISU_KEY_ALIAS")
val signingKeyPassword = signingValue("KEY_PASSWORD", "YUKISU_KEY_PASSWORD")
val signingValues = listOf(
    signingStoreFile,
    signingStorePassword,
    signingKeyAlias,
    signingKeyPassword,
)
val hasReleaseSigning = signingValues.all { it != null }
check(signingValues.all { it == null } || hasReleaseSigning) {
    "Release signing requires KEYSTORE_FILE, KEYSTORE_PASSWORD, KEY_ALIAS, and KEY_PASSWORD"
}

android {
    signingConfigs {
        if (hasReleaseSigning) {
            create("release") {
                storeFile = rootProject.file(requireNotNull(signingStoreFile))
                storePassword = requireNotNull(signingStorePassword)
                keyAlias = requireNotNull(signingKeyAlias)
                keyPassword = requireNotNull(signingKeyPassword)
            }
        }
    }
    namespace = "com.anatdx.yukisu"

    defaultConfig {
        testInstrumentationRunner = "ui.screen.ramdisk.RamdiskFileActionsTestRunner"
        buildConfigField("String", "KSUD_BUNDLED_VERSION", "\"$ksudBundledVersion\"")
        buildConfigField("long", "CI_RUN_ID", "${ciRunId}L")
        manifestPlaceholders["ciRunId"] = "run-$ciRunId"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            vcsInfo.include = false
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    buildFeatures {
        aidl = true
        buildConfig = true
        compose = true
        prefab = true
    }

    packaging {
        dex {
            // AGP stores classes.dex uncompressed once minSdk >= 28 so ART can
            // mmap it straight from the APK and the vdex need not hold a second
            // copy. That is the better deal for a Play-delivered app, but YukiSU
            // ships APKs people re-download from CI, and leaving it on cost ~2.9
            // MB of download on every one of them. Compressing trades roughly
            // that much back for ~3 MB of resident storage per device -- the
            // dex ends up materialized in the vdex instead.
            useLegacyPackaging = true
        }
        jniLibs {
            useLegacyPackaging = true
            excludes += "lib/*/libandroidx.graphics.path.so"
        }
        resources {
            // https://stackoverflow.com/a/58956288
            // It will break Layout Inspector, but it's unused for release build.
            excludes += "META-INF/*.version"
            pickFirsts += "META-INF/LICENSE.md"
            // https://github.com/Kotlin/kotlinx.coroutines?tab=readme-ov-file#avoiding-including-the-debug-infrastructure-in-the-resulting-apk
            excludes += "DebugProbesKt.bin"
            // https://issueantenna.com/repo/kotlin/kotlinx.coroutines/issues/3158
            excludes += "kotlin-tooling-metadata.json"
            // Bouncy Castle bundles Picnic parameter tables and X.509 reviewer
            // messages as Java resources. CI update verification only uses
            // OpenPGP Ed25519 signatures, so none of these resources are read.
            excludes += "org/bouncycastle/pqc/legacy/picnic/lowmcL1.bin.properties"
            excludes += "org/bouncycastle/pqc/legacy/picnic/lowmcL3.bin.properties"
            excludes += "org/bouncycastle/pqc/legacy/picnic/lowmcL5.bin.properties"
            excludes += "org/bouncycastle/x509/CertPathReviewerMessages.properties"
            excludes += "org/bouncycastle/x509/CertPathReviewerMessages_de.properties"
        }
    }

    lint {
        warningsAsErrors = true
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true
        unitTests.all {
            it.jvmArgs(
                "--add-opens=java.base/java.lang=ALL-UNNAMED",
                "--add-opens=java.base/java.util=ALL-UNNAMED",
                "--add-opens=java.base/java.io=ALL-UNNAMED",
                "--add-opens=java.base/java.net=ALL-UNNAMED",
                "--add-opens=java.base/java.security=ALL-UNNAMED",
                "--add-opens=java.base/java.text=ALL-UNNAMED",
                "--add-opens=java.base/jdk.internal.access=ALL-UNNAMED",
                "--add-opens=java.desktop/java.awt.font=ALL-UNNAMED",
                "--add-opens=jdk.compiler/com.sun.tools.javac.api=ALL-UNNAMED",
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = androidCmakeVersion
        }
    }

    // https://stackoverflow.com/a/77745844
    tasks.withType<PackageAndroidArtifact> {
        doFirst { appMetadata.asFile.orNull?.writeText("") }
    }

    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }

    androidResources {
        localeFilters += appLocaleFilters()
        generateLocaleConfig = true
    }

    bundle {
        language {
            enableSplit = false
        }
    }
}

androidComponents {
    onVariants { variant ->
        val runtime = configurations.getByName("${variant.name}RuntimeClasspath").incoming.artifacts
        val inputsFile = rootProject.layout.projectDirectory.file("licenses/native-inputs.json")
        @Suppress("UNCHECKED_CAST")
        val inputs = JsonSlurper().parseText(providers.fileContents(inputsFile).asText.get()) as Map<String, List<String>>
        val generate = tasks.register<GenerateOpenSourceLicenses>(
            "generate${variant.name.replaceFirstChar(Char::uppercaseChar)}OpenSourceLicenses"
        ) {
            catalog.set(rootProject.layout.projectDirectory.file("licenses/components.json"))
            licenseTexts.set(rootProject.layout.projectDirectory.dir("licenses/texts"))
            nativeInputs.set(inputsFile)
            repositoryRoot.set(rootProject.layout.projectDirectory.dir(".."))
            nativeSources.from(inputs.getValue("files").map { rootProject.file("../$it") })
            inputs.getValue("submodules").forEach { path ->
                val submoduleDir = rootProject.file("../$path")
                nativeRevisions.put(path, providers.exec {
                    workingDir(submoduleDir)
                    commandLine("git", "rev-parse", "--show-toplevel", "HEAD")
                }.standardOutput.asText.map { output ->
                    val lines = output.trim().lines()
                    check(lines.size == 2 && File(lines.first()).canonicalFile == submoduleDir.canonicalFile) {
                        "Uninitialized native submodule: $path. Run git submodule update --init --recursive."
                    }
                    lines.last()
                })
            }
            artifacts.from(runtime.artifactFiles)
            coordinates.set(runtime.resolvedArtifacts.map { resolved ->
                resolved.groupBy { artifact ->
                    val id = artifact.id.componentIdentifier as? ModuleComponentIdentifier
                        ?: error("Audit local runtime dependency: ${artifact.id}")
                    "${id.group}:${id.module}:${id.version}"
                }.mapValues { (_, artifacts) -> artifacts.map { it.file.name }.distinct().sorted() }
            })
            buildType.set(variant.buildType ?: variant.name)
            outputDirectory.set(layout.buildDirectory.dir("generated/openSourceLicenses/${variant.name}"))
        }
        variant.sources.assets?.addGeneratedSourceDirectory(generate, GenerateOpenSourceLicenses::outputDirectory)
        if (variant.buildType == "release") {
            // Eruda is the WebUI debug console that mmrl-webui carries in its
            // assets. The toggle that injects it lives behind BuildConfig.DEBUG,
            // so nothing in a release build can reach it -- and it is 251,732
            // bytes of the APK.
            variant.androidResources.ignoreAssetsPatterns.add("eruda")
        }

        variant.instrumentation.setAsmFramesComputationMode(
            FramesComputationMode.COMPUTE_FRAMES_FOR_INSTRUMENTED_CLASSES
        )
        variant.instrumentation.transformClassesWith(
            RectListParentGuard::class.java,
            InstrumentationScope.ALL,
        ) {}
        variant.instrumentation.transformClassesWith(
            WebUiReflectionPatch::class.java,
            InstrumentationScope.ALL,
        ) {}

        val capitalizedVariant = variant.name.replaceFirstChar(Char::uppercaseChar)
        val verifyWebUiPatchTask = tasks.register<VerifyWebUiReflectionPatchTask>(
            "verifyWebUiReflectionPatch$capitalizedVariant"
        ) {
            receipt.set(layout.buildDirectory.file("reports/webUiReflectionPatch/${variant.name}.txt"))
        }
        variant.artifacts.forScope(ScopedArtifacts.Scope.ALL)
            .use(verifyWebUiPatchTask)
            .toGet(
                ScopedArtifact.CLASSES,
                VerifyWebUiReflectionPatchTask::classJars,
                VerifyWebUiReflectionPatchTask::classDirs,
            )
        val verifyPatchTask = tasks.register<VerifyRectListPatchTask>(
            "verifyRectListPatch$capitalizedVariant"
        ) {
            receipt.set(
                layout.buildDirectory.file("reports/rectListPatch/${variant.name}.txt")
            )
        }
        variant.artifacts.forScope(ScopedArtifacts.Scope.ALL)
            .use(verifyPatchTask)
            .toGet(
                ScopedArtifact.CLASSES,
                VerifyRectListPatchTask::classJars,
                VerifyRectListPatchTask::classDirs,
            )
        // assemble/bundle tasks do not exist yet while variants are being configured.
        val packagingTasks = setOf("assemble$capitalizedVariant", "bundle$capitalizedVariant")
        tasks.matching { it.name in packagingTasks }.configureEach {
            dependsOn(verifyPatchTask)
            dependsOn(verifyWebUiPatchTask)
        }

        val outputName =
            "YukiSU_${managerVersionName}_${managerVersionCode}-arm64-v8a-${variant.name}.apk"
        val copyTask = tasks.register<CopyRenamedApkTask>(
            "copyRenamed${variant.name.replaceFirstChar(Char::uppercaseChar)}Apk"
        ) {
            builtArtifactsLoader.set(variant.artifacts.getBuiltArtifactsLoader())
            outputApkFolder.set(
                layout.buildDirectory.dir("outputs/renamed_apk/${variant.name}")
            )
            outputFileName.set(outputName)
        }
        variant.artifacts.use(copyTask)
            .wiredWith(CopyRenamedApkTask::inputApkFolder)
            .toListenTo(SingleArtifact.APK)
    }
}

ksp {
    arg("compose-destinations.defaultTransitions", "none")
}

composeCompiler {
    // MMRL currently contains a method shape the optional mapping tokenizer cannot parse.
    includeComposeMappingFile = false
}

dependencies {
    implementation(libs.bouncycastle.bcpg)
    implementation(libs.gson)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.exifinterface)
    implementation(libs.androidx.transition)
    implementation(libs.okhttp)
    implementation(libs.commons.compress)
    implementation(libs.xz)

    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)

    implementation(libs.compose.destinations.core)
    ksp(libs.compose.destinations.ksp)

    implementation(libs.com.github.topjohnwu.libsu.core)
    implementation(libs.com.github.topjohnwu.libsu.service)
    implementation(libs.com.github.topjohnwu.libsu.io)

    implementation(libs.dev.rikka.rikkax.parcelablelist)

    implementation(libs.io.coil.kt.coil.compose)

    implementation(libs.kotlinx.coroutines.core)

    implementation(libs.me.zhanghai.android.appiconloader.coil)

    implementation(libs.sheet.compose.dialogs.core)
    implementation(libs.sheet.compose.dialogs.list)
    implementation(libs.sheet.compose.dialogs.input)

    implementation(libs.markdown)
    implementation(libs.androidx.webkit)

    implementation(libs.lsposed.cxx)

    implementation(libs.com.github.topjohnwu.libsu.core)

    // WebUiReflectionPatch removes the live reflection calls.
    implementation(libs.mmrl.platform) {
        exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
    }
    compileOnly(libs.mmrl.hidden.api)
    implementation(libs.mmrl.webui) {
        exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
    }
    implementation(libs.mmrl.hwui)
    implementation(libs.mmrl.ext) {
        exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
    }
    implementation(libs.mmrl.compat) {
        exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
    }
    implementation(libs.androidx.swiperefreshlayout)
    implementation(libs.mmrl.ui) {
        exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
    }

    implementation(libs.accompanist.drawablepainter)
    implementation(libs.ucrop)
    implementation(libs.yukifb)
    implementation(libs.yukifb.hex)

    testImplementation(libs.kotlin.test)
    implementation(libs.kotlinx.serialization.json)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.junit)
}
