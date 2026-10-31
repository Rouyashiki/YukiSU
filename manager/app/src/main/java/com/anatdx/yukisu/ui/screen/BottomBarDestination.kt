package com.anatdx.yukisu.ui.screen

import androidx.annotation.DrawableRes
import androidx.annotation.StringRes
import com.ramcosta.composedestinations.generated.destinations.*
import com.ramcosta.composedestinations.spec.DirectionDestinationSpec
import com.anatdx.yukisu.R

enum class BottomBarDestination(
    val direction: DirectionDestinationSpec,
    @param:StringRes val label: Int,
    @param:DrawableRes val icon: Int,
    val rootRequired: Boolean,
) {
    Home(HomeScreenDestination, R.string.home, R.drawable.ms_home, false),
    Plugin(PluginScreenDestination, R.string.plugin, R.drawable.ms_developer_mode, true),
    SuperUser(SuperUserScreenDestination, R.string.superuser, R.drawable.ms_admin_panel_settings, true),
    Module(ModuleScreenDestination, R.string.module, R.drawable.ms_extension, true),
    Settings(SettingScreenDestination, R.string.settings, R.drawable.ms_settings_suggest, false),
}
