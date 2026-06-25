package com.example.huilangtoupingV3

import android.annotation.SuppressLint
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Build
import android.os.Process
import android.util.Log
import java.io.BufferedOutputStream
import kotlin.math.max

/**
 * Android -> PC audio path.
 *
 * Protocol over localabstract:huilang_screen_audio:
 *   repeat {
 *     4 bytes big-endian payload length
 *     PCM payload: 48kHz, stereo, signed 16-bit little-endian
 *   }
 *
 * Keep this separate from the JPEG video socket so video latency/debug logic stays unchanged.
 */
class AudioPlaybackCaptureSender(
    private val mediaProjection: MediaProjection,
    private val socketName: String = LOCAL_SOCKET_NAME,
) {
    companion object {
        const val LOCAL_SOCKET_NAME = "huilang_screen_audio"
        const val SAMPLE_RATE = 48_000
        const val CHANNEL_COUNT = 2
        const val BYTES_PER_SAMPLE = 2
        const val PCM_ENCODING = AudioFormat.ENCODING_PCM_16BIT
        private const val TAG = "AudioPlaybackSender"
    }

    private val lock = Object()

    @Volatile
    private var closed = false

    @Volatile
    var isConnected: Boolean = false
        private set

    private var worker: Thread? = null
    private var server: LocalServerSocket? = null
    private var client: LocalSocket? = null
    private var audioRecord: AudioRecord? = null

    fun start() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Log.w(TAG, "AudioPlaybackCapture requires Android 10+")
            return
        }
        synchronized(lock) {
            if (worker != null) return
            closed = false
            worker = Thread({ runLoop() }, "AudioPlaybackCaptureSender").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun close() {
        val thread = synchronized(lock) {
            if (closed) return
            closed = true
            runCatching { audioRecord?.stop() }
            runCatching { audioRecord?.release() }
            audioRecord = null
            isConnected = false
            runCatching { client?.close() }
            runCatching { server?.close() }
            client = null
            server = null
            worker.also { worker = null }
        }
        runCatching { thread?.join(700) }
    }

    private fun runLoop() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return

        try {
            try {
                server = LocalServerSocket(socketName)
            } catch (t: Throwable) {
                Log.e(TAG, "create LocalServerSocket failed name=$socketName", t)
                return
            }

            while (!closed) {
                var accepted: LocalSocket? = null
                try {
                    isConnected = false
                    runCatching { client?.close() }
                    client = null

                    accepted = server?.accept() ?: break
                    if (closed) break

                    client = accepted
                    accepted.sendBufferSize = 256 * 1024
                    accepted.receiveBufferSize = 16 * 1024
                    isConnected = true

                    streamPcmToPc(accepted)
                } catch (t: Throwable) {
                    if (!closed) {
                        Log.e(TAG, "audio socket loop failed", t)
                        Thread.sleep(200)
                    }
                } finally {
                    isConnected = false
                    runCatching { audioRecord?.stop() }
                    runCatching { audioRecord?.release() }
                    audioRecord = null
                    runCatching { accepted?.close() }
                    runCatching { client?.close() }
                    client = null
                }
            }
        } finally {
            isConnected = false
            runCatching { audioRecord?.stop() }
            runCatching { audioRecord?.release() }
            audioRecord = null
            runCatching { client?.close() }
            runCatching { server?.close() }
            client = null
            server = null
        }
    }

    @SuppressLint("MissingPermission")
    private fun buildAudioRecord(): AudioRecord? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null

        val channelMask = AudioFormat.CHANNEL_IN_STEREO
        val minBuffer = AudioRecord.getMinBufferSize(SAMPLE_RATE, channelMask, PCM_ENCODING)
        if (minBuffer <= 0) {
            Log.e(TAG, "invalid minBuffer=$minBuffer")
            return null
        }

        // 50ms PCM buffer minimum; larger internal buffer reduces underrun without adding socket latency.
        val fiftyMsBytes = SAMPLE_RATE * CHANNEL_COUNT * BYTES_PER_SAMPLE / 20
        val recordBufferBytes = max(minBuffer, fiftyMsBytes) * 4

        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val format = AudioFormat.Builder()
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(channelMask)
            .setEncoding(PCM_ENCODING)
            .build()

        val record = AudioRecord.Builder()
            .setAudioFormat(format)
            .setBufferSizeInBytes(recordBufferBytes)
            .setAudioPlaybackCaptureConfig(captureConfig)
            .build()

        if (record.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord not initialized state=${record.state}")
            runCatching { record.release() }
            return null
        }
        return record
    }

    private fun streamPcmToPc(socket: LocalSocket) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return

        val record = buildAudioRecord() ?: return
        audioRecord = record

        val out = BufferedOutputStream(socket.outputStream, 64 * 1024)
        val pcmBuffer = ByteArray(4096)

        try {
            runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO) }
            record.startRecording()

            while (!closed && isConnected) {
                val n = record.read(pcmBuffer, 0, pcmBuffer.size)
                if (n > 0) {
                    writeI32BE(out, n)
                    out.write(pcmBuffer, 0, n)
                    out.flush()
                } else if (n < 0) {
                    Log.e(TAG, "AudioRecord.read failed code=$n")
                    break
                }
            }
        } finally {
            runCatching { record.stop() }
            runCatching { record.release() }
            if (audioRecord === record) audioRecord = null
        }
    }

    private fun writeI32BE(out: BufferedOutputStream, value: Int) {
        out.write((value ushr 24) and 0xFF)
        out.write((value ushr 16) and 0xFF)
        out.write((value ushr 8) and 0xFF)
        out.write(value and 0xFF)
    }
}
