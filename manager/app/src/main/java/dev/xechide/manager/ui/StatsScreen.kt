// SPDX-License-Identifier: GPL-3.0-only
package dev.xechide.manager.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import dev.xechide.manager.R
import dev.xechide.manager.root.XeChideBridge
import kotlinx.coroutines.CancellationException

/**
 * Runs `xechide-ctl stats` (through su) and shows its output. The screen never
 * talks to the kernel itself; it only displays the helper's stdout/stderr.
 */
@Composable
fun StatsScreen(onBack: () -> Unit) {
    BackHandler(onBack = onBack)

    val context = LocalContext.current
    var request by remember { mutableStateOf(0) }
    var loading by remember { mutableStateOf(true) }
    var output by remember { mutableStateOf("") }
    var errorText by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(request) {
        loading = true
        output = ""
        errorText = null
        try {
            val result = XeChideBridge.stats()
            if (result.isSuccess) {
                output = result.stdout.ifBlank {
                    context.getString(R.string.stats_empty_output)
                }
            } else {
                val detail = when {
                    result.stderr.isNotBlank() -> result.stderr
                    result.stdout.isNotBlank() -> result.stdout
                    else -> context.getString(R.string.stats_no_detail)
                }.trim()
                errorText = context.getString(R.string.stats_failed, result.exitCode, detail)
            }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            errorText = context.getString(
                R.string.error_operation,
                error.message ?: error.javaClass.simpleName,
            )
        } finally {
            loading = false
        }
    }

    Scaffold(
        topBar = {
            ScreenHeader(
                title = stringResource(R.string.stats_title),
                onBack = onBack,
                actions = {
                    TextButton(
                        onClick = {
                            if (!loading) {
                                loading = true
                                request += 1
                            }
                        },
                        enabled = !loading,
                    ) {
                        Text(stringResource(R.string.action_refresh))
                    }
                },
            )
        },
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
            Text(
                text = stringResource(R.string.stats_description),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            if (loading) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp))
                    Spacer(Modifier.size(12.dp))
                    Text(
                        text = stringResource(R.string.stats_loading),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }

            if (errorText != null) {
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer,
                        contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    ),
                ) {
                    SelectionContainer {
                        Text(
                            text = errorText.orEmpty(),
                            modifier = Modifier.padding(16.dp),
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }

            if (!loading && errorText == null && output.isNotBlank()) {
                Card(modifier = Modifier.fillMaxWidth()) {
                    SelectionContainer {
                        Text(
                            text = output,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }

            Spacer(Modifier.height(8.dp))
        }
    }
}
