package com.example.huilangtoupingV3

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

class ScreenMirrorService : Service() {
    companion object {
        const val ACTION_START_SCREEN_MIRROR = "com.example.huilangtouping.action.START_SCREEN_MIRROR"
        const val ACTION_STOP_SCREEN_MIRROR = "com.example.huilangtouping.action.STOP_SCREEN_MIRROR"
        const val EXTRA_SCREEN_RESULT_CODE = "screen_result_code"
        const val EXTRA_SCREEN_RESULT_DATA = "screen_result_data"

        private const val CHANNEL_ID = "screen_mirror"
        private const val NOTIFICATION_ID = 27183

        @Volatile
        private var running = false

        fun isRunning(): Boolean = running
    }

    private val settingsStore by lazy { AppSettingsStore(this) }
    private lateinit var screenMirrorCoordinator: PcScreenMirrorCoordinator

    override fun onCreate() {
        super.onCreate()
        screenMirrorCoordinator = PcScreenMirrorCoordinator(
            context = this,
            onRequireForeground = { enterForeground("画面传输：正在运行") },
            onReleaseForeground = { leaveForeground() },
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_SCREEN_MIRROR -> {
                settingsStore.setScreenMirrorSummary("画面传输：服务已收到启动命令")
                enterForeground("画面传输：正在启动")
                val resultCode = intent.getIntExtra(EXTRA_SCREEN_RESULT_CODE, android.app.Activity.RESULT_CANCELED)
                val resultData = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(EXTRA_SCREEN_RESULT_DATA, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(EXTRA_SCREEN_RESULT_DATA)
                }

                if (resultCode == android.app.Activity.RESULT_OK && resultData != null) {
                    try {
                        screenMirrorCoordinator.start(resultCode, resultData)
                    } catch (t: Throwable) {
                        settingsStore.setScreenMirrorEnabled(false)
                        settingsStore.setScreenMirrorSummary(
                            buildString {
                                append("画面传输：启动失败 ")
                                append(t.javaClass.simpleName)
                                if (!t.message.isNullOrBlank()) {
                                    append(": ")
                                    append(t.message)
                                }
                            }
                        )
                        stopSelf()
                    }
                } else {
                    settingsStore.setScreenMirrorEnabled(false)
                    settingsStore.setScreenMirrorSummary("画面传输：未授权")
                    stopSelf()
                }
            }

            ACTION_STOP_SCREEN_MIRROR -> {
                screenMirrorCoordinator.stop("stop_intent")
                stopSelf()
            }

            else -> stopSelf()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        runCatching { screenMirrorCoordinator.release("service_onDestroy") }
        leaveForeground()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun enterForeground(text: String) {
        running = true
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    "Screen Mirror",
                    NotificationManager.IMPORTANCE_LOW,
                )
            )
        }

        val notification: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_warning)
            .setContentTitle("HuiLangTouPing")
            .setContentText(text)
            .setOngoing(true)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun leaveForeground() {
        running = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }
}
