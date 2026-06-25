package com.example.huilangtoupingV3

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import android.view.ViewGroup
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat


class MainActivity : ComponentActivity() {
    private lateinit var btnStartScreenMirror: Button
    private lateinit var btnStopScreenMirror: Button
    private lateinit var tvScreenMirrorStatus: TextView
    private lateinit var tvMirrorConfig: TextView
    private lateinit var tvVideoEncoderInfo: TextView

    private val settingsStore by lazy { AppSettingsStore(this) }
    private val mediaProjectionManager by lazy {
        getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    }

    private val prefsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
        renderUi()
    }

    private val screenCaptureLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK && result.data != null) {
            settingsStore.setScreenMirrorEnabled(true)
            settingsStore.setScreenMirrorSummary("画面传输：正在启动")
            renderUi()
            ContextCompat.startForegroundService(
                this,
                Intent(this, ScreenMirrorService::class.java).apply {
                    action = ScreenMirrorService.ACTION_START_SCREEN_MIRROR
                    putExtra(ScreenMirrorService.EXTRA_SCREEN_RESULT_CODE, result.resultCode)
                    putExtra(ScreenMirrorService.EXTRA_SCREEN_RESULT_DATA, result.data)
                }
            )
        } else {
            settingsStore.setScreenMirrorEnabled(false)
            settingsStore.setScreenMirrorSummary("画面传输：未授权")
            renderUi()
        }
    }

    private val recordAudioPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) {
            settingsStore.setScreenMirrorSummary("画面传输：音频权限未授权，只启动画面")
            renderUi()
        }
        launchScreenCaptureIntent()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        btnStartScreenMirror = findViewById(R.id.btnStartScreenMirror)
        btnStopScreenMirror = findViewById(R.id.btnStopScreenMirror)
        tvScreenMirrorStatus = findViewById(R.id.tvScreenMirrorStatus)
        tvMirrorConfig = findViewById(R.id.tvMirrorConfig)
        tvVideoEncoderInfo = TextView(this).apply {
            textSize = 12f
            setTextIsSelectable(true)
            setPadding(tvMirrorConfig.paddingLeft, 10, tvMirrorConfig.paddingRight, 0)
        }
        (tvMirrorConfig.parent as? ViewGroup)?.let { parent ->
            val insertAt = parent.indexOfChild(tvMirrorConfig).let { if (it >= 0) it + 1 else parent.childCount }
            parent.addView(tvVideoEncoderInfo, insertAt)
        }

        btnStartScreenMirror.setOnClickListener {
            if (ScreenMirrorService.isRunning()) return@setOnClickListener
            requestStartScreenMirror()
        }
        btnStopScreenMirror.setOnClickListener {
            stopScreenMirror(updateSummary = true)
        }

        settingsStore.registerListener(prefsListener)
        syncStaleRunningState()
        renderUi()
    }

    override fun onDestroy() {
        settingsStore.unregisterListener(prefsListener)
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        syncStaleRunningState()
        renderUi()
    }

    private fun requestStartScreenMirror() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED
        ) {
            settingsStore.setScreenMirrorSummary("画面传输：等待音频权限")
            renderUi()
            recordAudioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
            return
        }
        launchScreenCaptureIntent()
    }

    private fun launchScreenCaptureIntent() {
        settingsStore.setScreenMirrorSummary("画面传输：等待系统授权")
        renderUi()
        screenCaptureLauncher.launch(mediaProjectionManager.createScreenCaptureIntent())
    }

    private fun stopScreenMirror(updateSummary: Boolean) {
        settingsStore.setScreenMirrorEnabled(false)
        if (updateSummary) {
            settingsStore.setScreenMirrorSummary("画面传输：已停止")
        }
        startService(Intent(this, ScreenMirrorService::class.java).apply {
            action = ScreenMirrorService.ACTION_STOP_SCREEN_MIRROR
        })
        renderUi()
    }

    private fun syncStaleRunningState() {
        if (ScreenMirrorService.isRunning()) return
        val snapshot = settingsStore.read()
        if (snapshot.screenMirrorEnabled) {
            settingsStore.setScreenMirrorEnabled(false)
            if (
                snapshot.screenMirrorSummary.contains("正在") ||
                snapshot.screenMirrorSummary.contains("等待 PC") ||
                snapshot.screenMirrorSummary.contains("PC已") ||
                snapshot.screenMirrorSummary.contains("PC 已")
            ) {
                settingsStore.setScreenMirrorSummary("画面传输：未启动")
            }
        }
    }

    private fun renderUi() {
        val snapshot = settingsStore.read()
        val running = ScreenMirrorService.isRunning()
        btnStartScreenMirror.isEnabled = !running
        btnStopScreenMirror.isEnabled = running
        tvScreenMirrorStatus.text = snapshot.screenMirrorSummary
        tvMirrorConfig.text = if (running) {
            "电脑端控制台可实时调整画面参数"
        } else {
            "连接电脑端后由控制台同步画面参数"
        }
        tvVideoEncoderInfo.text = AvcEncoderInspector.inspect().toMainUiText()
    }
}
