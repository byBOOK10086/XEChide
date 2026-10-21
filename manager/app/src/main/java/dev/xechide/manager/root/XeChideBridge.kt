// SPDX-License-Identifier: GPL-3.0-only
package dev.xechide.manager.root

import dev.xechide.manager.data.ConfigCodec
import dev.xechide.manager.data.ManagerConfig
import dev.xechide.manager.data.resolveRules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * The only code that speaks to the stateless C helper.
 *
 * `apply` writes the module config file and, when a patched KernelSU driver is
 * present, applies the same bytes to the kernel. The two delivery forms share
 * one packed ABI, so the manager never has to choose between them: it reports
 * which forms are available and keeps both in sync.
 */
object XeChideBridge {
    private const val APPLY_TIMEOUT_SECONDS = 30L
    private const val STATUS_TIMEOUT_SECONDS = 20L
    private const val STATS_TIMEOUT_SECONDS = 20L

    data class ApplyResult(
        val success: Boolean,
        val ruleCount: Int,
        val exitCode: Int,
        val stdout: String,
        val stderr: String,
        val fileWritten: Boolean = false,
        val kernelApplied: Boolean = false,
    )

    data class DeviceStatus(
        val moduleInstalled: Boolean,
        val moduleSo: Boolean,
        val configPresent: Boolean,
        val configBytes: Long,
        val kernelDriver: Boolean,
        val version: String?,
    ) {
        val moduleActive: Boolean get() = moduleInstalled && moduleSo
    }

    suspend fun apply(
        config: ManagerConfig,
        timeoutSeconds: Long = APPLY_TIMEOUT_SECONDS,
    ): ApplyResult = withContext(Dispatchers.IO) {
        val ruleCount = config.resolveRules().size
        val payload = try {
            ConfigCodec.encodePacked(config)
        } catch (error: Exception) {
            return@withContext ApplyResult(
                success = false,
                ruleCount = ruleCount,
                exitCode = -1,
                stdout = "",
                stderr = "could not encode config: " +
                    (error.message ?: error.javaClass.simpleName),
            )
        }

        val command = "${RootShell.shellQuote(HelperInstaller.HELPER_PATH)} apply"
        val result = RootShell.run(
            cmd = command,
            stdin = payload,
            timeoutSeconds = timeoutSeconds,
        )
        ApplyResult(
            success = result.isSuccess,
            ruleCount = ruleCount,
            exitCode = result.exitCode,
            stdout = result.stdout,
            stderr = result.stderr,
            fileWritten = result.stdout.lineSequence().any { it.trim() == "file=ok" },
            kernelApplied = result.stdout.lineSequence().any { it.trim() == "kernel=ok" },
        )
    }

    suspend fun status(): DeviceStatus? = withContext(Dispatchers.IO) {
        val result = RootShell.run(
            cmd = "${RootShell.shellQuote(HelperInstaller.HELPER_PATH)} status",
            timeoutSeconds = STATUS_TIMEOUT_SECONDS,
        )
        if (!result.isSuccess) {
            return@withContext null
        }
        val values = HashMap<String, String>()
        for (line in result.stdout.lineSequence()) {
            val index = line.indexOf('=')
            if (index <= 0) continue
            values[line.substring(0, index).trim()] = line.substring(index + 1).trim()
        }
        if (!values.containsKey("module")) {
            return@withContext null
        }
        DeviceStatus(
            moduleInstalled = values["module"] == "1",
            moduleSo = values["module_so"] == "1",
            configPresent = values["config"] == "1",
            configBytes = values["config_bytes"]?.toLongOrNull() ?: 0L,
            kernelDriver = values["kernel"] == "1",
            version = values["version"]?.takeIf { it.isNotBlank() },
        )
    }

    suspend fun stats(): RootShell.Result = withContext(Dispatchers.IO) {
        RootShell.run(
            cmd = "${RootShell.shellQuote(HelperInstaller.HELPER_PATH)} stats",
            timeoutSeconds = STATS_TIMEOUT_SECONDS,
        )
    }
}
