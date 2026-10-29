package com.padscreen.client

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

class ScreenService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == STOP) { stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); return START_NOT_STICKY }
        return try {
            val preferences = getSharedPreferences("screen-service", MODE_PRIVATE)
            val port = intent?.getIntExtra("port", Protocol.DEFAULT_PORT)
                ?.takeIf { it in 1..65535 } ?: preferences.getInt("port", Protocol.DEFAULT_PORT)
            preferences.edit().putInt("port", port).apply()
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "PadScreen 连接", NotificationManager.IMPORTANCE_LOW))
            val open = Intent(this, MainActivity::class.java).putExtra("foreground", true).putExtra("port", port)
            val show = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            val stop = PendingIntent.getService(this, 1, Intent(this, ScreenService::class.java).setAction(STOP), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            val notification = Notification.Builder(this, CHANNEL)
                .setContentTitle("PadScreen 保活已开启")
                .setContentText("返回 App 显示副屏；断线会自动重试")
                .setSmallIcon(R.drawable.ic_screen).setContentIntent(show).setOngoing(true)
                .addAction(Notification.Action.Builder(null, "关闭保活", stop).build()).build()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            else startForeground(1, notification)
            // TODO(design): retain Activity ownership of socket/renderers. A destroyed
            // Surface stops the connection; returning to Activity reconnects safely.
            // This optional service keeps the process, never an invalid Surface, alive.
            START_STICKY
        } catch (error: RuntimeException) {
            Log.e(TAG, "Foreground service unavailable; Activity mode remains usable", error)
            stopSelf()
            START_NOT_STICKY
        }
    }

    companion object {
        private const val TAG = "PadScreen"
        private const val CHANNEL = "padscreen-connection"
        private const val STOP = "com.padscreen.client.STOP_FOREGROUND"
        fun startIfRequested(activity: Activity) {
            val service = Intent(activity, ScreenService::class.java)
            try {
                if (!activity.intent.getBooleanExtra("foreground", false)) { activity.stopService(service); return }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                    activity.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 51)
                service.putExtra("port", activity.intent.getIntExtra("port", Protocol.DEFAULT_PORT))
                activity.startForegroundService(service)
            } catch (error: RuntimeException) {
                Log.e(TAG, "Cannot start optional foreground service; using Activity mode", error)
            }
        }
    }
}
