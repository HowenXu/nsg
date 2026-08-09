package info.skyblond.nsp.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import info.skyblond.nsp.MainActivity
import info.skyblond.nsp.R
import info.skyblond.nsp.ui.L10n

/** Builds and updates the foreground-service notification. */
object NotificationHelper {
    const val CHANNEL_ID = "camera_connection_channel"
    const val NOTIFICATION_ID = 1
    const val ACTION_DISCONNECT = "info.skyblond.nsp.DISCONNECT"

    fun createChannel(context: Context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                L10n.t("相机连接", "Camera connection"),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = L10n.t("保持与尼康相机的连接", "Keeps the connection to your Nikon camera")
            }
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    fun build(context: Context, statusText: String): Notification {
        val contentIntent = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_IMMUTABLE
        )
        val disconnectIntent = PendingIntent.getService(
            context,
            0,
            Intent(context, CameraConnectionService::class.java).apply {
                action = ACTION_DISCONNECT
            },
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(context, CHANNEL_ID)
            .setContentTitle(L10n.t("尼康智能GPS", "Nikon Smart GPS"))
            .setContentText(statusText)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentIntent(contentIntent)
            .addAction(R.drawable.ic_launcher_foreground, L10n.t("断开", "Disconnect"), disconnectIntent)
            .setOngoing(true)
            .build()
    }

    fun update(context: Context, statusText: String) {
        val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIFICATION_ID, build(context, statusText))
    }
}
