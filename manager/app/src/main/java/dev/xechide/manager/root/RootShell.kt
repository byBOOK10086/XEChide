package dev.xechide.manager.root

import android.os.Looper
import java.io.IOException
import java.io.InputStream
import java.io.InputStreamReader
import java.util.concurrent.Callable
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.ThreadFactory
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import kotlin.math.max

/**
 * A deliberately tiny root runner. No libsu, no daemon, no persistence:
 * every call starts `su -c <cmd>`, optionally writes bytes to its stdin, drains
 * both output streams concurrently, and enforces a timeout.
 *
 * `run` is synchronous and must be called from Dispatchers.IO (or another
 * background thread). The main-thread check below is intentional so a future
 * caller cannot accidentally freeze the UI.
 */
object RootShell {
    private const val DEFAULT_TIMEOUT_SECONDS = 30L
    private const val STREAM_TIMEOUT_SECONDS = 2L

    data class Result(
        val exitCode: Int,
        val stdout: String,
        val stderr: String,
    ) {
        val isSuccess: Boolean get() = exitCode == 0
    }

    private val streamExecutor: ExecutorService = Executors.newCachedThreadPool(
        ThreadFactory { runnable ->
            Thread(runnable, "xechide-root-stream").apply { isDaemon = true }
        },
    )

    fun run(
        cmd: String,
        stdin: ByteArray? = null,
        timeoutSeconds: Long = DEFAULT_TIMEOUT_SECONDS,
    ): Result {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "RootShell.run must be called from a background thread"
        }
        require(cmd.isNotBlank()) { "empty root command" }

        val process = try {
            ProcessBuilder("su", "-c", cmd)
                .redirectErrorStream(false)
                .start()
        } catch (error: IOException) {
            return Result(
                exitCode = -1,
                stdout = "",
                stderr = "could not start su: ${error.message ?: error.javaClass.simpleName}",
            )
        }

        val stdoutFuture: Future<String> = streamExecutor.submit(
            Callable { readAll(process.inputStream) },
        )
        val stderrFuture: Future<String> = streamExecutor.submit(
            Callable { readAll(process.errorStream) },
        )
        val stdinFuture: Future<String?> = streamExecutor.submit(
            Callable { writeStdin(process, stdin) },
        )

        val effectiveTimeout = max(1L, timeoutSeconds)
        var timedOut = false
        val finished = try {
            process.waitFor(effectiveTimeout, TimeUnit.SECONDS)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        }

        if (!finished) {
            timedOut = true
            process.destroyForcibly()
            try {
                process.waitFor(STREAM_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }

        val stdout = await(stdoutFuture) ?: ""
        val stderrText = await(stderrFuture) ?: ""
        val stdinError = await(stdinFuture)

        val exitCode = if (finished && !process.isAlive) {
            try {
                process.exitValue()
            } catch (_: IllegalThreadStateException) {
                -1
            }
        } else {
            -1
        }

        val stderr = buildString {
            append(stderrText)
            if (!stdinError.isNullOrBlank()) {
                if (isNotEmpty() && !endsWith("\n")) append('\n')
                append("stdin: ").append(stdinError)
            }
            if (timedOut) {
                if (isNotEmpty() && !endsWith("\n")) append('\n')
                append("su command timed out after ")
                    .append(effectiveTimeout)
                    .append("s and was killed")
            }
        }

        return Result(exitCode = exitCode, stdout = stdout, stderr = stderr)
    }

    /** Quote one argument for the POSIX shell that `su -c` will start. */
    fun shellQuote(value: String): String = "'" + value.replace("'", "'\\''") + "'"

    private fun writeStdin(process: Process, stdin: ByteArray?): String? {
        return try {
            process.outputStream.use { output ->
                if (stdin != null && stdin.isNotEmpty()) {
                    output.write(stdin)
                }
                output.flush()
            }
            null
        } catch (error: IOException) {
            "could not write stdin: ${error.message ?: error.javaClass.simpleName}"
        }
    }

    private fun readAll(stream: InputStream): String {
        return try {
            InputStreamReader(stream, Charsets.UTF_8).use { reader ->
                reader.readText()
            }
        } catch (_: IOException) {
            ""
        }
    }

    private fun <T> await(future: Future<T>): T? {
        return try {
            future.get(STREAM_TIMEOUT_SECONDS, TimeUnit.SECONDS)
        } catch (_: TimeoutException) {
            future.cancel(true)
            null
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            future.cancel(true)
            null
        } catch (_: Exception) {
            null
        }
    }
}
