package org.rouyashiki.yukisu.ui.theme

import androidx.compose.ui.graphics.Color

sealed class ThemeColors {
    // Light theme
    abstract val primaryLight: Color
    abstract val onPrimaryLight: Color
    abstract val primaryContainerLight: Color
    abstract val onPrimaryContainerLight: Color
    abstract val secondaryLight: Color
    abstract val onSecondaryLight: Color
    abstract val secondaryContainerLight: Color
    abstract val onSecondaryContainerLight: Color
    abstract val tertiaryLight: Color
    abstract val onTertiaryLight: Color
    abstract val tertiaryContainerLight: Color
    abstract val onTertiaryContainerLight: Color
    abstract val errorLight: Color
    abstract val onErrorLight: Color
    abstract val errorContainerLight: Color
    abstract val onErrorContainerLight: Color
    abstract val backgroundLight: Color
    abstract val onBackgroundLight: Color
    abstract val surfaceLight: Color
    abstract val onSurfaceLight: Color
    abstract val surfaceVariantLight: Color
    abstract val onSurfaceVariantLight: Color
    abstract val outlineLight: Color
    abstract val outlineVariantLight: Color
    abstract val scrimLight: Color
    abstract val inverseSurfaceLight: Color
    abstract val inverseOnSurfaceLight: Color
    abstract val inversePrimaryLight: Color
    abstract val surfaceDimLight: Color
    abstract val surfaceBrightLight: Color
    abstract val surfaceContainerLowestLight: Color
    abstract val surfaceContainerLowLight: Color
    abstract val surfaceContainerLight: Color
    abstract val surfaceContainerHighLight: Color
    abstract val surfaceContainerHighestLight: Color
    // Dark theme
    abstract val primaryDark: Color
    abstract val onPrimaryDark: Color
    abstract val primaryContainerDark: Color
    abstract val onPrimaryContainerDark: Color
    abstract val secondaryDark: Color
    abstract val onSecondaryDark: Color
    abstract val secondaryContainerDark: Color
    abstract val onSecondaryContainerDark: Color
    abstract val tertiaryDark: Color
    abstract val onTertiaryDark: Color
    abstract val tertiaryContainerDark: Color
    abstract val onTertiaryContainerDark: Color
    abstract val errorDark: Color
    abstract val onErrorDark: Color
    abstract val errorContainerDark: Color
    abstract val onErrorContainerDark: Color
    abstract val backgroundDark: Color
    abstract val onBackgroundDark: Color
    abstract val surfaceDark: Color
    abstract val onSurfaceDark: Color
    abstract val surfaceVariantDark: Color
    abstract val onSurfaceVariantDark: Color
    abstract val outlineDark: Color
    abstract val outlineVariantDark: Color
    abstract val scrimDark: Color
    abstract val inverseSurfaceDark: Color
    abstract val inverseOnSurfaceDark: Color
    abstract val inversePrimaryDark: Color
    abstract val surfaceDimDark: Color
    abstract val surfaceBrightDark: Color
    abstract val surfaceContainerLowestDark: Color
    abstract val surfaceContainerLowDark: Color
    abstract val surfaceContainerDark: Color
    abstract val surfaceContainerHighDark: Color
    abstract val surfaceContainerHighestDark: Color

    object Default : ThemeColors() {
        override val primaryLight = Color(0xFF415F91)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFD6E3FF)
        override val onPrimaryContainerLight = Color(0xFF284777)
        override val secondaryLight = Color(0xFF565F71)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFDAE2F9)
        override val onSecondaryContainerLight = Color(0xFF3E4759)
        override val tertiaryLight = Color(0xFF705575)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFFAD8FD)
        override val onTertiaryContainerLight = Color(0xFF573E5C)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFF9F9FF)
        override val onBackgroundLight = Color(0xFF191C20)
        override val surfaceLight = Color(0xFFF9F9FF)
        override val onSurfaceLight = Color(0xFF191C20)
        override val surfaceVariantLight = Color(0xFFE0E2EC)
        override val onSurfaceVariantLight = Color(0xFF44474E)
        override val outlineLight = Color(0xFF74777F)
        override val outlineVariantLight = Color(0xFFC4C6D0)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF2E3036)
        override val inverseOnSurfaceLight = Color(0xFFF0F0F7)
        override val inversePrimaryLight = Color(0xFFAAC7FF)
        override val surfaceDimLight = Color(0xFFD9D9E0)
        override val surfaceBrightLight = Color(0xFFF9F9FF)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFF3F3FA)
        override val surfaceContainerLight = Color(0xFFEDEDF4)
        override val surfaceContainerHighLight = Color(0xFFE7E8EE)
        override val surfaceContainerHighestLight = Color(0xFFE2E2E9)

        override val primaryDark = Color(0xFFAAC7FF)
        override val onPrimaryDark = Color(0xFF0A305F)
        override val primaryContainerDark = Color(0xFF284777)
        override val onPrimaryContainerDark = Color(0xFFD6E3FF)
        override val secondaryDark = Color(0xFFBEC6DC)
        override val onSecondaryDark = Color(0xFF283141)
        override val secondaryContainerDark = Color(0xFF3E4759)
        override val onSecondaryContainerDark = Color(0xFFDAE2F9)
        override val tertiaryDark = Color(0xFFDDBCE0)
        override val onTertiaryDark = Color(0xFF3F2844)
        override val tertiaryContainerDark = Color(0xFF573E5C)
        override val onTertiaryContainerDark = Color(0xFFFAD8FD)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF111318)
        override val onBackgroundDark = Color(0xFFE2E2E9)
        override val surfaceDark = Color(0xFF111318)
        override val onSurfaceDark = Color(0xFFE2E2E9)
        override val surfaceVariantDark = Color(0xFF44474E)
        override val onSurfaceVariantDark = Color(0xFFC4C6D0)
        override val outlineDark = Color(0xFF8E9099)
        override val outlineVariantDark = Color(0xFF44474E)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE2E2E9)
        override val inverseOnSurfaceDark = Color(0xFF2E3036)
        override val inversePrimaryDark = Color(0xFF415F91)
        override val surfaceDimDark = Color(0xFF111318)
        override val surfaceBrightDark = Color(0xFF37393E)
        override val surfaceContainerLowestDark = Color(0xFF0C0E13)
        override val surfaceContainerLowDark = Color(0xFF191C20)
        override val surfaceContainerDark = Color(0xFF1D2024)
        override val surfaceContainerHighDark = Color(0xFF282A2F)
        override val surfaceContainerHighestDark = Color(0xFF33353A)
    }

    object Green : ThemeColors() {
        override val primaryLight = Color(0xFF4C662B)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFCDEDA3)
        override val onPrimaryContainerLight = Color(0xFF354E16)
        override val secondaryLight = Color(0xFF586249)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFDCE7C8)
        override val onSecondaryContainerLight = Color(0xFF404A33)
        override val tertiaryLight = Color(0xFF386663)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFBCECE7)
        override val onTertiaryContainerLight = Color(0xFF1F4E4B)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFF9FAEF)
        override val onBackgroundLight = Color(0xFF1A1C16)
        override val surfaceLight = Color(0xFFF9FAEF)
        override val onSurfaceLight = Color(0xFF1A1C16)
        override val surfaceVariantLight = Color(0xFFE1E4D5)
        override val onSurfaceVariantLight = Color(0xFF44483D)
        override val outlineLight = Color(0xFF75796C)
        override val outlineVariantLight = Color(0xFFC5C8BA)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF2F312A)
        override val inverseOnSurfaceLight = Color(0xFFF1F2E6)
        override val inversePrimaryLight = Color(0xFFB1D18A)
        override val surfaceDimLight = Color(0xFFDADBD0)
        override val surfaceBrightLight = Color(0xFFF9FAEF)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFF3F4E9)
        override val surfaceContainerLight = Color(0xFFEEEFE3)
        override val surfaceContainerHighLight = Color(0xFFE8E9DE)
        override val surfaceContainerHighestLight = Color(0xFFE2E3D8)

        override val primaryDark = Color(0xFFB1D18A)
        override val onPrimaryDark = Color(0xFF1F3701)
        override val primaryContainerDark = Color(0xFF354E16)
        override val onPrimaryContainerDark = Color(0xFFCDEDA3)
        override val secondaryDark = Color(0xFFBFCBAD)
        override val onSecondaryDark = Color(0xFF2A331E)
        override val secondaryContainerDark = Color(0xFF404A33)
        override val onSecondaryContainerDark = Color(0xFFDCE7C8)
        override val tertiaryDark = Color(0xFFA0D0CB)
        override val onTertiaryDark = Color(0xFF003735)
        override val tertiaryContainerDark = Color(0xFF1F4E4B)
        override val onTertiaryContainerDark = Color(0xFFBCECE7)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF12140E)
        override val onBackgroundDark = Color(0xFFE2E3D8)
        override val surfaceDark = Color(0xFF12140E)
        override val onSurfaceDark = Color(0xFFE2E3D8)
        override val surfaceVariantDark = Color(0xFF44483D)
        override val onSurfaceVariantDark = Color(0xFFC5C8BA)
        override val outlineDark = Color(0xFF8F9285)
        override val outlineVariantDark = Color(0xFF44483D)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE2E3D8)
        override val inverseOnSurfaceDark = Color(0xFF2F312A)
        override val inversePrimaryDark = Color(0xFF4C662B)
        override val surfaceDimDark = Color(0xFF12140E)
        override val surfaceBrightDark = Color(0xFF383A32)
        override val surfaceContainerLowestDark = Color(0xFF0C0F09)
        override val surfaceContainerLowDark = Color(0xFF1A1C16)
        override val surfaceContainerDark = Color(0xFF1E201A)
        override val surfaceContainerHighDark = Color(0xFF282B24)
        override val surfaceContainerHighestDark = Color(0xFF33362E)
    }

    object Purple : ThemeColors() {
        override val primaryLight = Color(0xFF7C4E7E)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFFFD6FC)
        override val onPrimaryContainerLight = Color(0xFF623765)
        override val secondaryLight = Color(0xFF6C586B)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFF5DBF1)
        override val onSecondaryContainerLight = Color(0xFF534152)
        override val tertiaryLight = Color(0xFF825249)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFFFDAD4)
        override val onTertiaryContainerLight = Color(0xFF673B33)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFFFF7FA)
        override val onBackgroundLight = Color(0xFF1F1A1F)
        override val surfaceLight = Color(0xFFFFF7FA)
        override val onSurfaceLight = Color(0xFF1F1A1F)
        override val surfaceVariantLight = Color(0xFFEDDFE8)
        override val onSurfaceVariantLight = Color(0xFF4D444C)
        override val outlineLight = Color(0xFF7F747C)
        override val outlineVariantLight = Color(0xFFD0C3CC)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF352F34)
        override val inverseOnSurfaceLight = Color(0xFFF9EEF4)
        override val inversePrimaryLight = Color(0xFFECB4EC)
        override val surfaceDimLight = Color(0xFFE2D7DE)
        override val surfaceBrightLight = Color(0xFFFFF7FA)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFFCF0F7)
        override val surfaceContainerLight = Color(0xFFF6EBF2)
        override val surfaceContainerHighLight = Color(0xFFF0E5EC)
        override val surfaceContainerHighestLight = Color(0xFFEBDFE6)

        override val primaryDark = Color(0xFFECB4EC)
        override val onPrimaryDark = Color(0xFF49204D)
        override val primaryContainerDark = Color(0xFF623765)
        override val onPrimaryContainerDark = Color(0xFFFFD6FC)
        override val secondaryDark = Color(0xFFD8BFD5)
        override val onSecondaryDark = Color(0xFF3B2B3B)
        override val secondaryContainerDark = Color(0xFF534152)
        override val onSecondaryContainerDark = Color(0xFFF5DBF1)
        override val tertiaryDark = Color(0xFFF6B8AD)
        override val onTertiaryDark = Color(0xFF4C251F)
        override val tertiaryContainerDark = Color(0xFF673B33)
        override val onTertiaryContainerDark = Color(0xFFFFDAD4)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF171216)
        override val onBackgroundDark = Color(0xFFEBDFE6)
        override val surfaceDark = Color(0xFF171216)
        override val onSurfaceDark = Color(0xFFEBDFE6)
        override val surfaceVariantDark = Color(0xFF4D444C)
        override val onSurfaceVariantDark = Color(0xFFD0C3CC)
        override val outlineDark = Color(0xFF998D96)
        override val outlineVariantDark = Color(0xFF4D444C)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFEBDFE6)
        override val inverseOnSurfaceDark = Color(0xFF352F34)
        override val inversePrimaryDark = Color(0xFF7C4E7E)
        override val surfaceDimDark = Color(0xFF171216)
        override val surfaceBrightDark = Color(0xFF3E373D)
        override val surfaceContainerLowestDark = Color(0xFF110D11)
        override val surfaceContainerLowDark = Color(0xFF1F1A1F)
        override val surfaceContainerDark = Color(0xFF231E23)
        override val surfaceContainerHighDark = Color(0xFF2E282D)
        override val surfaceContainerHighestDark = Color(0xFF393338)
    }

    object Orange : ThemeColors() {
        override val primaryLight = Color(0xFF8B4F24)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFFFDCC7)
        override val onPrimaryContainerLight = Color(0xFF6E390E)
        override val secondaryLight = Color(0xFF755846)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFFFDCC7)
        override val onSecondaryContainerLight = Color(0xFF5B4130)
        override val tertiaryLight = Color(0xFF865219)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFFFDCBF)
        override val onTertiaryContainerLight = Color(0xFF6A3B01)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFFFF8F5)
        override val onBackgroundLight = Color(0xFF221A15)
        override val surfaceLight = Color(0xFFFFF8F5)
        override val onSurfaceLight = Color(0xFF221A15)
        override val surfaceVariantLight = Color(0xFFF4DED3)
        override val onSurfaceVariantLight = Color(0xFF52443C)
        override val outlineLight = Color(0xFF84746A)
        override val outlineVariantLight = Color(0xFFD7C3B8)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF382E29)
        override val inverseOnSurfaceLight = Color(0xFFFFEDE5)
        override val inversePrimaryLight = Color(0xFFFFB787)
        override val surfaceDimLight = Color(0xFFE7D7CE)
        override val surfaceBrightLight = Color(0xFFFFF8F5)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFFFF1EA)
        override val surfaceContainerLight = Color(0xFFFCEBE2)
        override val surfaceContainerHighLight = Color(0xFFF6E5DC)
        override val surfaceContainerHighestLight = Color(0xFFF0DFD7)

        override val primaryDark = Color(0xFFFFB787)
        override val onPrimaryDark = Color(0xFF502400)
        override val primaryContainerDark = Color(0xFF6E390E)
        override val onPrimaryContainerDark = Color(0xFFFFDCC7)
        override val secondaryDark = Color(0xFFE5BFA8)
        override val onSecondaryDark = Color(0xFF422B1B)
        override val secondaryContainerDark = Color(0xFF5B4130)
        override val onSecondaryContainerDark = Color(0xFFFFDCC7)
        override val tertiaryDark = Color(0xFFFDB876)
        override val onTertiaryDark = Color(0xFF4B2800)
        override val tertiaryContainerDark = Color(0xFF6A3B01)
        override val onTertiaryContainerDark = Color(0xFFFFDCBF)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF19120D)
        override val onBackgroundDark = Color(0xFFF0DFD7)
        override val surfaceDark = Color(0xFF19120D)
        override val onSurfaceDark = Color(0xFFF0DFD7)
        override val surfaceVariantDark = Color(0xFF52443C)
        override val onSurfaceVariantDark = Color(0xFFD7C3B8)
        override val outlineDark = Color(0xFF9F8D83)
        override val outlineVariantDark = Color(0xFF52443C)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFF0DFD7)
        override val inverseOnSurfaceDark = Color(0xFF382E29)
        override val inversePrimaryDark = Color(0xFF8B4F24)
        override val surfaceDimDark = Color(0xFF19120D)
        override val surfaceBrightDark = Color(0xFF413731)
        override val surfaceContainerLowestDark = Color(0xFF140D08)
        override val surfaceContainerLowDark = Color(0xFF221A15)
        override val surfaceContainerDark = Color(0xFF261E19)
        override val surfaceContainerHighDark = Color(0xFF312823)
        override val surfaceContainerHighestDark = Color(0xFF3D332D)
    }

    object Pink : ThemeColors() {
        override val primaryLight = Color(0xFF8C4A60)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFFFD9E2)
        override val onPrimaryContainerLight = Color(0xFF703348)
        override val secondaryLight = Color(0xFF8B4A62)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFFFD9E3)
        override val onSecondaryContainerLight = Color(0xFF6F334B)
        override val tertiaryLight = Color(0xFF8B4A62)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFFFD9E3)
        override val onTertiaryContainerLight = Color(0xFF6F334B)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFFFF8F8)
        override val onBackgroundLight = Color(0xFF22191B)
        override val surfaceLight = Color(0xFFFFF8F8)
        override val onSurfaceLight = Color(0xFF22191B)
        override val surfaceVariantLight = Color(0xFFF2DDE1)
        override val onSurfaceVariantLight = Color(0xFF514346)
        override val outlineLight = Color(0xFF837377)
        override val outlineVariantLight = Color(0xFFD5C2C5)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF372E30)
        override val inverseOnSurfaceLight = Color(0xFFFDEDEF)
        override val inversePrimaryLight = Color(0xFFFFB1C7)
        override val surfaceDimLight = Color(0xFFE6D6D9)
        override val surfaceBrightLight = Color(0xFFFFF8F8)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFFFF0F2)
        override val surfaceContainerLight = Color(0xFFFBEAED)
        override val surfaceContainerHighLight = Color(0xFFF5E4E7)
        override val surfaceContainerHighestLight = Color(0xFFEFDFE1)

        override val primaryDark = Color(0xFFFFB1C7)
        override val onPrimaryDark = Color(0xFF541D32)
        override val primaryContainerDark = Color(0xFF703348)
        override val onPrimaryContainerDark = Color(0xFFFFD9E2)
        override val secondaryDark = Color(0xFFFFB0CB)
        override val onSecondaryDark = Color(0xFF541D34)
        override val secondaryContainerDark = Color(0xFF6F334B)
        override val onSecondaryContainerDark = Color(0xFFFFD9E3)
        override val tertiaryDark = Color(0xFFFFB0CB)
        override val onTertiaryDark = Color(0xFF541D34)
        override val tertiaryContainerDark = Color(0xFF6F334B)
        override val onTertiaryContainerDark = Color(0xFFFFD9E3)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF191113)
        override val onBackgroundDark = Color(0xFFEFDFE1)
        override val surfaceDark = Color(0xFF191113)
        override val onSurfaceDark = Color(0xFFEFDFE1)
        override val surfaceVariantDark = Color(0xFF514346)
        override val onSurfaceVariantDark = Color(0xFFD5C2C5)
        override val outlineDark = Color(0xFF9E8C90)
        override val outlineVariantDark = Color(0xFF514346)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFEFDFE1)
        override val inverseOnSurfaceDark = Color(0xFF372E30)
        override val inversePrimaryDark = Color(0xFF8C4A60)
        override val surfaceDimDark = Color(0xFF191113)
        override val surfaceBrightDark = Color(0xFF413739)
        override val surfaceContainerLowestDark = Color(0xFF140C0E)
        override val surfaceContainerLowDark = Color(0xFF22191B)
        override val surfaceContainerDark = Color(0xFF261D1F)
        override val surfaceContainerHighDark = Color(0xFF31282A)
        override val surfaceContainerHighestDark = Color(0xFF3C3234)
    }

    object Gray : ThemeColors() {
        override val primaryLight = Color(0xFF5B5C5C)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFF747474)
        override val onPrimaryContainerLight = Color(0xFFFEFCFC)
        override val secondaryLight = Color(0xFF5F5E5E)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFE4E2E1)
        override val onSecondaryContainerLight = Color(0xFF656464)
        override val tertiaryLight = Color(0xFF5E5B5D)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFF777375)
        override val onTertiaryContainerLight = Color(0xFFFFFBFF)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFFCF8F8)
        override val onBackgroundLight = Color(0xFF1C1B1B)
        override val surfaceLight = Color(0xFFFCF8F8)
        override val onSurfaceLight = Color(0xFF1C1B1B)
        override val surfaceVariantLight = Color(0xFFE0E3E3)
        override val onSurfaceVariantLight = Color(0xFF444748)
        override val outlineLight = Color(0xFF747878)
        override val outlineVariantLight = Color(0xFFC4C7C7)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF313030)
        override val inverseOnSurfaceLight = Color(0xFFF4F0EF)
        override val inversePrimaryLight = Color(0xFFC7C6C6)
        override val surfaceDimLight = Color(0xFFDDD9D8)
        override val surfaceBrightLight = Color(0xFFFCF8F8)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFF7F3F2)
        override val surfaceContainerLight = Color(0xFFF1EDEC)
        override val surfaceContainerHighLight = Color(0xFFEBE7E7)
        override val surfaceContainerHighestLight = Color(0xFFE5E2E1)

        override val primaryDark = Color(0xFFC7C6C6)
        override val onPrimaryDark = Color(0xFF303031)
        override val primaryContainerDark = Color(0xFF919190)
        override val onPrimaryContainerDark = Color(0xFF161718)
        override val secondaryDark = Color(0xFFC8C6C5)
        override val onSecondaryDark = Color(0xFF303030)
        override val secondaryContainerDark = Color(0xFF474746)
        override val onSecondaryContainerDark = Color(0xFFB7B5B4)
        override val tertiaryDark = Color(0xFFCAC5C7)
        override val onTertiaryDark = Color(0xFF323031)
        override val tertiaryContainerDark = Color(0xFF948F91)
        override val onTertiaryContainerDark = Color(0xFF181718)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF141313)
        override val onBackgroundDark = Color(0xFFE5E2E1)
        override val surfaceDark = Color(0xFF141313)
        override val onSurfaceDark = Color(0xFFE5E2E1)
        override val surfaceVariantDark = Color(0xFF444748)
        override val onSurfaceVariantDark = Color(0xFFC4C7C7)
        override val outlineDark = Color(0xFF8E9192)
        override val outlineVariantDark = Color(0xFF444748)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE5E2E1)
        override val inverseOnSurfaceDark = Color(0xFF313030)
        override val inversePrimaryDark = Color(0xFF5E5E5E)
        override val surfaceDimDark = Color(0xFF141313)
        override val surfaceBrightDark = Color(0xFF3A3939)
        override val surfaceContainerLowestDark = Color(0xFF0E0E0E)
        override val surfaceContainerLowDark = Color(0xFF1C1B1B)
        override val surfaceContainerDark = Color(0xFF201F1F)
        override val surfaceContainerHighDark = Color(0xFF2A2A2A)
        override val surfaceContainerHighestDark = Color(0xFF353434)
    }

    object Yellow : ThemeColors() {
        override val primaryLight = Color(0xFF6D5E0F)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFF8E288)
        override val onPrimaryContainerLight = Color(0xFF534600)
        override val secondaryLight = Color(0xFF6D5E0F)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFF7E388)
        override val onSecondaryContainerLight = Color(0xFF534600)
        override val tertiaryLight = Color(0xFF685F13)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFF1E58A)
        override val onTertiaryContainerLight = Color(0xFF4F4800)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFFFF9ED)
        override val onBackgroundLight = Color(0xFF1E1C13)
        override val surfaceLight = Color(0xFFFFF9ED)
        override val onSurfaceLight = Color(0xFF1E1C13)
        override val surfaceVariantLight = Color(0xFFE9E2D0)
        override val onSurfaceVariantLight = Color(0xFF4B4739)
        override val outlineLight = Color(0xFF7C7768)
        override val outlineVariantLight = Color(0xFFCDC6B4)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF333027)
        override val inverseOnSurfaceLight = Color(0xFFF7F0E2)
        override val inversePrimaryLight = Color(0xFFDAC66F)
        override val surfaceDimLight = Color(0xFFE0D9CC)
        override val surfaceBrightLight = Color(0xFFFFF9ED)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFFAF3E5)
        override val surfaceContainerLight = Color(0xFFF4EDDF)
        override val surfaceContainerHighLight = Color(0xFFEEE8DA)
        override val surfaceContainerHighestLight = Color(0xFFE8E2D4)

        override val primaryDark = Color(0xFFDAC66F)
        override val onPrimaryDark = Color(0xFF393000)
        override val primaryContainerDark = Color(0xFF534600)
        override val onPrimaryContainerDark = Color(0xFFF8E288)
        override val secondaryDark = Color(0xFFDAC76F)
        override val onSecondaryDark = Color(0xFF393000)
        override val secondaryContainerDark = Color(0xFF534600)
        override val onSecondaryContainerDark = Color(0xFFF7E388)
        override val tertiaryDark = Color(0xFFD4C871)
        override val onTertiaryDark = Color(0xFF363100)
        override val tertiaryContainerDark = Color(0xFF4F4800)
        override val onTertiaryContainerDark = Color(0xFFF1E58A)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF15130B)
        override val onBackgroundDark = Color(0xFFE8E2D4)
        override val surfaceDark = Color(0xFF15130B)
        override val onSurfaceDark = Color(0xFFE8E2D4)
        override val surfaceVariantDark = Color(0xFF4B4739)
        override val onSurfaceVariantDark = Color(0xFFCDC6B4)
        override val outlineDark = Color(0xFF969080)
        override val outlineVariantDark = Color(0xFF4B4739)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE8E2D4)
        override val inverseOnSurfaceDark = Color(0xFF333027)
        override val inversePrimaryDark = Color(0xFF6D5E0F)
        override val surfaceDimDark = Color(0xFF15130B)
        override val surfaceBrightDark = Color(0xFF3C3930)
        override val surfaceContainerLowestDark = Color(0xFF100E07)
        override val surfaceContainerLowDark = Color(0xFF1E1C13)
        override val surfaceContainerDark = Color(0xFF222017)
        override val surfaceContainerHighDark = Color(0xFF2C2A21)
        override val surfaceContainerHighestDark = Color(0xFF37352B)
    }

    companion object {
        fun fromName(name: String): ThemeColors = when (name.lowercase()) {
            "green" -> Green
            "purple" -> Purple
            "orange" -> Orange
            "pink" -> Pink
            "gray" -> Gray
            "yellow" -> Yellow
            "trans", "transright" -> TransPride
            else -> Default
        }
    }

    object TransPride : ThemeColors() {
        private val c1 = Color(0xFF5BCFFA)
        private val c2 = Color(0xFFF5ABB9)
        private val c3 = Color(0xFFFFFFFF)
        private val c1l = Color(0xFF91E0FF)
        private val c1m = Color(0xFF3BB8E8)
        private val c1d = Color(0xFF0D7FAD)
        private val c1x = Color(0xFF065A7A)
        private val c2l = Color(0xFFFFD4E0)
        private val c2m = Color(0xFFE896AB)
        private val c2d = Color(0xFFBF6B83)
        private val c2x = Color(0xFF8E4A5E)
        private val t1 = Color(0xFFC8B6FF)
        private val t1l = Color(0xFFE8DEFF)
        private val t1d = Color(0xFF9B7FD1)
        private val s1 = Color(0xFFF0FAFF)
        private val s2 = Color(0xFFFFF5F8)
        private val ds = Color(0xFF16141A)
        override val primaryLight = c1m
        override val onPrimaryLight = ds
        override val primaryContainerLight = Color(0xFFCCF0FF)
        override val onPrimaryContainerLight = c1x
        override val secondaryLight = c2m
        override val onSecondaryLight = ds
        override val secondaryContainerLight = Color(0xFFFFE4EC)
        override val onSecondaryContainerLight = c2x
        override val tertiaryLight = t1d
        override val onTertiaryLight = ds
        override val tertiaryContainerLight = t1l
        override val onTertiaryContainerLight = Color(0xFF3D2E66)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = c3
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF410002)
        override val backgroundLight = c3
        override val onBackgroundLight = Color(0xFF1A1B1F)
        override val surfaceLight = c3
        override val onSurfaceLight = Color(0xFF1A1B1F)
        override val surfaceVariantLight = Color(0xFFE8E0F0)
        override val onSurfaceVariantLight = Color(0xFF49454F)
        override val outlineLight = c2m.copy(alpha = 0.5f)
        override val outlineVariantLight = c1l.copy(alpha = 0.6f)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = ds
        override val inverseOnSurfaceLight = Color(0xFFF4EFF4)
        override val inversePrimaryLight = c1l
        override val surfaceDimLight = Color(0xFFDDD8E4)
        override val surfaceBrightLight = c3
        override val surfaceContainerLowestLight = c3
        override val surfaceContainerLowLight = s2
        override val surfaceContainerLight = Color(0xFFF8F2FA)
        override val surfaceContainerHighLight = Color(0xFFF2ECF8)
        override val surfaceContainerHighestLight = Color(0xFFECE6F3)
        override val primaryDark = c1
        override val onPrimaryDark = ds
        override val primaryContainerDark = c1d
        override val onPrimaryContainerDark = c3
        override val secondaryDark = c2
        override val onSecondaryDark = ds
        override val secondaryContainerDark = c2d
        override val onSecondaryContainerDark = ds
        override val tertiaryDark = t1
        override val onTertiaryDark = Color(0xFF2D1F4E)
        override val tertiaryContainerDark = Color(0xFF453670)
        override val onTertiaryContainerDark = t1l
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = ds
        override val onBackgroundDark = Color(0xFFE6E1E6)
        override val surfaceDark = ds
        override val onSurfaceDark = Color(0xFFE6E1E6)
        override val surfaceVariantDark = Color(0xFF49454F)
        override val onSurfaceVariantDark = Color(0xFFCBC4CF)
        override val outlineDark = c1.copy(alpha = 0.6f)
        override val outlineVariantDark = c2.copy(alpha = 0.4f)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE6E1E6)
        override val inverseOnSurfaceDark = Color(0xFF313033)
        override val inversePrimaryDark = c1m
        override val surfaceDimDark = ds
        override val surfaceBrightDark = Color(0xFF3B383E)
        override val surfaceContainerLowestDark = Color(0xFF0F0D12)
        override val surfaceContainerLowDark = Color(0xFF1C1A21)
        override val surfaceContainerDark = Color(0xFF201E25)
        override val surfaceContainerHighDark = Color(0xFF2B2830)
        override val surfaceContainerHighestDark = Color(0xFF36333B)
    }
}
