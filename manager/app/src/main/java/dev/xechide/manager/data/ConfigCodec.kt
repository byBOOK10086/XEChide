// SPDX-License-Identifier: GPL-3.0-only
package dev.xechide.manager.data

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.json.JSONArray
import org.json.JSONObject

/**
 * The one and only bridge between the manager's data model and the packed
 * ABI. `kernel/xechide.h` is the source of truth:
 *
 *   struct xechide_rule  = 3 * u32 + u8[128] = 140 bytes
 *   struct xechide_config = 4 * u32 + 64 * 140  = 8976 bytes
 *
 * The ByteBuffer is allocated with a fresh (zero-filled) ByteArray, so rules
 * beyond `n_rules` are zero automatically and unused package-name bytes are
 * zero-padded. No trailing bytes are ever appended: `xechide-ctl set` rejects
 * input that is not exactly one struct.
 */
object ConfigCodec {
    private const val HEADER_BYTES = 16
    private const val RULE_BYTES = 140
    const val PACKED_BYTES: Int = 8976

    private const val JSON_SCHEMA = 1

    private const val KEY_SCHEMA = "schema"
    private const val KEY_HIDDEN_APPS = "hiddenApps"
    private const val KEY_TARGET_APPS = "targetApps"
    private const val KEY_HIDE_ALL_TARGETS = "hideAllTargets"
    private const val KEY_HIDE_SHELL = "hideShell"
    private const val KEY_HIDE_SELF = "hideSelf"
    private const val KEY_RULE_FLAGS = "ruleFlags"
    private const val KEY_PACKAGE_NAME = "packageName"
    private const val KEY_UID = "uid"
    private const val KEY_LABEL = "label"

    init {
        require(PACKED_BYTES == HEADER_BYTES + XECHIDE_MAX_RULES * RULE_BYTES) {
            "packed ABI size mismatch: $PACKED_BYTES"
        }
    }

    fun encodePacked(config: ManagerConfig): ByteArray {
        val rules = config.resolveRules()
        require(rules.size <= XECHIDE_MAX_RULES) {
            "too many rules: ${rules.size}, max $XECHIDE_MAX_RULES"
        }

        // ByteArray is zero-initialized by the JVM, including the 64 - n_rules
        // unused rule slots and the padding after every package name.
        val bytes = ByteArray(PACKED_BYTES)
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)

        buffer.putInt(XECHIDE_ABI_MAGIC)
        buffer.putInt(XECHIDE_ABI_VERSION)
        buffer.putInt(rules.size)
        buffer.putInt(config.wireFlags() and (ConfigFlags.HIDE_SHELL or ConfigFlags.HIDE_SELF))

        for (rule in rules) {
            require(rule.hiddenUid > 0 && rule.hiddenUid != 1000) {
                "invalid hidden uid ${rule.hiddenUid}"
            }
            require(
                rule.targetUid == XECHIDE_UID_ALL ||
                    (rule.targetUid > 0 && rule.targetUid != 1000)
            ) {
                "invalid target uid ${rule.targetUid}"
            }
            val flags = rule.flags and RuleFlags.ALL
            require(flags != 0) { "rule for ${rule.packageName} has no layers" }

            val packageBytes = encodePackageName(rule.packageName)
            buffer.putInt(rule.targetUid)
            buffer.putInt(rule.hiddenUid)
            buffer.putInt(flags)
            buffer.put(packageBytes)
            // pkg[128] is fixed-width: skip to the next rule boundary, the bytes
            // in between stay zero because the backing array was zero-filled.
            buffer.position(buffer.position() + (XECHIDE_PKG_LEN - packageBytes.size))
        }

        check(buffer.position() == HEADER_BYTES + rules.size * RULE_BYTES) {
            "packed layout drift: ${buffer.position()} bytes for ${rules.size} rules"
        }
        return bytes
    }

    fun encodeJson(config: ManagerConfig): String {
        val root = JSONObject()
        root.put(KEY_SCHEMA, JSON_SCHEMA)
        root.put(KEY_HIDE_ALL_TARGETS, config.hideAllTargets)
        root.put(KEY_HIDE_SHELL, config.hideShell)
        root.put(KEY_HIDE_SELF, config.hideSelf)
        root.put(KEY_RULE_FLAGS, config.ruleFlags and RuleFlags.ALL)

        val hidden = JSONArray()
        for (app in config.hiddenApps) {
            hidden.put(
                JSONObject()
                    .put(KEY_PACKAGE_NAME, app.packageName)
                    .put(KEY_UID, app.uid)
                    .put(KEY_LABEL, app.label),
            )
        }
        root.put(KEY_HIDDEN_APPS, hidden)

        val targets = JSONArray()
        for (app in config.targetApps) {
            targets.put(
                JSONObject()
                    .put(KEY_PACKAGE_NAME, app.packageName)
                    .put(KEY_UID, app.uid)
                    .put(KEY_LABEL, app.label),
            )
        }
        root.put(KEY_TARGET_APPS, targets)

        return root.toString()
    }

    @Throws(org.json.JSONException::class, IllegalArgumentException::class)
    fun decodeJson(raw: String): ManagerConfig {
        val root = JSONObject(raw)
        val schema = root.optInt(KEY_SCHEMA, -1)
        require(schema == JSON_SCHEMA) { "unsupported config schema $schema" }

        val hidden = readHiddenApps(root.optJSONArray(KEY_HIDDEN_APPS))
        val targets = readTargetApps(root.optJSONArray(KEY_TARGET_APPS))
        val storedFlags = root.optInt(KEY_RULE_FLAGS, RuleFlags.ALL) and RuleFlags.ALL

        return ManagerConfig(
            hiddenApps = hidden,
            targetApps = targets,
            hideAllTargets = root.optBoolean(KEY_HIDE_ALL_TARGETS, true),
            hideShell = root.optBoolean(KEY_HIDE_SHELL, false),
            hideSelf = root.optBoolean(KEY_HIDE_SELF, true),
            ruleFlags = if (storedFlags == 0) RuleFlags.ALL else storedFlags,
        )
    }

    private fun readHiddenApps(array: JSONArray?): List<HiddenApp> {
        if (array == null) return emptyList()
        val result = ArrayList<HiddenApp>(array.length())
        for (index in 0 until array.length()) {
            val item = array.optJSONObject(index) ?: continue
            val packageName = item.optString(KEY_PACKAGE_NAME, "").trim()
            val uid = item.optInt(KEY_UID, -1)
            if (packageName.isEmpty() || uid <= 0 || uid == 1000) continue
            val label = item.optString(KEY_LABEL, packageName).ifBlank { packageName }
            result.add(HiddenApp(packageName = packageName, uid = uid, label = label))
        }
        return result
    }

    private fun readTargetApps(array: JSONArray?): List<TargetApp> {
        if (array == null) return emptyList()
        val result = ArrayList<TargetApp>(array.length())
        for (index in 0 until array.length()) {
            val item = array.optJSONObject(index) ?: continue
            val packageName = item.optString(KEY_PACKAGE_NAME, "").trim()
            val uid = item.optInt(KEY_UID, -1)
            if (packageName.isEmpty() || uid <= 0 || uid == 1000) continue
            val label = item.optString(KEY_LABEL, packageName).ifBlank { packageName }
            result.add(TargetApp(uid = uid, packageName = packageName, label = label))
        }
        return result
    }

    private fun encodePackageName(packageName: String): ByteArray {
        require(packageName.isNotEmpty()) { "empty package name" }
        require(packageName.length < XECHIDE_PKG_LEN) {
            "package name is too long: $packageName"
        }
        require(packageName.all(::isAllowedPackageChar)) {
            "package name has unsupported characters: $packageName"
        }
        return packageName.toByteArray(Charsets.US_ASCII)
    }

    private fun isAllowedPackageChar(character: Char): Boolean =
        character in 'a'..'z' ||
            character in 'A'..'Z' ||
            character in '0'..'9' ||
            character == '.' ||
            character == '_'
}
