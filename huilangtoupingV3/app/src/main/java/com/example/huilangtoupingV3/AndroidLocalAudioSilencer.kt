package com.example.huilangtoupingV3

import android.content.Context
import android.media.AudioManager
import android.os.Build
import android.util.Log

/**
 * During PC mirroring we capture Android playback audio and play it on the PC.
 * AudioPlaybackCapture only copies playback; it does not reroute/mute the phone speaker.
 *
 * This helper temporarily lowers STREAM_MUSIC on the Android device so local speakers
 * do not continue playing while the PC is already playing the captured audio.
 */
class AndroidLocalAudioSilencer(context: Context) {
    companion object {
        private const val TAG = "AndroidAudioSilencer"
    }

    private val audioManager =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private var enabled = false
    private var savedMusicVolume = -1

    fun enable() {
        if (enabled) return
        enabled = true

        runCatching {
            if (audioManager.isVolumeFixed) {
                Log.w(TAG, "device volume is fixed; cannot silence local speaker")
                return
            }

            savedMusicVolume = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
            val minVolume = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                audioManager.getStreamMinVolume(AudioManager.STREAM_MUSIC)
            } else {
                0
            }

            audioManager.setStreamVolume(
                AudioManager.STREAM_MUSIC,
                minVolume,
                AudioManager.FLAG_REMOVE_SOUND_AND_VIBRATE
            )
            Log.i(TAG, "local STREAM_MUSIC silenced old=$savedMusicVolume min=$minVolume")
        }.onFailure {
            Log.e(TAG, "silence local audio failed", it)
        }
    }

    fun disable() {
        if (!enabled) return
        enabled = false

        val restore = savedMusicVolume
        savedMusicVolume = -1
        if (restore < 0) return

        runCatching {
            if (audioManager.isVolumeFixed) return

            val maxVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val target = restore.coerceIn(0, maxVolume)
            audioManager.setStreamVolume(
                AudioManager.STREAM_MUSIC,
                target,
                AudioManager.FLAG_REMOVE_SOUND_AND_VIBRATE
            )
            Log.i(TAG, "local STREAM_MUSIC restored volume=$target")
        }.onFailure {
            Log.e(TAG, "restore local audio failed", it)
        }
    }
}
