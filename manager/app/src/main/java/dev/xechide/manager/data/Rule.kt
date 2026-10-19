package dev.xechide.manager.data

/**
 * The userspace view of the packed ABI in `kernel/xechide.h`.
 *
 * The integer constants are duplicated here intentionally: the app must not
 * depend on generated code or on the kernel sources at build time.
 */
const val XECHIDE_ABI_MAGIC: Int = 0x58454348 // "XECH"
const val XECHIDE_ABI_VERSION: Int = 1
const val XECHIDE_MAX_RULES: Int = 64
const val XECHIDE_PKG_LEN: Int = 128

/** 0xffffffff: every non-root uid except uid 1000 (see the kernel header). */
const val XECHIDE_UID_ALL: Int = -1

object RuleFlags {
    const val PATHS: Int = 0x0001
    const val PROC: Int = 0x0002
    const val BINDER: Int = 0x0004
    const val ALL: Int = 0x0007
}

object ConfigFlags {
    const val HIDE_SHELL: Int = 0x0001
    const val HIDE_SELF: Int = 0x0002
}

data class HiddenApp(
    val packageName: String,
    val uid: Int,
    val label: String,
)

data class TargetApp(
    val uid: Int,
    val packageName: String,
    val label: String,
)

/**
 * Everything the manager owns. This is the object that is serialized to JSON,
 * encrypted with the Android Keystore and later converted to the packed ABI.
 */
data class ManagerConfig(
    val hiddenApps: List<HiddenApp> = emptyList(),
    val targetApps: List<TargetApp> = emptyList(),
    val hideAllTargets: Boolean = true,
    val hideShell: Boolean = false,
    val hideSelf: Boolean = true,
    val ruleFlags: Int = RuleFlags.ALL,
)

/** One (target uid, hidden app) pair, ready for the packed config. */
data class RuleSpec(
    val targetUid: Int,
    val hiddenUid: Int,
    val flags: Int,
    val packageName: String,
)

private fun usableUid(uid: Int): Boolean = uid > 0 && uid != 1000

/**
 * Expands the user selection into rules.
 *
 * - "all non-root" is represented by one rule with target uid 0xffffffff for
 *   each hidden app;
 * - a specific target set is deduplicated by uid (shared uids are one target);
 * - uid 0 and 1000 are never emitted, matching the kernel validation.
 *
 * The size is not clamped here: callers must reject more than
 * [XECHIDE_MAX_RULES] rules instead of silently dropping some.
 */
fun ManagerConfig.resolveRules(): List<RuleSpec> {
    val hidden = hiddenApps
        .distinctBy { it.packageName }
        .filter { it.packageName.isNotBlank() && usableUid(it.uid) }
    if (hidden.isEmpty()) return emptyList()

    val targetUids = if (hideAllTargets) {
        listOf(XECHIDE_UID_ALL)
    } else {
        targetApps
            .map { it.uid }
            .filter(::usableUid)
            .distinct()
    }
    if (targetUids.isEmpty()) return emptyList()

    val selectedFlags = ruleFlags and RuleFlags.ALL
    val flags = if (selectedFlags == 0) RuleFlags.ALL else selectedFlags

    return hidden.flatMap { app ->
        targetUids.map { targetUid ->
            RuleSpec(
                targetUid = targetUid,
                hiddenUid = app.uid,
                flags = flags,
                packageName = app.packageName,
            )
        }
    }
}

fun ManagerConfig.wireFlags(): Int {
    var flags = 0
    if (hideShell) flags = flags or ConfigFlags.HIDE_SHELL
    if (hideSelf) flags = flags or ConfigFlags.HIDE_SELF
    return flags
}
