package org.rouyashiki.yukisu

import android.os.Parcelable
import androidx.annotation.Keep
import androidx.compose.runtime.Immutable
import kotlinx.parcelize.Parcelize
import org.rouyashiki.yukisu.ui.util.rootAvailable
import org.rouyashiki.yukisu.ui.util.KasumiConflict

/**
 * @author weishu
 * @date 2022/12/8.
 */
object Natives {
    const val KERNEL_SU_DOMAIN = "u:r:ksu:s0"

    const val MINIMAL_NEW_IOCTL_KERNEL = 10000

    const val ROOT_UID = 0
    const val ROOT_GID = 0

    /** root_profile.flags: block re-escalation for this profile and its children. */
    const val FLAG_KSU_NO_NEW_PRIVS = 1L

    external fun getFullVersion(): String

    external fun kagamiRequest(ksudPath: ByteArray, request: ByteArray): ByteArray
    external fun kasumiKernelSnapshot(): ByteArray
    external fun kasumiRetryUserHide(path: ByteArray): ByteArray
    external fun kasumiIsInitialized(): Boolean
    external fun kasumiRuntimeState(): Int
    external fun getKasumiConflicts(): Array<KasumiConflict>?
    external fun ackKasumiConflicts(sequence: Long): Boolean
    external fun kasumiClearMapsRules()
    external fun kasumiAddMapsRule(numbers: LongArray, path: ByteArray)
    external fun kasumiReadLog(kernel: Boolean): ByteArray
    external fun kasumiClearLog()
    external fun kasumiStorageInfo(path: ByteArray): ByteArray
    external fun getSuPath(): ByteArray?
    external fun saveSuPath(ksudPath: ByteArray, path: ByteArray): ByteArray

    /** Kernel UAPI contract version (KERNEL_SU_UAPI_VERSION); 0 if unsupported. */
    external fun getUapiVersion(): Int

    /** UAPI contract version this manager binary was built against. */
    external fun getManagerUapiVersion(): Int

    /** True when the kernel's UAPI version differs from the manager's (skew). */
    fun checkUapiMismatch(): Boolean = getUapiVersion() != getManagerUapiVersion()

    init {
        System.loadLibrary("kernelsu")
    }

    val version: Int
        external get

    // get the uid list of allowed su processes.
    val allowList: IntArray
        external get

    /** Returns total number of apps in allow list (count only, no full list fetch). */
    external fun getSuperuserCount(): Int

    val isSafeMode: Boolean
        external get

    val isLateLoadMode: Boolean
        external get

    val isLkmBundled: Boolean
        external get

    val isImagePatchMode: Boolean
        external get

    /** Returns 0 when unavailable, otherwise one of the KSU_LOAD_MODE_* values. */
    external fun getLoadMode(): Int

    const val LOAD_MODE_UNKNOWN = 0
    const val LOAD_MODE_RAMDISK = 1
    const val LOAD_MODE_IMAGE_PATCH = 2
    const val LOAD_MODE_LATE = 3

    val isManager: Boolean
        external get

    const val KSUD_INTEGRITY_UNAVAILABLE = 0
    const val KSUD_INTEGRITY_MATCH = 1
    const val KSUD_INTEGRITY_MISMATCH = 2

    external fun verifyKsudDaemon(sourcePath: String): Int
    external fun installKsudDaemon(sourcePath: String): Boolean
    external fun ensureBusybox(sourcePath: String): Boolean
    external fun ensureKsudToolLinks(): Boolean

    external fun uidShouldUmount(uid: Int): Boolean
    external fun getDynamicManagers(): IntArray

    const val DYNAMIC_MANAGER_FLAG_PRESET = 1 shl 0
    const val DYNAMIC_MANAGER_FLAG_TRUSTED = 1 shl 1

    /**
     * Get the profile of the given package.
     * @param key usually the package name
     * @return return null if failed.
     */
    external fun getAppProfile(key: String?, uid: Int): Profile
    external fun setAppProfile(profile: Profile?): Boolean
    external fun getProfileUids(allow: Boolean): IntArray
    external fun readAppProfile(uid: Int): Profile?
    external fun getProtectedProfileAppIds(): IntArray

    /**
     * `su` compat mode can be disabled temporarily.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSuEnabled(): Boolean
    external fun setSuEnabled(enabled: Boolean): Boolean

    external fun isMagiskCompatEnabled(): Boolean
    external fun setMagiskCompatEnabled(enabled: Boolean): Boolean
    external fun submitSuPrompt(
        requestId: Long,
        nonce: Long,
        choice: Int,
        packageName: String,
    ): Boolean
    external fun suPromptReady(requestId: Long, nonce: Long): Int

    /**
     * Kernel module umount can be disabled temporarily.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isKernelUmountEnabled(): Boolean
    external fun setKernelUmountEnabled(enabled: Boolean): Boolean

    /**
     * Su Log can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSuLogEnabled(): Boolean
    external fun setSuLogEnabled(enabled: Boolean): Boolean

    /**
     * ADB Root can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isAdbRootEnabled(): Boolean
    external fun setAdbRootEnabled(enabled: Boolean): Boolean

    /**
     * SELinux Hide can be enabled/disabled.
     *  0: disabled
     *  1: enabled
     *  negative : error
     */
    external fun isSelinuxHideEnabled(): Boolean
    external fun setSelinuxHideEnabled(enabled: Boolean): Boolean

    /**
     * Global "app profile 防逃逸" default. When on, the default root profile
     * (used by every profile that resolves to default, incl. the manager and
     * shell) carries NO_NEW_PRIVS. Persisted via ksud's feature config.
     */
    external fun isDefaultNoNewPrivsEnabled(): Boolean
    external fun setDefaultNoNewPrivsEnabled(enabled: Boolean): Boolean

    /** Runtime bootloader-property hiding. Persisted via ksud's feature config. */
    external fun isHideBootloaderEnabled(): Boolean
    external fun setHideBootloaderEnabled(enabled: Boolean): Boolean

    external fun getHookType(): String

    /**
     * Feature ids, mirroring `enum ksu_feature_id` in uapi/feature.h. Ids
     * 0-99 are upstream KernelSU's; YukiSU extensions start at 100.
     */
    const val FEATURE_SU_COMPAT = 0
    const val FEATURE_KERNEL_UMOUNT = 1
    const val FEATURE_SULOG = 2
    const val FEATURE_ADB_ROOT = 3
    const val FEATURE_SELINUX_HIDE = 4
    const val FEATURE_SHELL_SU_ADB_ONLY = 6
    const val FEATURE_MAGISK_COMPAT = 101
    const val FEATURE_DEFAULT_NO_NEW_PRIVS = 102
    const val FEATURE_YUKIZYGISK = 103
    const val FEATURE_HIDE_BOOTLOADER = 104
    const val FEATURE_KASUMI_SUCOMPAT = 105
    const val FEATURE_KASUMI = 106
    const val FEATURE_UNSHARE_MNT = 107

    /**
     * Reads a feature's value straight from the kernel, or -1 when the kernel
     * does not support it. Prefer [isFeatureSupported] / [isFeatureEnabled];
     * this is the raw form for features whose value is not just a flag.
     */
    external fun getFeature(id: Int): Long

    external fun getFeatureFlags(id: Int): Int

    /** Whether the running kernel knows about this feature at all. */
    fun isFeatureSupported(id: Int): Boolean = getFeature(id) >= 0

    /** Whether the feature is both supported and turned on. */
    fun isFeatureEnabled(id: Int): Boolean = getFeature(id) > 0

    external fun getUserName(uid: Int): String?

    /**
     * SuperKey authentication
     * Authenticates the manager with the given SuperKey
     * @param superKey the secret key to authenticate
     * @return true if authentication successful, false otherwise
     */
    external fun authenticateSuperKey(superKey: String): Boolean
    
    /**
     * Check if KSU driver is present (without authentication)
     * @return true if driver fd can be found, false otherwise
     */
    external fun isKsuDriverPresent(): Boolean
    
    /**
     * Check if SuperKey is configured in kernel
     * @return true if SuperKey is configured, false otherwise
     */
    external fun isSuperKeyConfigured(): Boolean
    
    /**
     * Check if already authenticated via SuperKey
     * @return true if authenticated via SuperKey, false otherwise
     */
    external fun isSuperKeyAuthenticated(): Boolean
    
    /**
     * Check if manager signature is considered OK by kernel.
     * This reflects whether signature-based verification is in effect.
     */
    external fun isSignatureOk(): Boolean

    private const val NON_ROOT_DEFAULT_PROFILE_KEY = "$"
    private const val NOBODY_UID = 9999

    fun setDefaultUmountModules(umountModules: Boolean): Boolean {
        Profile(
            NON_ROOT_DEFAULT_PROFILE_KEY,
            NOBODY_UID,
            false,
            nonRootUseDefault = false,
            umountModules = umountModules
        ).let {
            return setAppProfile(it)
        }
    }

    fun isDefaultUmountModules(): Boolean {
        getAppProfile(NON_ROOT_DEFAULT_PROFILE_KEY, NOBODY_UID).let {
            return it.umountModules
        }
    }

    fun isFullFeatured(): Boolean = isManager && !checkUapiMismatch() && rootAvailable()

    @Immutable
    @Parcelize
    @Keep
    data class Profile(
        // and there is a default profile for root and non-root
        val name: String,
        // current uid for the package, this is convivent for kernel to check
        // if the package name doesn't match uid, then it should be invalidated.
        val currentUid: Int = 0,

        // if this is true, kernel will grant root permission to this package
        val allowSu: Boolean = false,

        // these are used for root profile
        val rootUseDefault: Boolean = true,
        val rootTemplate: String? = null,
        val uid: Int = ROOT_UID,
        val gid: Int = ROOT_GID,
        val groups: List<Int> = mutableListOf(),
        val capabilities: List<Int> = mutableListOf(),
        val capabilitiesPermitted: List<Int> = mutableListOf(),
        val capabilitiesInheritable: List<Int> = mutableListOf(),
        val context: String = KERNEL_SU_DOMAIN,
        val namespace: Int = Namespace.INHERITED.ordinal,
        // root_profile.flags bitmask. Neutral by default; the per-profile UI
        // seeds the anti-escape toggle from the global default
        // (isDefaultNoNewPrivsEnabled) when the profile still uses default.
        val flags: Long = 0L,

        val nonRootUseDefault: Boolean = true,
        // Default to NOT unmounting modules for non-root apps.
        // Apps without an explicit profile will keep module modifications applied.
        val umountModules: Boolean = false,
        var rules: String = "", // this field is save in ksud!!
    ) : Parcelable {
        enum class RootProfileFlag(val mask: Long) {
            NO_NEW_PRIVS(FLAG_KSU_NO_NEW_PRIVS),
        }

        enum class Namespace {
            INHERITED,
            GLOBAL,
            INDIVIDUAL,
        }

        constructor() : this("")
    }
}

fun List<Natives.Profile.RootProfileFlag>.toRawFlags(): Long =
    fold(0L) { flags, flag -> flags or flag.mask }

fun Long.toRootProfileFlags(): List<Natives.Profile.RootProfileFlag> =
    Natives.Profile.RootProfileFlag.entries.filter { this and it.mask != 0L }
