import com.android.build.api.dsl.ApplicationExtension
import com.android.build.gradle.api.AndroidBasePlugin
import java.nio.charset.StandardCharsets

plugins {
    alias(libs.plugins.agp.app) apply false
    alias(libs.plugins.agp.lib) apply false
    alias(libs.plugins.compose.compiler) apply false
    alias(libs.plugins.lsplugin.cmaker)
}

val arm64Abi = "arm64-v8a"
val requestedAbi = findProperty("ABI")?.toString()
require(requestedAbi == null || requestedAbi == arm64Abi) {
    "YukiSU supports only $arm64Abi; requested ABI: $requestedAbi"
}
val buildAbiList = provider { listOf(arm64Abi) }

cmaker {
    default {
        cppFlags.removeAll { it.startsWith("-std=c++") }
        cppFlags += "-std=c++17"
        arguments.addAll(
            arrayOf(
                "-DANDROID_STL=none",
            )
        )
        abiFilters(*buildAbiList.get().toTypedArray())
    }
    buildTypes {
        if (it.name == "release") {
            arguments += "-DDEBUG_SYMBOLS_PATH=${layout.buildDirectory.asFile.get().absolutePath}/symbols"
        }
    }
}

val androidMinSdkVersion = 31
val androidTargetSdkVersion = 37
val androidCompileSdkVersion = 37
val androidCompileSdkMinorVersion = 2
val androidBuildToolsVersion = "36.1.0"
val androidCompileNdkVersion = libs.versions.ndk.get()
val androidCmakeVersion = "3.22.1"
val androidSourceCompatibility = JavaVersion.VERSION_21
val androidTargetCompatibility = JavaVersion.VERSION_21
private val repositoryRoot = rootProject.projectDir.parentFile
private val dirtyVersionPattern = Regex(
    "(?:v?\\d+\\.\\d+\\.\\d+|[0-9a-fA-F]{8})(?:-[0-9a-fA-F]{8})?-nc(?:@YukiSU)?(?:[^0-9A-Za-z]|$)"
)
val kernelHasNc = hasKernelNc()
val ksudHasNc = kernelHasNc || gitHasChanges("userspace/ksud") || hasBundledKsudNc()
val managerHasNc = ksudHasNc || gitHasChanges("manager")
val managerVersionCode = 10000 - 3135 + getGitCommitCount()
val managerVersionName = computeManagerVersionName(managerHasNc)
val ksudBundledVersion = computeKsudBundledVersion(ksudHasNc)

extra.set("androidCompileNdkVersion", androidCompileNdkVersion)
extra.set("androidCmakeVersion", androidCmakeVersion)
extra.set("managerVersionCode", managerVersionCode)
extra.set("managerVersionName", managerVersionName)
extra.set("ksudBundledVersion", ksudBundledVersion)

fun getGitCommitCount(): Int {
    return providers.exec {
        commandLine("git", "rev-list", "--count", "HEAD")
    }.standardOutput.asText.get().trim().toInt()
}

fun gitHasChanges(vararg paths: String): Boolean {
    val output = providers.exec {
        commandLine(
            listOf(
                "git",
                "-C",
                repositoryRoot.absolutePath,
                "-c",
                "core.autocrlf=true",
                "status",
                "--porcelain",
                "--untracked-files=normal",
                "--",
            ) + paths.toList()
        )
    }.standardOutput.asText.get().trim()
    return output.isNotEmpty()
}

fun fileHasDirtyVersion(file: java.io.File): Boolean {
    if (!file.isFile) return false
    return dirtyVersionPattern.containsMatchIn(file.readBytes().toString(StandardCharsets.ISO_8859_1))
}

fun hasKernelNc(): Boolean {
    val kernelDirectories = listOf(
        repositoryRoot.resolve("userspace/ksud/assets"),
        repositoryRoot.resolve("out"),
    )
    val builtKernelNc = kernelDirectories.any { directory ->
        directory.listFiles { file ->
            file.isFile && file.name.endsWith("_kernelsu.ko")
        }.orEmpty().any(::fileHasDirtyVersion)
    }
    return builtKernelNc || gitHasChanges("kernel", "uapi")
}

fun hasBundledKsudNc(): Boolean {
    val jniDirectory = repositoryRoot.resolve("manager/app/src/main/jniLibs")
    return jniDirectory.walkTopDown().any { file ->
        file.isFile && file.name == "libksud.so" && fileHasDirtyVersion(file)
    }
}

fun appendDirtySuffix(version: String, dirty: Boolean): String {
    return if (dirty && !version.endsWith("-nc")) "$version-nc" else version
}

/** Manager version from latest tag: v1.4.0 or v1.4.0-8char_hash (git describe --tags). */
fun computeManagerVersionName(dirty: Boolean): String {
    val describe = providers.exec {
        commandLine("git", "describe", "--tags", "--always", "--abbrev=8")
    }.standardOutput.asText.get().trim()
    // "v1.4.0" or "v1.4.0-1-g56b0efb0" -> "v1.4.0-56b0efb0"
    val version = if (describe.contains("-g")) {
        describe.replace(Regex("-\\d+-g"), "-")
    } else {
        describe
    }
    return appendDirtySuffix(version, dirty)
}

/**
 * Mirror userspace/ksud/scripts/generate_version.py so the manager-bundled
 * ksud version is known at build time and we don't need to exec the daemon
 * at runtime just to find it out.
 */
fun computeKsudBundledVersion(dirty: Boolean): String {
    val describe = providers.exec {
        commandLine("git", "describe", "--tags", "--always", "--abbrev=8")
    }.standardOutput.asText.get().trim()
    val normalized = if (describe.contains("-g")) {
        describe.replace(Regex("-\\d+-g"), "-")
    } else {
        describe
    }
    return appendDirtySuffix(normalized.removePrefix("v"), dirty)
}

subprojects {
    plugins.withType(AndroidBasePlugin::class.java) {
        extensions.configure(ApplicationExtension::class.java) {
            compileSdk {
                version = release(androidCompileSdkVersion) {
                    minorApiLevel = androidCompileSdkMinorVersion
                }
            }
            ndkVersion = androidCompileNdkVersion
            buildToolsVersion = androidBuildToolsVersion

            defaultConfig {
                minSdk = androidMinSdkVersion
                targetSdk = androidTargetSdkVersion
                versionCode = managerVersionCode
                versionName = managerVersionName
                ndk {
                    abiFilters += buildAbiList.get()
                }
            }

            lint {
                abortOnError = true
                checkReleaseBuilds = false
            }

            compileOptions {
                sourceCompatibility = androidSourceCompatibility
                targetCompatibility = androidTargetCompatibility
            }
        }
    }
}
