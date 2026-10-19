package dev.xechide.manager.root

import dev.xechide.manager.data.ConfigCodec
import dev.xechide.manager.data.ManagerConfig
import dev.xechide.manager.data.resolveRules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * The only code that speaks to the stateless C helper.
 *
 * `set` receives exactly one packed struct on stdin; `stats` prints the kernel
 * counters. Both run through [RootShell] on Dispatchers.IO.
 */
object XeChideBridge {
    private const val APPLY_TIMEOUT_SECONDS = 30L
    private const val STATS_TIMEOUT_SECONDS = 20L

    data class ApplyResult(
        val success: Boolean,
        val ruleCount: Int,
        val exitCode: Int,
        val stdout: String,
        val stderr: String,
    )

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

        val command = "${RootShell.shellQuote(HelperInstaller.HELPER_PATH)} set"
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
        )
    }

    suspend fun stats(): RootShell.Result = withContext(Dispatchers.IO) {
        RootShell.run(
            cmd = "${RootShell.shellQuote(HelperInstaller.HELPER_PATH)} stats",
            timeoutSeconds = STATS_TIMEOUT_SECONDS,
        )
    }
}
