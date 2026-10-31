package com.anatdx.yukisu.ui.component

import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import com.dergoogler.mmrl.ui.component.LabelItem
import com.dergoogler.mmrl.ui.component.text.TextRow

@Composable
fun SwitchItem(
    icon: ImageVector? = null,
    title: String,
    summary: String? = null,
    checked: Boolean,
    enabled: Boolean = true,
    beta: Boolean = false,
    onCheckedChange: (Boolean) -> Unit
) {
    val interactionSource = remember { MutableInteractionSource() }
    val stateAlpha = remember(checked, enabled) { Modifier.alpha(if (enabled) 1f else 0.5f) }

    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            surface = Color.Transparent
        )
    ) {
        ListItem(
            modifier = Modifier
                .toggleable(
                    value = checked,
                    interactionSource = interactionSource,
                    role = Role.Switch,
                    enabled = enabled,
                    indication = LocalIndication.current,
                    onValueChange = onCheckedChange
                ),
            content = {
                TextRow(
                    leadingContent = if (beta) {
                        {
                            LabelItem(
                                modifier = Modifier.then(stateAlpha),
                                text = "Beta"
                            )
                        }
                    } else null
                ) {
                    Text(
                        modifier = Modifier.then(stateAlpha),
                        text = title,
                        fontWeight = FontWeight.Normal,
                    )
                }
            },
            leadingContent = icon?.let {
                {
                    YukiIcon(
                        modifier = Modifier.then(stateAlpha),
                        imageVector = icon,
                        contentDescription = null
                    )
                }
            },
            trailingContent = {
                YukiSwitch(
                    checked = checked,
                    enabled = enabled,
                    onCheckedChange = null,
                    interactionSource = interactionSource
                )
            },
            supportingContent = {
                if (summary != null) {
                    Text(
                        modifier = Modifier.then(stateAlpha),
                        text = summary
                    )
                }
            }
        )
    }
}
