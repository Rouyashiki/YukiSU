package com.anatdx.yukisu.ui.component

import android.widget.Toast
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Key
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.ThemeConfig
import com.anatdx.yukisu.ui.theme.ThemeColors
import com.anatdx.yukisu.ui.theme.ThemeManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * SuperKey authentication dialog state
 */
sealed interface SuperKeyAuthResult {
    data class Success(val superKey: String) : SuperKeyAuthResult
    object Canceled : SuperKeyAuthResult
    data class Error(val message: String) : SuperKeyAuthResult
}

interface SuperKeyDialogHandle {
    val isShown: Boolean
    fun show()
    fun hide()
    suspend fun awaitSuperKey(): SuperKeyAuthResult
}

class SuperKeyDialogState : SuperKeyDialogHandle {
    internal var isVisible by mutableStateOf(false)
        private set

    internal var onResult: ((SuperKeyAuthResult) -> Unit)? = null

    override val isShown: Boolean
        get() = isVisible

    override fun show() {
        isVisible = true
    }

    override fun hide() {
        isVisible = false
    }

    override suspend fun awaitSuperKey(): SuperKeyAuthResult {
        return kotlinx.coroutines.suspendCancellableCoroutine { continuation ->
            onResult = { result ->
                if (continuation.isActive) {
                    continuation.resumeWith(Result.success(result))
                }
                onResult = null
            }
            show()
            continuation.invokeOnCancellation {
                hide()
                onResult = null
            }
        }
    }

    internal fun submitResult(result: SuperKeyAuthResult) {
        onResult?.invoke(result)
        hide()
    }
}

@Composable
fun rememberSuperKeyDialog(): SuperKeyDialogHandle {
    return remember { SuperKeyDialogState() }
}

@OptIn(ExperimentalComposeUiApi::class)
@Composable
fun SuperKeyDialog(
    state: SuperKeyDialogHandle,
    title: String = stringResource(id = R.string.superkey_auth_title),
    subtitle: String = stringResource(id = R.string.superkey_auth_subtitle),
    onAuthenticate: suspend (String) -> Boolean,
    onResult: (SuperKeyAuthResult) -> Unit = {}
) {
    if (state !is SuperKeyDialogState) return

    val context = LocalContext.current

    val authFailedMessage = stringResource(id = R.string.superkey_auth_failed)
    val emptyKeyMessage = stringResource(id = R.string.superkey_input_hint)

    fun checkEasterEgg(input: String): Boolean {
        if (input.equals("transright", ignoreCase = true) && !ThemeConfig.isTransPrideUnlocked) {
            ThemeManager.unlockTransPride(context)
            ThemeConfig.currentTheme = ThemeColors.TransPride
            ThemeManager.saveThemeColors(context, "trans")
            Toast.makeText(context, "🏳️‍⚧️ Trans Rights! ✨", Toast.LENGTH_LONG).show()
            return true
        }
        return false
    }

    if (state.isVisible) {
        // SuperKey is user input that must survive process death / config change.
        var superKeyInput by rememberSaveable { mutableStateOf("") }
        var isPasswordVisible by rememberSaveable { mutableStateOf(false) }
        var isLoading by remember { mutableStateOf(false) }
        var errorMessage by remember { mutableStateOf<String?>(null) }
        val focusRequester = remember { FocusRequester() }
        val keyboardController = LocalSoftwareKeyboardController.current

        LaunchedEffect(Unit) {
            delay(100)
            focusRequester.requestFocus()
        }

        Dialog(
            onDismissRequest = {
                if (!isLoading) {
                    val result = SuperKeyAuthResult.Canceled
                    state.submitResult(result)
                    onResult(result)
                }
            },
            properties = DialogProperties(
                dismissOnBackPress = !isLoading,
                dismissOnClickOutside = !isLoading,
                usePlatformDefaultWidth = false
            )
        ) {
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickHapticFeedback()
                    .padding(horizontal = 24.dp),
                shape = MaterialTheme.shapes.extraLarge,
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surfaceContainerHigh
                ),
                elevation = CardDefaults.cardElevation(
                    defaultElevation = 0.dp
                )
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    // Icon
                    Icon(
                        imageVector = Icons.Default.Key,
                        contentDescription = null,
                        modifier = Modifier.size(48.dp),
                        tint = MaterialTheme.colorScheme.primary
                    )

                    Spacer(modifier = Modifier.height(16.dp))

                    // Title
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.Normal),
                        color = MaterialTheme.colorScheme.onSurface
                    )

                    Spacer(modifier = Modifier.height(8.dp))

                    // Subtitle
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )

                    Spacer(modifier = Modifier.height(24.dp))

                    // Password input field
                    OutlinedTextField(
                        value = superKeyInput,
                        onValueChange = { newValue ->
                            superKeyInput = newValue
                            errorMessage = null
                            checkEasterEgg(newValue)
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .focusRequester(focusRequester),
                        label = { Text(stringResource(id = R.string.superkey_input_hint)) },
                        placeholder = { Text(stringResource(id = R.string.superkey_input_placeholder)) },
                        visualTransformation = if (isPasswordVisible) {
                            VisualTransformation.None
                        } else {
                            PasswordVisualTransformation()
                        },
                        keyboardOptions = KeyboardOptions(
                            keyboardType = KeyboardType.Password,
                            imeAction = ImeAction.Done
                        ),
                        keyboardActions = KeyboardActions(
                            onDone = {
                                keyboardController?.hide()
                            }
                        ),
                        trailingIcon = {
                            IconButton(onClick = { isPasswordVisible = !isPasswordVisible }) {
                                Icon(
                                    imageVector = if (isPasswordVisible) {
                                        Icons.Default.VisibilityOff
                                    } else {
                                        Icons.Default.Visibility
                                    },
                                    contentDescription = if (isPasswordVisible) {
                                        stringResource(id = R.string.hide_password)
                                    } else {
                                        stringResource(id = R.string.show_password)
                                    }
                                )
                            }
                        },
                        singleLine = true,
                        isError = errorMessage != null,
                        supportingText = errorMessage?.let { { Text(it) } },
                        enabled = !isLoading,
                        shape = RoundedCornerShape(12.dp)
                    )

                    Spacer(modifier = Modifier.height(24.dp))

                    // Action buttons
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.End
                    ) {
                        TextButton(
                            onClick = {
                                val result = SuperKeyAuthResult.Canceled
                                state.submitResult(result)
                                onResult(result)
                            },
                            enabled = !isLoading
                        ) {
                            Text(stringResource(id = android.R.string.cancel))
                        }

                        Spacer(modifier = Modifier.width(8.dp))

                        Button(
                            onClick = {
                                if (superKeyInput.isBlank()) {
                                    errorMessage = emptyKeyMessage
                                    return@Button
                                }

                                isLoading = true
                                CoroutineScope(Dispatchers.Main).launch {
                                    try {
                                        val success = onAuthenticate(superKeyInput)
                                        if (success) {
                                            val result = SuperKeyAuthResult.Success(superKeyInput)
                                            state.submitResult(result)
                                            onResult(result)
                                        } else {
                                            errorMessage = authFailedMessage
                                            isLoading = false
                                        }
                                    } catch (e: Exception) {
                                        errorMessage = e.message ?: authFailedMessage
                                        isLoading = false
                                    }
                                }
                            },
                            enabled = !isLoading && superKeyInput.isNotBlank()
                        ) {
                            if (isLoading) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(16.dp),
                                    strokeWidth = 2.dp,
                                    color = MaterialTheme.colorScheme.onPrimary
                                )
                                Spacer(modifier = Modifier.width(8.dp))
                            }
                            Text(stringResource(id = R.string.superkey_authenticate))
                        }
                    }
                }
            }
        }
    }
}
