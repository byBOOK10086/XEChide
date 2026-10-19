package dev.xechide.manager.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import dev.xechide.manager.R
import dev.xechide.manager.data.ApplyRecord
import dev.xechide.manager.data.ManagerConfig
import dev.xechide.manager.data.XECHIDE_MAX_RULES
import dev.xechide.manager.data.resolveRules
import dev.xechide.manager.root.HelperState
import java.text.DateFormat
import java.util.Date

@Composable
fun HomeScreen(
    config: ManagerConfig,
    helperState: HelperState,
    lastApply: ApplyRecord?,
    busyLabel: String?,
    snackbarHostState: SnackbarHostState,
    onPickHidden: () -> Unit,
    onPickTargets: () -> Unit,
    onHideAllTargetsChange: (Boolean) -> Unit,
    onHideShellChange: (Boolean) -> Unit,
    onHideSelfChange: (Boolean) -> Unit,
    onApply: () -> Unit,
    onInstallHelper: () -> Unit,
    onOpenStats: () -> Unit,
) {
    val isBusy = busyLabel != null
    val ruleCount = config.resolveRules().size
    val overLimit = ruleCount > XECHIDE_MAX_RULES

    Scaffold(
        topBar = {
            ScreenHeader(
                title = stringResource(R.string.app_name),
                subtitle = stringResource(R.string.home_subtitle),
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
        containerColor = MaterialTheme.colorScheme.background,
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            StatusCard(
                ruleCount = ruleCount,
                overLimit = overLimit,
                helperState = helperState,
                lastApply = lastApply,
            )

            Text(
                text = stringResource(R.string.hide_scope_title),
                style = MaterialTheme.typography.titleMedium,
            )

            Card(modifier = Modifier.fillMaxWidth()) {
                Column {
                    ToggleRow(
                        title = stringResource(R.string.toggle_all_non_root),
                        description = stringResource(R.string.toggle_all_non_root_desc),
                        checked = config.hideAllTargets,
                        enabled = !isBusy,
                        onCheckedChange = onHideAllTargetsChange,
                    )
                    ToggleRow(
                        title = stringResource(R.string.toggle_hide_shell),
                        checked = config.hideShell,
                        enabled = !isBusy,
                        onCheckedChange = onHideShellChange,
                    )
                    ToggleRow(
                        title = stringResource(R.string.toggle_hide_self),
                        description = stringResource(R.string.toggle_hide_self_desc),
                        checked = config.hideSelf,
                        enabled = !isBusy,
                        onCheckedChange = onHideSelfChange,
                    )
                }
            }

            OutlinedButton(
                onClick = onPickHidden,
                modifier = Modifier.fillMaxWidth(),
                enabled = !isBusy,
            ) {
                Text(stringResource(R.string.pick_hidden_count, config.hiddenApps.size))
            }

            OutlinedButton(
                onClick = onPickTargets,
                modifier = Modifier.fillMaxWidth(),
                enabled = !isBusy && !config.hideAllTargets,
            ) {
                Text(stringResource(R.string.pick_target_count, config.targetApps.size))
            }

            if (config.hideAllTargets) {
                Text(
                    text = stringResource(R.string.target_disabled_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp),
                )
            }

            if (!config.hideAllTargets && config.targetApps.isEmpty() && config.hiddenApps.isNotEmpty()) {
                Text(
                    text = stringResource(R.string.target_missing_warning),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 4.dp),
                )
            }

            if (overLimit) {
                Text(
                    text = stringResource(
                        R.string.error_too_many_rules,
                        ruleCount,
                        XECHIDE_MAX_RULES,
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 4.dp),
                )
            }

            if (busyLabel != null) {
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    ),
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        Text(
                            text = busyLabel,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    }
                }
            }

            Button(
                onClick = onApply,
                modifier = Modifier.fillMaxWidth(),
                enabled = !isBusy,
            ) {
                Text(stringResource(R.string.apply_config))
            }

            OutlinedButton(
                onClick = onInstallHelper,
                modifier = Modifier.fillMaxWidth(),
                enabled = !isBusy,
            ) {
                Text(stringResource(R.string.install_helper))
            }

            TextButton(
                onClick = onOpenStats,
                modifier = Modifier.fillMaxWidth(),
                enabled = !isBusy,
            ) {
                Text(stringResource(R.string.view_stats))
            }

            Text(
                text = stringResource(R.string.home_footer),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 4.dp, vertical = 8.dp),
            )
        }
    }
}

@Composable
private fun StatusCard(
    ruleCount: Int,
    overLimit: Boolean,
    helperState: HelperState,
    lastApply: ApplyRecord?,
) {
    val helperLabel = when (helperState) {
        HelperState.CHECKING -> stringResource(R.string.helper_state_checking)
        HelperState.INSTALLED -> stringResource(R.string.helper_state_installed)
        HelperState.MISSING -> stringResource(R.string.helper_state_missing)
        HelperState.UNKNOWN -> stringResource(R.string.helper_state_unknown)
    }
    val helperColor = when (helperState) {
        HelperState.CHECKING -> MaterialTheme.colorScheme.onSurfaceVariant
        HelperState.INSTALLED -> MaterialTheme.colorScheme.primary
        HelperState.MISSING -> MaterialTheme.colorScheme.error
        HelperState.UNKNOWN -> MaterialTheme.colorScheme.tertiary
    }

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                text = stringResource(R.string.status_title),
                style = MaterialTheme.typography.titleMedium,
            )

            StatusRow(
                label = stringResource(R.string.status_helper_label),
                value = helperLabel,
                valueColor = helperColor,
            )
            StatusRow(
                label = stringResource(R.string.status_rules_label),
                value = stringResource(R.string.rules_count, ruleCount, XECHIDE_MAX_RULES),
                valueColor = if (overLimit) {
                    MaterialTheme.colorScheme.error
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
            )

            if (helperState == HelperState.MISSING) {
                Text(
                    text = stringResource(R.string.helper_missing_warning),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            Spacer(Modifier.height(2.dp))
            Text(
                text = stringResource(R.string.status_last_result_label),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            if (lastApply == null) {
                Text(
                    text = stringResource(R.string.last_never_applied),
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else {
                val time = DateFormat.getDateTimeInstance(
                    DateFormat.SHORT,
                    DateFormat.SHORT,
                ).format(Date(lastApply.timestamp))

                if (lastApply.success) {
                    Text(
                        text = stringResource(
                            R.string.last_apply_success,
                            time,
                            lastApply.ruleCount,
                        ),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.primary,
                    )
                } else {
                    Text(
                        text = stringResource(
                            R.string.last_apply_failure,
                            time,
                            lastApply.exitCode,
                        ),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                    if (lastApply.stderr.isNotBlank()) {
                        Text(
                            text = stringResource(R.string.error_output_label),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Text(
                            text = lastApply.stderr.trim(),
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun StatusRow(
    label: String,
    value: String,
    valueColor: Color,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            color = valueColor,
            textAlign = TextAlign.End,
        )
    }
}

@Composable
private fun ToggleRow(
    title: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    description: String? = null,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .toggleable(
                value = checked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = onCheckedChange,
            )
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(end = 12.dp),
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
            )
            if (description != null) {
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Switch(
            checked = checked,
            onCheckedChange = null,
            enabled = enabled,
        )
    }
}
