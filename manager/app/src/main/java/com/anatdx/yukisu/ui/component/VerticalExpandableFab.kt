package com.anatdx.yukisu.ui.component

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.updateTransition
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.motion.LocalYukiMotion
import com.anatdx.yukisu.ui.motion.MotionRole
import kotlin.math.tanh


data class FabMenuItem(
    val icon: ImageVector,
    val labelRes: Int,
    val color: Color = Color.Unspecified,
    val onClick: () -> Unit
)

object FabAnimationConfig {
    val BUTTON_SPACING = 64.dp
    val BUTTON_SIZE = 56.dp
    val SMALL_BUTTON_SIZE = 48.dp
}

@Composable
fun VerticalExpandableFab(
    menuItems: List<FabMenuItem>,
    modifier: Modifier = Modifier,
    buttonSize: Dp = FabAnimationConfig.BUTTON_SIZE,
    smallButtonSize: Dp = FabAnimationConfig.SMALL_BUTTON_SIZE,
    buttonSpacing: Dp = FabAnimationConfig.BUTTON_SPACING,
    mainButtonIcon: ImageVector = Icons.Filled.Add,
    mainButtonExpandedIcon: ImageVector = Icons.Filled.Close,
    onMainButtonClick: (() -> Unit)? = null,
) {
    val motion = LocalYukiMotion.current
    var isExpanded by remember { mutableStateOf(false) }
    val mainFocus = remember { FocusRequester() }
    val boundary = remember(isExpanded) { FabCollapseBoundary() }
    val transition = updateTransition(isExpanded, label = "FabMenu")
    val progress by transition.animateFloat(
        transitionSpec = { motion.spatial(MotionRole.Fab, expanding = targetState) },
        label = "FabMenuPosition",
    ) { if (it) 1f else 0f }
    val itemScale by transition.animateFloat(
        transitionSpec = { motion.spatial(MotionRole.Scale, expanding = targetState) },
        label = "FabMenuScale",
    ) { if (it) 1f else 0.65f }
    val opacity by transition.animateFloat(
        transitionSpec = { motion.effects() },
        label = "FabMenuOpacity",
    ) { if (it) 1f else 0f }
    val rotation by transition.animateFloat(
        transitionSpec = { motion.spatial(MotionRole.Arrow, expanding = targetState) },
        label = "FabMenuRotation",
    ) { if (it) 45f else 0f }
    val mainScale by transition.animateFloat(
        transitionSpec = { motion.spatial(MotionRole.Scale, expanding = targetState) },
        label = "FabMainScale",
    ) { if (it) 1.08f else 1f }
    val closeMenu: () -> Unit = {
        isExpanded = false
        mainFocus.requestFocus()
    }
    BackHandler(enabled = isExpanded, onBack = closeMenu)

    BoxWithConstraints(modifier = modifier.wrapContentSize(), contentAlignment = Alignment.BottomEnd) {
        val overshoot = 36.dp
        val requiredHeight = buttonSize + buttonSpacing * menuItems.size + overshoot
        val compactMenu = maxHeight < requiredHeight
        val showItems = !compactMenu && if (motion.enabled) {
            transition.currentState || transition.targetState || transition.isRunning
        } else isExpanded
        Box(
            modifier = if (showItems) Modifier.height(requiredHeight) else Modifier,
            contentAlignment = Alignment.BottomEnd,
        ) {
            if (showItems) {
                val farthest = buttonSpacing.value * menuItems.size.coerceAtLeast(1)
                menuItems.forEachIndexed { index, menuItem ->
                    Row(
                        modifier = Modifier
                            .offset {
                                val currentProgress = boundary.fraction(
                                    isExpanded, if (motion.enabled) progress else if (isExpanded) 1f else 0f,
                                )
                                val visualProgress = if (currentProgress > 1f) {
                                    1f + overshoot.value / farthest *
                                        tanh(((currentProgress - 1f) * farthest / overshoot.value).toDouble()).toFloat()
                                } else {
                                    currentProgress.coerceAtLeast(0f)
                                }
                                IntOffset(0, -(buttonSpacing * ((index + 1) * visualProgress)).roundToPx())
                            }
                            .graphicsLayer {
                                alpha = when {
                                    !isExpanded && boundary.closed -> 0f
                                    motion.enabled -> opacity.coerceIn(0f, 1f)
                                    isExpanded -> 1f
                                    else -> 0f
                                }
                            }
                            .then(if (isExpanded) Modifier else Modifier.clearAndSetSemantics {}),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.End,
                    ) {
                        Surface(
                            modifier = Modifier.padding(end = 16.dp),
                            shape = MaterialTheme.shapes.small,
                            color = MaterialTheme.colorScheme.inverseSurface,
                            tonalElevation = 6.dp,
                        ) {
                            Text(
                                text = stringResource(menuItem.labelRes),
                                modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.inverseOnSurface,
                            )
                        }
                        SmallFloatingActionButton(
                            onClick = {
                                if (isExpanded) {
                                    closeMenu()
                                    menuItem.onClick()
                                }
                            },
                            modifier = Modifier
                                .size(smallButtonSize)
                                .focusProperties { canFocus = isExpanded }
                                .graphicsLayer {
                                    val visualScale = if (motion.enabled) boundedFabScale(itemScale) else 1f
                                    scaleX = visualScale
                                    scaleY = visualScale
                                },
                            containerColor = menuItem.color.takeUnless { it == Color.Unspecified }
                                ?: MaterialTheme.colorScheme.secondary,
                            contentColor = if (menuItem.color == Color.Gray) Color.White
                                else MaterialTheme.colorScheme.onSecondary,
                            elevation = FloatingActionButtonDefaults.elevation(
                                defaultElevation = 4.dp,
                                pressedElevation = 6.dp,
                            ),
                        ) {
                            YukiIcon(menuItem.icon, stringResource(menuItem.labelRes), Modifier.size(20.dp))
                        }
                    }
                }
            }
            Box {
                FloatingActionButton(
                    onClick = {
                        onMainButtonClick?.invoke()
                        isExpanded = !isExpanded
                    },
                    modifier = Modifier
                        .size(buttonSize)
                        .focusRequester(mainFocus)
                        .graphicsLayer {
                            val visualScale = if (motion.enabled) boundedFabScale(mainScale) else if (isExpanded) 1.08f else 1f
                            scaleX = visualScale
                            scaleY = visualScale
                        },
                    elevation = FloatingActionButtonDefaults.elevation(
                        defaultElevation = 6.dp,
                        pressedElevation = 8.dp,
                        hoveredElevation = 8.dp,
                    ),
                ) {
                    val description = stringResource(if (isExpanded) R.string.collapse_menu else R.string.expand_menu)
                    if (mainButtonIcon == Icons.Filled.Add) {
                        YukiIcon(
                            mainButtonIcon,
                            description,
                            Modifier.size(24.dp).graphicsLayer {
                                rotationZ = if (motion.enabled) rotation else if (isExpanded) 45f else 0f
                            },
                        )
                    } else if (!motion.enabled) {
                        YukiIcon(
                            if (isExpanded) mainButtonExpandedIcon else mainButtonIcon,
                            description,
                            Modifier.size(24.dp),
                        )
                    } else {
                        AnimatedContent(
                            targetState = isExpanded,
                            transitionSpec = {
                                fadeIn(motion.effects()).togetherWith(fadeOut(motion.effects())).using(null)
                            },
                            label = "FabMenuIcon",
                        ) { expanded ->
                            YukiIcon(
                                if (expanded) mainButtonExpandedIcon else mainButtonIcon,
                                description,
                                Modifier.size(24.dp),
                            )
                        }
                    }
                }
                if (compactMenu) {
                    DropdownMenu(expanded = isExpanded, onDismissRequest = closeMenu) {
                        menuItems.forEach { menuItem ->
                            DropdownMenuItem(
                                text = { Text(stringResource(menuItem.labelRes)) },
                                leadingIcon = { YukiIcon(menuItem.icon, null) },
                                onClick = {
                                    closeMenu()
                                    menuItem.onClick()
                                },
                            )
                        }
                    }
                }
            }
        }
    }
}

private fun boundedFabScale(value: Float): Float =
    if (value <= 1f) value.coerceAtLeast(0f)
    else 1f + 0.18f * tanh(((value - 1f) / 0.18f).toDouble()).toFloat()

object FabMenuPresets {
    fun getScrollMenuItems(
        onScrollToTop: () -> Unit,
        onScrollToBottom: () -> Unit
    ) = listOf(
        FabMenuItem(
            icon = Icons.Filled.KeyboardArrowDown,
            labelRes = R.string.scroll_to_bottom,
            onClick = onScrollToBottom
        ),
        FabMenuItem(
            icon = Icons.Filled.KeyboardArrowUp,
            labelRes = R.string.scroll_to_top,
            onClick = onScrollToTop
        )
    )

    @Composable
    fun getBatchActionMenuItems(
        onCancel: () -> Unit,
        onDeny: () -> Unit,
        onAllow: () -> Unit,
        onUnmountModules: () -> Unit,
        onDisableUnmount: () -> Unit
    ) = listOf(
        FabMenuItem(
            icon = Icons.Filled.Close,
            labelRes = R.string.cancel,
            color = Color.Gray,
            onClick = onCancel
        ),
        FabMenuItem(
            icon = Icons.Filled.Block,
            labelRes = R.string.deny_authorization,
            color = MaterialTheme.colorScheme.error,
            onClick = onDeny
        ),
        FabMenuItem(
            icon = Icons.Filled.Check,
            labelRes = R.string.grant_authorization,
            color = MaterialTheme.colorScheme.primary,
            onClick = onAllow
        ),
        FabMenuItem(
            icon = Icons.Filled.FolderOff,
            labelRes = R.string.unmount_modules,
            onClick = onUnmountModules
        ),
        FabMenuItem(
            icon = Icons.Filled.Folder,
            labelRes = R.string.disable_unmount,
            onClick = onDisableUnmount
        )
    )
}
