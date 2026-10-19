package dev.xechide.manager.ui

import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import dev.xechide.manager.R
import dev.xechide.manager.data.AppEntry
import dev.xechide.manager.data.ApplyRecord
import dev.xechide.manager.data.AppRepository
import dev.xechide.manager.data.HiddenApp
import dev.xechide.manager.data.ManagerConfig
import dev.xechide.manager.data.SecureStore
import dev.xechide.manager.data.StatusStore
import dev.xechide.manager.data.TargetApp
import dev.xechide.manager.data.XECHIDE_MAX_RULES
import dev.xechide.manager.data.resolveRules
import dev.xechide.manager.root.HelperInstaller
import dev.xechide.manager.root.HelperState
import dev.xechide.manager.root.XeChideBridge
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private enum class AppScreen {
    HOME,
    HIDDEN_PICKER,
    TARGET_PICKER,
    STATS,
}

private enum class BusyKind {
    APPLY,
    INSTALL_HELPER,
}

@Composable
fun ManagerApp() {
    val context = LocalContext.current
    val appContext = context.applicationContext
    val scope = rememberCoroutineScope()

    val appRepository = remember(appContext) { AppRepository(appContext) }
    val secureStore = remember(appContext) { SecureStore(appContext) }
    val statusStore = remember(appContext) { StatusStore(appContext) }
    val snackbarHostState = remember { SnackbarHostState() }

    var screen by remember { mutableStateOf(AppScreen.HOME) }
    var config by remember { mutableStateOf(ManagerConfig()) }
    var apps by remember { mutableStateOf<List<AppEntry>>(emptyList()) }
    var appsLoading by remember { mutableStateOf(true) }
    var appsError by remember { mutableStateOf<String?>(null) }
    var appsRequest by remember { mutableStateOf(0) }
    var helperState by remember { mutableStateOf(HelperState.CHECKING) }
    var lastApply by remember { mutableStateOf<ApplyRecord?>(null) }
    var busy by remember { mutableStateOf<BusyKind?>(null) }

    fun showMessage(message: String) {
        scope.launch {
            snackbarHostState.showSnackbar(message)
        }
    }

    LaunchedEffect(Unit) {
        val loadedConfig = withContext(Dispatchers.IO) {
            try {
                secureStore.load()
            } catch (_: Exception) {
                null
            }
        }
        if (loadedConfig != null) config = loadedConfig

        val loadedStatus = withContext(Dispatchers.IO) {
            try {
                statusStore.load()
            } catch (_: Exception) {
                null
            }
        }
        lastApply = loadedStatus
    }

    LaunchedEffect(appsRequest) {
        appsLoading = true
        appsError = null
        try {
            apps = appRepository.loadInstalledApps()
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            appsError = appContext.getString(
                R.string.error_load_apps,
                error.message ?: error.javaClass.simpleName,
            )
        } finally {
            appsLoading = false
        }
    }

    LaunchedEffect(Unit) {
        helperState = try {
            HelperInstaller.check()
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            HelperState.UNKNOWN
        }
    }

    fun applyConfiguration() {
        if (busy != null) return

        val snapshot = config
        if (snapshot.hiddenApps.isEmpty()) {
            showMessage(appContext.getString(R.string.error_no_hidden))
            return
        }
        if (!snapshot.hideAllTargets && snapshot.targetApps.isEmpty()) {
            showMessage(appContext.getString(R.string.error_no_targets))
            return
        }

        val rules = snapshot.resolveRules()
        if (rules.isEmpty()) {
            showMessage(appContext.getString(R.string.error_no_rules))
            return
        }
        if (rules.size > XECHIDE_MAX_RULES) {
            showMessage(
                appContext.getString(
                    R.string.error_too_many_rules,
                    rules.size,
                    XECHIDE_MAX_RULES,
                ),
            )
            return
        }

        busy = BusyKind.APPLY
        scope.launch {
            try {
                val outcome = XeChideBridge.apply(snapshot)

                var saveFailure: String? = null
                if (outcome.success) {
                    try {
                        withContext(Dispatchers.IO) {
                            secureStore.save(snapshot)
                        }
                    } catch (error: CancellationException) {
                        throw error
                    } catch (error: Exception) {
                        saveFailure = error.message ?: error.javaClass.simpleName
                    }
                }

                val record = ApplyRecord(
                    success = outcome.success,
                    ruleCount = outcome.ruleCount,
                    exitCode = outcome.exitCode,
                    stdout = outcome.stdout,
                    stderr = outcome.stderr,
                    timestamp = System.currentTimeMillis(),
                )
                lastApply = record
                try {
                    withContext(Dispatchers.IO) {
                        statusStore.save(record)
                    }
                } catch (_: Exception) {
                    // The status display is best effort; the applied config was
                    // already saved above.
                }

                if (outcome.success) {
                    helperState = HelperState.INSTALLED
                    val persistenceError = saveFailure
                    if (persistenceError == null) {
                        showMessage(
                            appContext.getString(
                                R.string.snack_apply_success,
                                outcome.ruleCount,
                            ),
                        )
                    } else {
                        showMessage(
                            appContext.getString(R.string.error_save_config, persistenceError),
                        )
                    }
                } else {
                    showMessage(
                        appContext.getString(R.string.snack_apply_failed, outcome.exitCode),
                    )
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                showMessage(
                    appContext.getString(
                        R.string.error_operation,
                        error.message ?: error.javaClass.simpleName,
                    ),
                )
            } finally {
                busy = null
            }
        }
    }

    fun installHelper() {
        if (busy != null) return

        busy = BusyKind.INSTALL_HELPER
        scope.launch {
            try {
                val result = HelperInstaller.install(appContext)
                if (result.success) {
                    helperState = HelperState.INSTALLED
                }
                showMessage(result.message)
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                showMessage(
                    appContext.getString(
                        R.string.error_operation,
                        error.message ?: error.javaClass.simpleName,
                    ),
                )
            } finally {
                busy = null
            }
        }
    }

    val busyLabel = when (busy) {
        BusyKind.APPLY -> stringResource(R.string.busy_apply)
        BusyKind.INSTALL_HELPER -> stringResource(R.string.busy_install)
        null -> null
    }

    when (screen) {
        AppScreen.HOME -> {
            HomeScreen(
                config = config,
                helperState = helperState,
                lastApply = lastApply,
                busyLabel = busyLabel,
                snackbarHostState = snackbarHostState,
                onPickHidden = { screen = AppScreen.HIDDEN_PICKER },
                onPickTargets = { screen = AppScreen.TARGET_PICKER },
                onHideAllTargetsChange = { checked ->
                    config = config.copy(hideAllTargets = checked)
                },
                onHideShellChange = { checked ->
                    config = config.copy(hideShell = checked)
                },
                onHideSelfChange = { checked ->
                    config = config.copy(hideSelf = checked)
                },
                onApply = { applyConfiguration() },
                onInstallHelper = { installHelper() },
                onOpenStats = { screen = AppScreen.STATS },
            )
        }

        AppScreen.HIDDEN_PICKER -> {
            AppPickerScreen(
                title = stringResource(R.string.pick_hidden),
                hint = stringResource(R.string.picker_hidden_hint),
                apps = apps,
                loading = appsLoading,
                loadError = appsError,
                initialSelection = remember(config.hiddenApps) {
                    config.hiddenApps.map { it.packageName }.toSet()
                },
                onRetry = { appsRequest += 1 },
                onBack = { screen = AppScreen.HOME },
                onConfirm = { selected ->
                    config = config.copy(
                        hiddenApps = selected.map { app ->
                            HiddenApp(
                                packageName = app.packageName,
                                uid = app.uid,
                                label = app.label,
                            )
                        },
                    )
                    screen = AppScreen.HOME
                },
            )
        }

        AppScreen.TARGET_PICKER -> {
            AppPickerScreen(
                title = stringResource(R.string.pick_target),
                hint = stringResource(R.string.picker_target_hint),
                apps = apps,
                loading = appsLoading,
                loadError = appsError,
                initialSelection = remember(config.targetApps) {
                    config.targetApps.map { it.packageName }.toSet()
                },
                onRetry = { appsRequest += 1 },
                onBack = { screen = AppScreen.HOME },
                onConfirm = { selected ->
                    config = config.copy(
                        targetApps = selected.map { app ->
                            TargetApp(
                                uid = app.uid,
                                packageName = app.packageName,
                                label = app.label,
                            )
                        },
                    )
                    screen = AppScreen.HOME
                },
            )
        }

        AppScreen.STATS -> {
            StatsScreen(onBack = { screen = AppScreen.HOME })
        }
    }
}
