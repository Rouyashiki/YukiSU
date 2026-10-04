package com.anatdx.yukisu.ui.util.module

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import com.anatdx.yukisu.R
import java.io.IOException

object ModuleUtils {
    private const val TAG = "ModuleUtils"

    fun extractModuleName(context: Context, uri: Uri): String {
        if (uri == Uri.EMPTY) {
            Log.e(TAG, "The supplied URI is empty")
            return context.getString(R.string.unknown_module)
        }

        return try {
            Log.d(TAG, "Start extracting module names from URIs: $uri")

            val fileName = uri.lastPathSegment?.let { path ->
                val lastSlash = path.lastIndexOf('/')
                if (lastSlash != -1 && lastSlash < path.length - 1) {
                    path.substring(lastSlash + 1)
                } else {
                    path
                }
            }?.removeSuffix(".zip") ?: context.getString(R.string.unknown_module)

            val formattedFileName = fileName.replace(Regex("[^a-zA-Z0-9\\s\\-_.@()\\u4e00-\\u9fa5]"), "").trim()

            try {
                val moduleName = ModuleArchive.readProperties(context, uri)["name"]
                    ?.replace(Regex("[^a-zA-Z0-9\\s\\-_.@()\\u4e00-\\u9fa5]"), "")?.trim()
                    ?: formattedFileName
                Log.d(TAG, "Successfully extracted module name: $moduleName")
                moduleName
            } catch (e: IOException) {
                Log.e(TAG, "Error reading ZIP file: ${e.message}")
                formattedFileName
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception when extracting module name: ${e.message}")
            context.getString(R.string.unknown_module)
        }
    }

    // 验证URI是否有效并可访问
    fun isUriAccessible(context: Context, uri: Uri): Boolean {
        if (uri == Uri.EMPTY) return false

        return try {
            val inputStream = context.contentResolver.openInputStream(uri)
            inputStream?.close()
            inputStream != null
        } catch (e: Exception) {
            Log.e(TAG, "The URI is inaccessible: $uri, Error: ${e.message}")
            false
        }
    }

    fun takePersistableUriPermission(context: Context, uri: Uri) {
        try {
            val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
            context.contentResolver.takePersistableUriPermission(uri, flags)
            Log.d(TAG, "Persistent permissions for URIs have been obtained: $uri")
        } catch (e: Exception) {
            Log.e(TAG, "Unable to get persistent permissions on URIs: $uri, Error: ${e.message}")
        }
    }

    fun extractModuleId(context: Context, uri: Uri): String? {
        if (uri == Uri.EMPTY) {
            return null
        }

        return try {
            ModuleArchive.readProperties(context, uri)["id"]
        } catch (e: Exception) {
            Log.e(TAG, "Error extracting module ID: ${e.message}", e)
            null
        }
    }
}