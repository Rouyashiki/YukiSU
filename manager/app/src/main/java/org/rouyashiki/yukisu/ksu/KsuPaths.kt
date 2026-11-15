package org.rouyashiki.yukisu.ksu

/**
 * Canonical on-device paths owned by KernelSU userspace.
 *
 * Anything that needs to read or write the KSU data dir should reach for one
 * of these constants instead of pasting `/data/adb/...` strings around. Keeping
 * them in one place lets us rename the layout (or drive it from ksud) later
 * without combing through the manager.
 */
object KsuPaths {
    const val DATA_ADB = "/data/adb"
    const val KSU_ROOT = "$DATA_ADB/ksu"
    const val KSU_BIN_DIR = "$KSU_ROOT/bin"
    const val KSU_LOG_DIR = "$KSU_ROOT/log"
    const val KSUD_BIN = "$DATA_ADB/ksud"
    const val ALLOWLIST = "$KSU_ROOT/.allowlist"
    const val BUSYBOX = "$KSU_BIN_DIR/busybox"
    const val MODULES_DIR = "$DATA_ADB/modules"
}
