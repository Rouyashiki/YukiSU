package ui.screen.moreSettings.component

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material3.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.ui.semantics.Role
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.anatdx.yukisu.ui.theme.*
import com.anatdx.yukisu.ui.component.YukiSwitch
import com.anatdx.yukisu.ui.component.YukiIcon

enum class MoreSettingsItemPosition(val index: Int, val count: Int) {
    First(0, 3),
    Middle(1, 3),
    Last(2, 3),
    Only(0, 1)
}

@Composable
fun SettingsCard(
    title: String,
    icon: ImageVector? = null,
    content: @Composable () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = ManagerUi.GroupSpacing)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = ManagerUi.SectionTitlePadding, vertical = ManagerUi.SectionTitlePadding)
        ) {
            if (icon != null) {
                YukiIcon(
                    imageVector = icon,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(24.dp)
                )
                Spacer(modifier = Modifier.width(12.dp))
            }
            Text(
                text = title,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
                color = MaterialTheme.colorScheme.primary
            )
        }
        Column(content = { content() })
    }
}

@Composable
fun SettingItem(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    groupPosition: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
    onClick: () -> Unit,
    iconTint: Color = MaterialTheme.colorScheme.primary,
    trailingContent: @Composable (() -> Unit)? = {
        YukiIcon(
            Icons.AutoMirrored.Filled.NavigateNext,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
) {
    val expressiveShape = if (groupPosition == MoreSettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                Modifier
                    .padding(
                        horizontal = 6.dp,
                        vertical = ListItemDefaults.SegmentedGap / 2
                    )
                    .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                    .clip(expressiveShape)
                    .background(
                        MaterialTheme.colorScheme.surfaceContainer.copy(
                            alpha = CardConfig.cardAlpha
                        )
                    )
                    .clickable(onClick = onClick)
            )
            .padding(horizontal = ManagerUi.CardPadding, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        MoreSettingsLeadingIcon(icon = icon, tint = iconTint)

        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Normal,
                maxLines = Int.MAX_VALUE,
                overflow = TextOverflow.Visible
            )
            if (subtitle != null) {
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = Int.MAX_VALUE,
                    overflow = TextOverflow.Visible
                )
            }
        }

        trailingContent?.invoke()
    }
}

@Composable
fun SwitchSettingItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    checked: Boolean,
    enabled: Boolean = true,
    groupPosition: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
    onChange: (Boolean) -> Unit
) {
    val expressiveShape = if (groupPosition == MoreSettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.6f)
            .then(
                Modifier
                    .padding(
                        horizontal = 6.dp,
                        vertical = ListItemDefaults.SegmentedGap / 2
                    )
                    .defaultMinSize(minHeight = ExpressiveListGroupMinHeight)
                    .clip(expressiveShape)
                    .background(
                        MaterialTheme.colorScheme.surfaceContainer.copy(
                            alpha = CardConfig.cardAlpha
                        )
                    )
                    .toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            )
            .padding(horizontal = ManagerUi.CardPadding, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        MoreSettingsLeadingIcon(
            icon = icon,
            tint = if (checked) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant
            }
        )

        Column(
            modifier = Modifier.weight(1f).padding(end = 12.dp),
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Normal,
            )
            if (summary != null) {
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        YukiSwitch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = null
        )
    }
}

@Composable
fun SettingsControlGroup(
    groupPosition: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
    content: @Composable ColumnScope.() -> Unit
) {
    val expressiveShape = if (groupPosition == MoreSettingsItemPosition.Only) {
        MaterialTheme.shapes.large
    } else {
        ListItemDefaults.segmentedShapes(groupPosition.index, groupPosition.count).shape
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(
                horizontal = 6.dp,
                vertical = ListItemDefaults.SegmentedGap / 2
            )
            .clip(expressiveShape)
            .background(
                MaterialTheme.colorScheme.surfaceContainer.copy(alpha = CardConfig.cardAlpha)
            )
            .padding(horizontal = 16.dp, vertical = 12.dp),
        content = content
    )
}

@Composable
private fun MoreSettingsLeadingIcon(icon: ImageVector, tint: Color) {
    YukiIcon(
        imageVector = icon,
        contentDescription = null,
        tint = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier
            .padding(end = 16.dp)
            .size(24.dp)
    )
}

@Composable
fun SettingsDivider() {
    HorizontalDivider(
        modifier = Modifier.padding(vertical = 8.dp)
    )
}

@Composable
fun ColorCircle(
    color: Color,
    isSelected: Boolean,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .size(20.dp)
            .clip(CircleShape)
            .background(color)
            .then(
                if (isSelected) {
                    Modifier.border(
                        width = 2.dp,
                        color = MaterialTheme.colorScheme.primary,
                        shape = CircleShape
                    )
                } else {
                    Modifier
                }
            )
    )
}
