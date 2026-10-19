package dev.xechide.manager.data

import android.content.Context
import org.json.JSONObject

/**
 * Non-secret metadata about the last `set` attempt. The rule list itself is
 * only stored encrypted in [SecureStore]; this record exists so the home screen
 * can still explain a failed background apply after the process restarted.
 */
data class ApplyRecord(
    val success: Boolean,
    val ruleCount: Int,
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
    val timestamp: Long,
)

class StatusStore(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences(
        PREFERENCES_NAME,
        Context.MODE_PRIVATE,
    )

    fun save(record: ApplyRecord) {
        val json = JSONObject()
            .put(KEY_SUCCESS, record.success)
            .put(KEY_RULE_COUNT, record.ruleCount)
            .put(KEY_EXIT_CODE, record.exitCode)
            .put(KEY_STDOUT, record.stdout)
            .put(KEY_STDERR, record.stderr)
            .put(KEY_TIMESTAMP, record.timestamp)
        // Called from Dispatchers.IO. commit() makes a BOOT_COMPLETED result
        // survive even if the receiver process is killed immediately.
        preferences.edit().putString(KEY_LAST_APPLY, json.toString()).commit()
    }

    fun load(): ApplyRecord? {
        val raw = preferences.getString(KEY_LAST_APPLY, null) ?: return null
        return try {
            val json = JSONObject(raw)
            ApplyRecord(
                success = json.getBoolean(KEY_SUCCESS),
                ruleCount = json.optInt(KEY_RULE_COUNT, 0),
                exitCode = json.optInt(KEY_EXIT_CODE, -1),
                stdout = json.optString(KEY_STDOUT, ""),
                stderr = json.optString(KEY_STDERR, ""),
                timestamp = json.optLong(KEY_TIMESTAMP, 0L),
            )
        } catch (_: Exception) {
            null
        }
    }

    private companion object {
        const val PREFERENCES_NAME = "xechide_status"
        const val KEY_LAST_APPLY = "last_apply"

        const val KEY_SUCCESS = "success"
        const val KEY_RULE_COUNT = "ruleCount"
        const val KEY_EXIT_CODE = "exitCode"
        const val KEY_STDOUT = "stdout"
        const val KEY_STDERR = "stderr"
        const val KEY_TIMESTAMP = "timestamp"
    }
}
