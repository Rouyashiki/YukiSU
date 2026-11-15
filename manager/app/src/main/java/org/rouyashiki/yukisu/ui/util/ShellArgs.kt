package org.rouyashiki.yukisu.ui.util

/**
 * Wraps a value so a POSIX shell sees it as exactly one literal argument.
 *
 * Single quotes suppress every expansion the shell would otherwise perform, so
 * the only character needing care is the quote itself: close, escape, reopen.
 * Double quotes are never a substitute -- they leave `$`, backtick and
 * backslash live, which is command substitution in whatever shell we handed the
 * string to, and ours run as root.
 *
 * Everything interpolated into a command string goes through here: paths,
 * package names, sepolicy rules, and the SuperKey the user typed.
 */
internal fun shellArg(value: String): String = "'${value.replace("'", "'\\''")}'"
