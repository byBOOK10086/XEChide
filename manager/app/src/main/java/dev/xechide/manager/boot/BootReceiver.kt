// SPDX-License-Identifier: GPL-3.0-only
package dev.xechide.manager.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import dev.xechide.manager.data.ApplyRecord
import dev.xechide.manager.data.SecureStore
import dev.xechide.manager.data.StatusStore
import dev.xechide.manager.root.XeChideBridge
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Re-applies the last successfully saved configuration once the user-unlocked
 * boot completes. The kernel deliberately starts with zero rules, so a missing
 * or undecryptable config leaves everything visible (fail-open).
 *
 * A root manager may refuse background `su`; the result is stored in
 * StatusStore so the next UI launch can say what happened.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return

        val appContext = context.applicationContext
        val pendingResult = goAsync()
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            try {
                val config = SecureStore(appContext).load()
                if (config == null || config.hiddenApps.isEmpty()) {
                    return@launch
                }

                val outcome = XeChideBridge.apply(
                    config = config,
                    timeoutSeconds = BOOT_TIMEOUT_SECONDS,
                )
                StatusStore(appContext).save(
                    ApplyRecord(
                        success = outcome.success,
                        ruleCount = outcome.ruleCount,
                        exitCode = outcome.exitCode,
                        stdout = outcome.stdout,
                        stderr = outcome.stderr,
                        timestamp = System.currentTimeMillis(),
                    ),
                )
            } catch (error: Exception) {
                Log.e(TAG, "could not re-apply the saved configuration", error)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private companion object {
        const val TAG = "XeChideBoot"
        const val BOOT_TIMEOUT_SECONDS = 8L
    }
}
