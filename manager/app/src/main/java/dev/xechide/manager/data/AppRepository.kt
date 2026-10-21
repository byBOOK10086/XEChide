// SPDX-License-Identifier: GPL-3.0-only
package dev.xechide.manager.data

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.core.graphics.drawable.toBitmap
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** An installed application as shown by both pickers. */
data class AppEntry(
    val packageName: String,
    val uid: Int,
    val label: String,
    val icon: ImageBitmap?,
)

class AppRepository(context: Context) {
    private val appContext = context.applicationContext

    /**
     * Loads all packages visible to this manager. The caller must not run this
     * on the main thread; the context switch is enforced here as well.
     *
     * uid 0/1000 and the manager itself are filtered: the kernel refuses the
     * former, and hiding the manager from itself is never useful.
     */
    suspend fun loadInstalledApps(): List<AppEntry> = withContext(Dispatchers.IO) {
        val packageManager = appContext.packageManager

        @Suppress("DEPRECATION")
        val applications = packageManager.getInstalledApplications(0)

        applications.asSequence()
            .filter { it.packageName != appContext.packageName }
            .filter { it.uid > 0 && it.uid != 1000 }
            .map { info -> toEntry(packageManager, info) }
            .sortedWith(
                compareBy<AppEntry> { it.label.lowercase(Locale.getDefault()) }
                    .thenBy { it.packageName },
            )
            .toList()
    }

    private fun toEntry(packageManager: PackageManager, info: ApplicationInfo): AppEntry {
        val label = try {
            info.loadLabel(packageManager).toString().ifBlank { info.packageName }
        } catch (_: Exception) {
            info.packageName
        }
        val icon = try {
            info.loadIcon(packageManager).toBitmap(width = 96, height = 96).asImageBitmap()
        } catch (_: Exception) {
            null
        }
        return AppEntry(
            packageName = info.packageName,
            uid = info.uid,
            label = label,
            icon = icon,
        )
    }
}
