package org.rouyashiki.yukisu.ui.util

import org.rouyashiki.yukisu.Natives

internal fun Natives.Profile.withoutLegacySepolicyDiagnostic(profileKey: String): Natives.Profile {
    // Saved UI state can survive an APK upgrade and bypass the daemon getter.
    return if (rules.trim(' ', '\t', '\r', '\n') == "No sepolicy profile for $profileKey") {
        copy(rules = "")
    } else {
        this
    }
}

internal fun saveProfileSepolicy(
    previousRules: String,
    profile: Natives.Profile,
    writeRules: (String) -> Boolean,
): Boolean {
    if (!profile.allowSu || profile.rootUseDefault) return true
    // An empty read result must not implicitly erase rules that could not be read.
    if (profile.rules.isEmpty() && previousRules.isEmpty()) return true
    return writeRules(profile.rules)
}
