package com.anatdx.yukisu.ui.screen

import com.anatdx.yukisu.ui.motion.yukiExpansionSemantics
import com.anatdx.yukisu.ui.motion.YukiExpandableContent
import com.anatdx.yukisu.ui.motion.YukiExpandArrow
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.component.SuperDropdown
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.util.bootForceFeatureNames

@Composable
internal fun BootConfigControls(
    allowConfigOnly: Boolean,
    configOnly: Boolean,
    onConfigOnlyChange: (Boolean) -> Unit,
    noPreinit: Boolean,
    onNoPreinitChange: (Boolean) -> Unit,
    noCustomRc: Boolean,
    onNoCustomRcChange: (Boolean) -> Unit,
    forcedFeatures: List<String>,
    onForcedFeaturesChange: (List<String>) -> Unit,
) {
    YukiExpandableContent(visible = allowConfigOnly) {
        BootConfigSwitch(
            stringResource(R.string.install_config_only),
            stringResource(R.string.install_config_only_summary),
            configOnly, onConfigOnlyChange,
        )
    }
    BootConfigSwitch(
        stringResource(R.string.install_disable_preinit),
        stringResource(R.string.install_disable_preinit_summary),
        noPreinit, onNoPreinitChange,
    )
    BootConfigSwitch(
        stringResource(R.string.install_disable_custom_rc),
        stringResource(R.string.install_disable_custom_rc_summary),
        noCustomRc, onNoCustomRcChange,
    )
    var expanded by rememberSaveable { mutableStateOf(false) }
    TextButton(
        onClick = { expanded = !expanded },
        modifier = Modifier.yukiExpansionSemantics(expanded),
    ) {
        Text(stringResource(R.string.install_force_features, forcedFeatures.size))
        Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        YukiExpandArrow(expanded = expanded)
    }
    YukiExpandableContent(visible = expanded, large = true) {
        Text(
            stringResource(R.string.install_force_features_summary),
            style = MaterialTheme.typography.bodySmall,
        )
        val labels = listOf(
            R.string.su_compat_title,
            R.string.install_feature_kernel_umount,
            R.string.install_feature_sulog,
            R.string.settings_adb_root,
            R.string.settings_selinux_hide,
            R.string.settings_shell_su_adb_only,
        )
        val choices = listOf(
            stringResource(R.string.install_feature_unlocked),
            stringResource(R.string.install_feature_force_on),
            stringResource(R.string.install_feature_force_off),
        )
        bootForceFeatureNames.forEachIndexed { index, name ->
            val value = forcedFeatures.firstOrNull { it.startsWith("$name=") }
            SuperDropdown(
                title = stringResource(labels[index]),
                items = choices,
                selectedIndex = when (value) {
                    "$name=1" -> 1
                    "$name=0" -> 2
                    else -> 0
                },
                onSelectedIndexChange = { selection ->
                    val updated = forcedFeatures.filterNot { it.startsWith("$name=") }
                    onForcedFeaturesChange(
                        when (selection) {
                            1 -> updated + "$name=1"
                            2 -> updated + "$name=0"
                            else -> updated
                        },
                    )
                },
            )
        }
    }
    Spacer(Modifier.height(12.dp))
}

@Composable
private fun BootConfigSwitch(
    title: String,
    summary: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, style = MaterialTheme.typography.bodyMedium)
            Text(summary, style = MaterialTheme.typography.bodySmall)
        }
        YukiSwitch(checked = checked, onCheckedChange = onCheckedChange)
    }
}
