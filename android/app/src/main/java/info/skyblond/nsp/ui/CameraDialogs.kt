package info.skyblond.nsp.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import info.skyblond.nsp.data.DiscoveredCamera
import info.skyblond.nsp.data.PairedCamera

@Composable
fun DiscoveredCameraDialog(
    cameras: List<DiscoveredCamera>,
    onSelect: (DiscoveredCamera) -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(L10n.t("选择要配对的相机", "Select camera to pair")) },
        text = {
            if (cameras.isEmpty()) {
                Text(L10n.t("未发现相机，请确认相机已进入配对模式", "No camera found. Make sure the camera is in pairing mode."))
            } else {
                LazyColumn {
                    items(cameras, key = { it.address }) { camera ->
                        Card(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 4.dp),
                            onClick = { onSelect(camera) }
                        ) {
                            Text(
                                text = buildString {
                                    append(camera.name)
                                    append("\n")
                                    append(camera.address)
                                    if (camera.manufacturerData != null) {
                                        append("\n[" + L10n.t("已有配对记录，选择后可直接切换连接", "Already paired; select to switch") + "]")
                                    }
                                },
                                style = MaterialTheme.typography.bodyMedium,
                                modifier = Modifier.padding(16.dp)
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(L10n.t("取消", "Cancel"))
            }
        }
    )
}

@Composable
fun SavedCameraDialog(
    cameras: List<PairedCamera>,
    onSelect: (PairedCamera) -> Unit,
    onAutoExtract: (PairedCamera) -> Unit,
    onSetDefault: (PairedCamera) -> Unit,
    onDelete: (PairedCamera) -> Unit,
    defaultCameraName: String?,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(L10n.t("选择已保存的相机", "Select a saved camera")) },
        text = {
            if (cameras.isEmpty()) {
                Text(L10n.t("暂无已保存的相机，请先配对", "No saved cameras yet. Pair one first."))
            } else {
                LazyColumn {
                    if (cameras.size > 1) {
                        item {
                            Text(
                                text = L10n.t(
                                    "已保存 ${cameras.size} 台相机。可点「设为默认」选择启动时自动连接的相机，点「删除」移除。",
                                    "${cameras.size} camera(s) saved. Use \"Set Default\" to pick the auto-connect camera at startup; \"Delete\" removes it."
                                ),
                                style = MaterialTheme.typography.bodySmall
                            )
                        }
                    }
                    items(cameras, key = { it.address }) { camera ->
                        Card(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 4.dp)
                        ) {
                            val isDefault = defaultCameraName == camera.name
                            Column(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(start = 12.dp, top = 16.dp, end = 12.dp, bottom = 8.dp),
                                verticalArrangement = Arrangement.spacedBy(2.dp),
                                horizontalAlignment = Alignment.CenterHorizontally
                            ) {
                                Text(
                                    text = "${camera.name}\n${camera.address}",
                                    style = MaterialTheme.typography.bodyMedium,
                                    textAlign = TextAlign.Center
                                )
                                if (isDefault) {
                                        Text(
                                            text = L10n.t("✓ 启动时默认连接", "✓ Default at startup"),
                                        style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.primary
                                    )
                                }
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.Center
                                ) {
                                    TextButton(onClick = { onSelect(camera) }) {
                                        Text(L10n.t("连接", "Connect"))
                                    }
                                    TextButton(onClick = { onAutoExtract(camera) }) {
                                        Text(L10n.t("自动提取标识", "Auto Extract ID"))
                                    }
                                    if (cameras.size > 1) {
                                        TextButton(onClick = { onSetDefault(camera) }) {
                                            Text(if (isDefault) L10n.t("取消默认", "Unset Default") else L10n.t("设为默认", "Set Default"))
                                        }
                                    }
                                    TextButton(onClick = { onDelete(camera) }) {
                                        Text(L10n.t("删除", "Delete"))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(L10n.t("取消", "Cancel"))
            }
        }
    )
}
