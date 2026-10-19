package dev.xechide.manager.root

import android.content.Context
import dev.xechide.manager.R
import java.io.FileNotFoundException
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

enum class HelperState {
    CHECKING,
    INSTALLED,
    MISSING,
    UNKNOWN,
}

data class HelperInstallResult(
    val success: Boolean,
    val message: String,
)

/**
 * Installs the prebuilt root helper out of the APK assets.
 *
 * The binary itself is not checked into this repository: CI (or the release
 * packager) builds `ctl/xechide-ctl.c` with the NDK and drops it into
 * `app/src/main/assets/xechide-ctl-arm64`. The runtime error for a missing
 * asset says so explicitly.
 */
object HelperInstaller {
    const val ASSET_NAME = "xechide-ctl-arm64"
    const val HELPER_PATH = "/data/adb/xechide/bin/xechide-ctl"
    private const val HELPER_DIR = "/data/adb/xechide/bin"
    private const val INSTALL_TIMEOUT_SECONDS = 60L
    private const val CHECK_TIMEOUT_SECONDS = 10L

    suspend fun install(context: Context): HelperInstallResult = withContext(Dispatchers.IO) {
        val appContext = context.applicationContext
        val binary = try {
            appContext.assets.open(ASSET_NAME).use { stream -> stream.readBytes() }
        } catch (_: FileNotFoundException) {
            return@withContext HelperInstallResult(
                success = false,
                message = appContext.getString(R.string.helper_asset_missing),
            )
        } catch (error: IOException) {
            return@withContext HelperInstallResult(
                success = false,
                message = appContext.getString(
                    R.string.helper_asset_read_failed,
                    error.message ?: error.javaClass.simpleName,
                ),
            )
        }

        if (binary.isEmpty()) {
            return@withContext HelperInstallResult(
                success = false,
                message = appContext.getString(R.string.helper_asset_empty),
            )
        }

        val destination = RootShell.shellQuote(HELPER_PATH)
        val directory = RootShell.shellQuote(HELPER_DIR)
        val copyCommand = "/system/bin/mkdir -p $directory && " +
            "/system/bin/cat > $destination && " +
            "/system/bin/chmod 0755 $destination"

        val copyResult = RootShell.run(
            cmd = copyCommand,
            stdin = binary,
            timeoutSeconds = INSTALL_TIMEOUT_SECONDS,
        )
        if (!copyResult.isSuccess) {
            return@withContext HelperInstallResult(
                success = false,
                message = appContext.getString(
                    R.string.helper_install_failed,
                    copyResult.exitCode,
                    detailOf(appContext, copyResult),
                ),
            )
        }

        val verifyResult = RootShell.run(
            cmd = "/system/bin/test -x $destination",
            timeoutSeconds = CHECK_TIMEOUT_SECONDS,
        )
        if (!verifyResult.isSuccess) {
            return@withContext HelperInstallResult(
                success = false,
                message = appContext.getString(
                    R.string.helper_verify_failed,
                    verifyResult.exitCode,
                    detailOf(appContext, verifyResult),
                ),
            )
        }

        HelperInstallResult(
            success = true,
            message = appContext.getString(R.string.helper_install_success),
        )
    }

    /**
     * `test -x` is intentionally only a file check. It cannot prove that the
     * kernel half is flashed, and the UI says so.
     */
    suspend fun check(): HelperState = withContext(Dispatchers.IO) {
        val result = try {
            RootShell.run(
                cmd = "/system/bin/test -x ${RootShell.shellQuote(HELPER_PATH)}",
                timeoutSeconds = CHECK_TIMEOUT_SECONDS,
            )
        } catch (_: Exception) {
            return@withContext HelperState.UNKNOWN
        }

        when {
            result.exitCode == 0 -> HelperState.INSTALLED
            result.exitCode == 1 && result.stderr.isBlank() -> HelperState.MISSING
            else -> HelperState.UNKNOWN
        }
    }

    private fun detailOf(context: Context, result: RootShell.Result): String {
        return when {
            result.stderr.isNotBlank() -> result.stderr.trim()
            result.stdout.isNotBlank() -> result.stdout.trim()
            else -> context.getString(R.string.no_output_detail)
        }
    }
}
